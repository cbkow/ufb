// Some helpers here (is_ignored_name, unix_now_f64, hydrated_size)
// currently have macOS-only callers; Windows builds see them as dead.
#![cfg_attr(windows, allow(dead_code))]

/// Shared cache helpers used by both macOS (`macos_cache`) and Windows
/// (`windows_cache`) sync backends.
///
/// Contains: constants, chunk-bitmap bit operations, path helpers,
/// SQLite type aliases, integrity checking, and the `CachedAttr` type
/// returned by cache-serving accessors.

use r2d2::{Pool, PooledConnection};
use r2d2_sqlite::SqliteConnectionManager;
use rusqlite::Connection;

pub const EVICTION_TARGET_PERCENT: f64 = 0.8;
/// Sized for burst fan-out: each NFS/WinFsp write makes several short
/// checkouts while enumerations and the evictor hold connections for
/// whole transactions. At 6, heavy Finder I/O could exhaust the pool —
/// r2d2 then blocks 30s and the `conn()` unwrap panics the handler.
pub const POOL_SIZE: u32 = 32;

/// Names excluded from NAS enumerations AND protected from orphan
/// pruning. Must stay symmetric: a class of name that never appears in
/// listings but can be created through the mount (Finder's `.DS_Store`
/// and `._*` AppleDouble sidecars) would otherwise be registered on
/// create and then reaped as an "orphan" by the next re-enumeration —
/// destroying its fh while the client still holds it (STALE mid-copy).
///
/// Matches exact NAS-appliance sentinels rather than any `@`/`#` prefix:
/// user folders like `@Media` or clips like `#2 take.mov` are real
/// content and must be visible.
#[inline]
pub fn is_ignored_name(name: &str) -> bool {
    name.starts_with('.')
        || name == "@eaDir"
        || name == "@Recycle"
        || name == "#recycle"
        || name == "#snapshot"
}

/// Content cache chunk size (1 MiB). Matches the NFS `rsize` mount option
/// on macOS and the ProjFS read-request granularity on Windows so each
/// platform's read callback maps cleanly to one chunk.
pub const CHUNK_SIZE: u64 = 1024 * 1024;

pub type SqlitePool = Pool<SqliteConnectionManager>;
pub type SqliteConn = PooledConnection<SqliteConnectionManager>;

// ── Chunk-bitmap bit operations ──
//
// Bits are packed LSB-first within each byte: chunk `i` lives in byte
// `i / 8`, bit `i % 8`.

#[inline]
pub fn num_chunks(size: u64) -> u64 {
    (size + CHUNK_SIZE - 1) / CHUNK_SIZE
}

#[inline]
pub fn bit_is_set(bitmap: &[u8], chunk: u64) -> bool {
    let byte = (chunk / 8) as usize;
    let mask = 1u8 << ((chunk % 8) as u8);
    bitmap.get(byte).map(|b| b & mask != 0).unwrap_or(false)
}

#[inline]
pub fn set_bit(bitmap: &mut Vec<u8>, chunk: u64) {
    let byte = (chunk / 8) as usize;
    let mask = 1u8 << ((chunk % 8) as u8);
    if bitmap.len() <= byte {
        bitmap.resize(byte + 1, 0);
    }
    bitmap[byte] |= mask;
}

#[inline]
pub fn bitmap_is_complete(bitmap: &[u8], total_chunks: u64) -> bool {
    if total_chunks == 0 {
        return true;
    }
    let full_bytes = (total_chunks / 8) as usize;
    for i in 0..full_bytes {
        if bitmap.get(i).copied().unwrap_or(0) != 0xFF {
            return false;
        }
    }
    let remainder = total_chunks % 8;
    if remainder > 0 {
        let mask = (1u8 << remainder) - 1;
        if bitmap.get(full_bytes).copied().unwrap_or(0) & mask != mask {
            return false;
        }
    }
    true
}

// ── Path helpers ──

/// Parent directory of a forward-slash-separated relative path.
/// `"a/b/c.txt"` → `"a/b"`. `"foo.txt"` → `""`.
#[inline]
pub fn parent_of(path: &str) -> &str {
    match path.rfind('/') {
        Some(i) => &path[..i],
        None => "",
    }
}

// ── Shared cache attribute type ──

/// Subset of `known_files` fields needed by the VFS provider layer
/// (NFS `fattr3` on macOS, ProjFS placeholder info on Windows).
#[derive(Debug, Clone)]
pub struct CachedAttr {
    pub is_dir: bool,
    pub size: u64,
    /// Seconds since Unix epoch (NAS mtime). Stored as `f64` for
    /// sub-second precision on platforms that support it.
    pub mtime: f64,
    /// Seconds since Unix epoch (NAS ctime / birth time).
    pub created: f64,
    pub is_hydrated: bool,
    pub hydrated_size: u64,
}

// ── SQLite helpers ──

/// Per-connection PRAGMAs applied on every pool checkout.
pub const PER_CONN_PRAGMAS: &str =
    "PRAGMA synchronous=NORMAL;\n\
     PRAGMA busy_timeout=5000;\n\
     PRAGMA foreign_keys=ON;";

/// One-time pragmas applied on the serial setup connection before the
/// pool opens (WAL is persistent on the DB file).
pub const INIT_PRAGMAS: &str =
    "PRAGMA journal_mode=WAL;\n\
     PRAGMA synchronous=NORMAL;";

/// `metadata` table DDL — key-value store for global state. Shared
/// across both platforms.
pub const METADATA_DDL: &str =
    "CREATE TABLE IF NOT EXISTS metadata (\n\
         key TEXT PRIMARY KEY,\n\
         value TEXT NOT NULL\n\
     );";

/// Check SQLite integrity. Returns `true` if the DB is healthy.
pub fn check_integrity(conn: &Connection) -> bool {
    conn.query_row("PRAGMA integrity_check", [], |row| row.get::<_, String>(0))
        .map(|result| result == "ok")
        .unwrap_or(false)
}

/// Current wall-clock time as seconds since Unix epoch.
pub fn unix_now_secs() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs() as i64
}

/// Current wall-clock time as fractional seconds since Unix epoch.
pub fn unix_now_f64() -> f64 {
    let d = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    d.as_secs() as f64 + d.subsec_nanos() as f64 / 1_000_000_000.0
}

/// Defensive pre-touch: ensure the parent dir exists and the DB file
/// exists (empty if missing). Workaround for a SQLite-on-Windows bug
/// where the CREATE codepath through the Win32 VFS fails with
/// "unable to open database" on paths whose resolution passes through
/// an 8.3 short name (e.g. `C:\Users\UNIONG~1\…`). The OPEN-EXISTING
/// branch handles such paths correctly, so we touch the file with
/// std::fs first and let SQLite take that branch.
///
/// Idempotent — no-op when the file already exists, which is the
/// common case after first launch.
pub fn ensure_db_file(db_path: &std::path::Path) {
    if let Some(parent) = db_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if !db_path.exists() {
        let _ = std::fs::OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(false)
            .open(db_path);
    }
}

/// Build a connection pool with shared per-connection pragmas.
pub fn build_pool(db_path: &std::path::Path) -> Result<SqlitePool, String> {
    ensure_db_file(db_path);
    let manager = SqliteConnectionManager::file(db_path).with_init(|c| {
        c.execute_batch(PER_CONN_PRAGMAS)
    });
    Pool::builder()
        .max_size(POOL_SIZE)
        .build(manager)
        .map_err(|e| format!("Failed to build SQLite pool: {}", e))
}
