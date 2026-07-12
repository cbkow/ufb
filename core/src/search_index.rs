//! Indexed ("alt") file-name search backend — Tantivy.
//!
//! Crawls a root tree (a local folder or a mounted network share) into a
//! persisted Tantivy index of file/folder **names + extensions** (no
//! content), giving Everything-style substring matching, and supports
//! live incremental updates from an FS watcher. This is the "alt" mode
//! paired with the OS default backend in [`crate::search`] (es.exe /
//! mdfind); the `Search` QObject in `bindings` picks one at a time.
//!
//! Design notes:
//! - **Substring matching** via an n-gram tokenizer (2–3) + lowercaser on
//!   a non-stored `terms` field. A query is tokenized the same way and its
//!   grams are AND-ed, so "name contains <query>" holds for queries ≥ 2
//!   chars (the UI gates shorter queries).
//! - **Stored fields** (`path`, `name`, `ext`, `is_dir`, `size`,
//!   `modified`) let a hit rebuild a [`FileEntry`] without re-stat'ing the
//!   disk, so results stay fast even over a slow network share.
//! - `path` is also indexed as a raw `STRING` so the watcher can
//!   `delete_term` a single entry for incremental upsert/remove.
//! - The crawl is **cancellable** (an `AtomicBool`) and commits in batches
//!   so results fill in live as the index builds.

use crate::file_ops::FileEntry;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::UNIX_EPOCH;

use ignore::WalkBuilder;
use tantivy::collector::TopDocs;
use tantivy::directory::MmapDirectory;
use tantivy::query::{BooleanQuery, Occur, Query, TermQuery};
use tantivy::schema::{
    Field, IndexRecordOption, Schema, TextFieldIndexing, TextOptions, Value, STORED, STRING,
};
use tantivy::tokenizer::{LowerCaser, NgramTokenizer, TextAnalyzer, Token, TokenStream};
use tantivy::{Index, IndexReader, IndexWriter, ReloadPolicy, TantivyDocument, Term};

/// Tantivy permits a single `IndexWriter` per index at a time (it holds
/// an on-disk lock). A re-entered root or an overlapping crawl could try
/// to open a second writer and fail with a lock error, so all index
/// *writes* (rebuild/upsert/remove) acquire this process-wide lock first.
/// Queries use the lock-free reader and are unaffected. A new crawl that
/// arrives mid-rebuild blocks here only briefly — the prior crawl's
/// cancel flag makes it bail and release fast.
static WRITE_LOCK: once_cell::sync::Lazy<std::sync::Mutex<()>> =
    once_cell::sync::Lazy::new(|| std::sync::Mutex::new(()));

const TOKENIZER_NAME: &str = "ufb_ngram";
const NGRAM_MIN: usize = 2;
const NGRAM_MAX: usize = 3;
const WRITER_HEAP_BYTES: usize = 64 * 1024 * 1024; // 64 MB
const COMMIT_EVERY: u64 = 4000; // batch commits so results fill in live

/// Per-root on-disk index location: `<app-data>/index/<md5(root)>`.
pub fn index_dir_for_root(root: &Path) -> PathBuf {
    let canon = dunce::canonicalize(root).unwrap_or_else(|_| root.to_path_buf());
    let key = format!("{:x}", md5::compute(canon.to_string_lossy().as_bytes()));
    crate::utils::get_app_data_dir().join("index").join(key)
}

struct Fields {
    terms: Field,
    path: Field,
    name: Field,
    ext: Field,
    is_dir: Field,
    size: Field,
    modified: Field,
}

/// A live, persisted Tantivy index over one crawl root.
pub struct SearchIndex {
    index: Index,
    reader: IndexReader,
    fields: Fields,
    root: PathBuf,
}

fn build_schema() -> (Schema, Fields) {
    let mut b = Schema::builder();

    // Substring-match field: n-gram tokenized + lowercased, not stored.
    let term_indexing = TextFieldIndexing::default()
        .set_tokenizer(TOKENIZER_NAME)
        .set_index_option(IndexRecordOption::Basic);
    let terms = b.add_text_field("terms", TextOptions::default().set_indexing_options(term_indexing));

    // Raw indexed + stored path (delete_term key + result payload).
    let path = b.add_text_field("path", STRING | STORED);
    // Stored-only payload fields.
    let name = b.add_text_field("name", STORED);
    let ext = b.add_text_field("ext", STORED);
    let is_dir = b.add_i64_field("is_dir", STORED);
    let size = b.add_i64_field("size", STORED);
    let modified = b.add_i64_field("modified", STORED);

    (
        b.build(),
        Fields { terms, path, name, ext, is_dir, size, modified },
    )
}

