//! `create_date_prefixed_note` must produce a document minNotes can open:
//! the v3 schema, a stamped doc_meta row, and one empty paragraph block
//! with a 26-char ULID id and the initial rank "V".

use rusqlite::Connection;

#[test]
fn note_is_a_real_minnotes_document() {
    let dir = std::env::temp_dir().join(format!("ufb-note-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();

    let path = ufb_core::file_ops::create_date_prefixed_note(dir.to_str().unwrap(), "test")
        .expect("note created");
    assert!(path.ends_with("_test.mndb"), "{path}");

    let conn = Connection::open(&path).unwrap();
    let tables: Vec<String> = conn
        .prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")
        .unwrap()
        .query_map([], |r| r.get(0))
        .unwrap()
        .map(|r| r.unwrap())
        .collect();
    for t in ["blocks", "doc_meta", "block_ink", "comment_threads", "comment_messages"] {
        assert!(tables.iter().any(|x| x == t), "missing table {t}: {tables:?}");
    }

    let (schema, app): (i64, String) = conn
        .query_row("SELECT schema_version, app_version FROM doc_meta WHERE id=1", [], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .unwrap();
    assert_eq!(schema, 3);
    assert!(app.starts_with("ufb "), "{app}");

    let (id, rank, ty, content): (String, String, String, String) = conn
        .query_row("SELECT id, rank, type, content FROM blocks", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?))
        })
        .unwrap();
    assert_eq!(id.len(), 26, "{id}");
    assert!(id.bytes().all(|b| b"0123456789ABCDEFGHJKMNPQRSTVWXYZ".contains(&b)), "{id}");
    assert_eq!(rank, "V");
    assert_eq!(ty, "paragraph");
    assert_eq!(content, "");

    // Plain rollback-journal file, no WAL sidecars left behind.
    assert!(!std::path::Path::new(&format!("{path}-wal")).exists());
    let journal: String = conn
        .query_row("PRAGMA journal_mode", [], |r| r.get(0))
        .unwrap();
    assert_ne!(journal.to_lowercase(), "wal");

    // A second note the same day takes the next letter.
    let path2 = ufb_core::file_ops::create_date_prefixed_note(dir.to_str().unwrap(), "test")
        .unwrap();
    assert_ne!(path, path2);
    let _ = std::fs::remove_dir_all(&dir);
}
