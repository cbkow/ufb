//! Shared `Arc<Database>` for the bindings layer.
//!
//! All service singletons (Bookmarks, Thumbnails, Metadata, Columns, …)
//! point at the same SQLite file the legacy Tauri app used. Opening it
//! more than once is harmless but wastes a file handle and re-runs the
//! migration check; this OnceLock makes "open the DB" a single
//! operation regardless of which service asks first.

use std::sync::{Arc, OnceLock};
use ufb_core::db::Database;

/// Lazy singleton DB handle. `None` means the open or migration step
/// failed — see logs at startup. Callers should treat that as
/// "feature unavailable" and degrade gracefully.
pub fn shared_db() -> Option<Arc<Database>> {
    static DB: OnceLock<Option<Arc<Database>>> = OnceLock::new();
    DB.get_or_init(|| {
        let db_path = ufb_core::utils::get_database_path();
        match Database::open(&db_path) {
            Ok(db) => {
                let db = Arc::new(db);
                if let Err(e) = db.run_migrations() {
                    log::error!("db: migrations failed: {}", e);
                    return None;
                }
                log::info!("db: opened {:?}", db_path);
                Some(db)
            }
            Err(e) => {
                log::error!("db: failed to open {:?}: {}", db_path, e);
                None
            }
        }
    })
    .clone()
}