fn register_tokenizer(index: &Index) -> Result<(), String> {
    let ngram = NgramTokenizer::new(NGRAM_MIN, NGRAM_MAX, false)
        .map_err(|e| format!("ngram tokenizer: {e}"))?;
    let analyzer = TextAnalyzer::builder(ngram).filter(LowerCaser).build();
    index.tokenizers().register(TOKENIZER_NAME, analyzer);
    Ok(())
}

impl SearchIndex {
    /// Open (or create) the persisted index for `root`.
    pub fn open(root: &Path) -> Result<Self, String> {
        let dir = index_dir_for_root(root);
        std::fs::create_dir_all(&dir).map_err(|e| format!("create index dir: {e}"))?;
        let mmap = MmapDirectory::open(&dir).map_err(|e| format!("open index dir: {e}"))?;
        let (schema, fields) = build_schema();
        let index = Index::open_or_create(mmap, schema).map_err(|e| format!("open index: {e}"))?;
        register_tokenizer(&index)?;
        let reader = index
            .reader_builder()
            .reload_policy(ReloadPolicy::Manual)
            .try_into()
            .map_err(|e| format!("index reader: {e}"))?;
        Ok(Self { index, reader, fields, root: root.to_path_buf() })
    }

    /// Crawl the whole root tree and (re)build the index from scratch.
    /// `cancel` is polled each entry so a toggle-off / navigation aborts
    /// promptly; `on_progress` is called with the running document count
    /// after each batch commit so the UI can show indexing progress.
    pub fn rebuild(
        &self,
        cancel: &AtomicBool,
        mut on_progress: impl FnMut(u64),
    ) -> Result<(), String> {
        let _wlock = WRITE_LOCK.lock().map_err(|_| "write lock poisoned".to_string())?;
        let mut writer: IndexWriter = self
            .index
            .writer(WRITER_HEAP_BYTES)
            .map_err(|e| format!("index writer: {e}"))?;
        writer
            .delete_all_documents()
            .map_err(|e| format!("clear index: {e}"))?;

        let walker = WalkBuilder::new(&self.root)
            .hidden(false) // index hidden entries too (Everything-like)
            .ignore(false)
            .git_ignore(false)
            .git_global(false)
            .git_exclude(false)
            .parents(false)
            .follow_links(false)
            .build();

        let mut count: u64 = 0;
        for entry in walker {
            if cancel.load(Ordering::Relaxed) {
                // Abort: roll the partial batch into the index so what we
                // crawled so far is still queryable, then bail.
                let _ = writer.commit();
                let _ = self.reader.reload();
                return Ok(());
            }
            let entry = match entry {
                Ok(e) => e,
                Err(_) => continue, // permission denied / vanished — skip
            };
            // Skip the crawl root itself; index everything beneath it.
            if entry.depth() == 0 {
                continue;
            }
            if let Some(doc) = self.entry_doc(entry.path()) {
                writer.add_document(doc).map_err(|e| format!("add doc: {e}"))?;
                count += 1;
                if count % COMMIT_EVERY == 0 {
                    writer.commit().map_err(|e| format!("commit: {e}"))?;
                    let _ = self.reader.reload();
                    on_progress(count);
                }
            }
        }
        writer.commit().map_err(|e| format!("final commit: {e}"))?;
        self.reader.reload().map_err(|e| format!("reader reload: {e}"))?;
        on_progress(count);
        Ok(())
    }

    /// Incrementally upsert a single path (watcher: create / modify).
    pub fn upsert(&self, path: &Path) -> Result<(), String> {
        let _wlock = WRITE_LOCK.lock().map_err(|_| "write lock poisoned".to_string())?;
        let mut writer: IndexWriter = self
            .index
            .writer(WRITER_HEAP_BYTES)
            .map_err(|e| format!("index writer: {e}"))?;
        let key = Term::from_field_text(self.fields.path, &path.to_string_lossy());
        writer.delete_term(key);
        if let Some(doc) = self.entry_doc(path) {
            writer.add_document(doc).map_err(|e| format!("add doc: {e}"))?;
        }
        writer.commit().map_err(|e| format!("commit: {e}"))?;
        self.reader.reload().map_err(|e| format!("reload: {e}"))?;
        Ok(())
    }

    /// Incrementally remove a single path (watcher: delete / rename-away).
    pub fn remove(&self, path: &Path) -> Result<(), String> {
        let _wlock = WRITE_LOCK.lock().map_err(|_| "write lock poisoned".to_string())?;
        let mut writer: IndexWriter = self
            .index
            .writer(WRITER_HEAP_BYTES)
            .map_err(|e| format!("index writer: {e}"))?;
        let key = Term::from_field_text(self.fields.path, &path.to_string_lossy());
        writer.delete_term(key);
        writer.commit().map_err(|e| format!("commit: {e}"))?;
        self.reader.reload().map_err(|e| format!("reload: {e}"))?;
        Ok(())
    }

    /// Query the index for names containing `q` (≥ 2 chars). Returns up to
    /// `limit` hits as [`FileEntry`]s rebuilt from stored fields.
    pub fn query(&self, q: &str, limit: usize) -> Result<Vec<FileEntry>, String> {
        let grams = self.tokenize(q);
        if grams.is_empty() {
            return Ok(Vec::new());
        }
        let clauses: Vec<(Occur, Box<dyn Query>)> = grams
            .into_iter()
            .map(|g| {
                let t = Term::from_field_text(self.fields.terms, &g);
                let q: Box<dyn Query> = Box::new(TermQuery::new(t, IndexRecordOption::Basic));
                (Occur::Must, q)
            })
            .collect();
        let query = BooleanQuery::new(clauses);

        let searcher = self.reader.searcher();
        let hits = searcher
            .search(&query, &TopDocs::with_limit(limit))
            .map_err(|e| format!("search: {e}"))?;

        let mut out = Vec::with_capacity(hits.len());
        for (_score, addr) in hits {
            let doc: TantivyDocument = match searcher.doc(addr) {
                Ok(d) => d,
                Err(_) => continue,
            };
            if let Some(e) = self.doc_to_entry(&doc) {
                out.push(e);
            }
        }
        Ok(out)
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    // --- helpers ---------------------------------------------------------

    /// Tokenize a user query with the same n-gram analyzer used at index
    /// time, deduping grams. Empty for queries shorter than NGRAM_MIN.
    fn tokenize(&self, q: &str) -> Vec<String> {
        let analyzer = match self.index.tokenizers().get(TOKENIZER_NAME) {
            Some(a) => a,
            None => return Vec::new(),
        };
        let mut analyzer = analyzer;
        let mut stream = analyzer.token_stream(q);
        let mut grams: Vec<String> = Vec::new();
        let mut push = |t: &Token| {
            if !grams.iter().any(|g| g == &t.text) {
                grams.push(t.text.clone());
            }
        };
        stream.process(&mut push);
        grams
    }

    /// Build a stored Tantivy document from a filesystem path. Returns
    /// None if the path can't be stat'd or has no file name.
    fn entry_doc(&self, path: &Path) -> Option<TantivyDocument> {
        let name = path.file_name()?.to_string_lossy().to_string();
        let meta = std::fs::symlink_metadata(path).ok()?;
        let is_dir = meta.file_type().is_dir();
        let ext = path
            .extension()
            .map(|e| e.to_string_lossy().to_string())
            .unwrap_or_default();
        let size = meta.len();
        let modified = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| d.as_millis() as i64);

        let mut doc = TantivyDocument::default();
        // Index the name (incl. its extension) for substring matching.
        doc.add_text(self.fields.terms, &name);
        doc.add_text(self.fields.path, &path.to_string_lossy());
        doc.add_text(self.fields.name, &name);
        doc.add_text(self.fields.ext, &ext);
        doc.add_i64(self.fields.is_dir, if is_dir { 1 } else { 0 });
        doc.add_i64(self.fields.size, size as i64);
        if let Some(m) = modified {
            doc.add_i64(self.fields.modified, m);
        }
        Some(doc)
    }

    fn doc_to_entry(&self, doc: &TantivyDocument) -> Option<FileEntry> {
        let path = doc.get_first(self.fields.path)?.as_str()?.to_string();
        let name = doc
            .get_first(self.fields.name)
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let extension = doc
            .get_first(self.fields.ext)
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let is_dir = doc
            .get_first(self.fields.is_dir)
            .and_then(|v| v.as_i64())
            .unwrap_or(0)
            != 0;
        let size = doc
            .get_first(self.fields.size)
            .and_then(|v| v.as_i64())
            .unwrap_or(0) as u64;
        let modified = doc.get_first(self.fields.modified).and_then(|v| v.as_i64());
        Some(FileEntry { name, path, is_dir, size, modified, extension })
    }
}
