//! One-shot dry-run of `Database::normalize_job_paths` against a copy
//! of the user's local DB at `/tmp/ufb-test-migration.db`. Verifies the
//! v3 migration converges glenlivet rows to local form before we ship
//! and let the real migration run.
//!
//! Skip-on-CI: gated on the test DB existing. Local-only sanity check.

use rusqlite::Connection;
use ufb_core::db::Database;

#[test]
#[ignore = "local dry-run; run with `cargo test --test normalize_paths_dryrun -- --ignored --nocapture`"]
fn dryrun_migration_against_user_copy() {
    let path = std::env::temp_dir().join("ufb-test-migration.db");
    if !path.exists() {
        eprintln!("skipping: {} not present", path.display());
        return;
    }

    let conn = Connection::open(&path).expect("open test DB");
    let stats = Database::normalize_job_paths(&conn);
    eprintln!(
        "stats: column_defs={} meta_job={} meta_item={} subs={} bookmarks={}",
        stats.column_defs, stats.meta_job, stats.meta_item, stats.subs, stats.bookmarks
    );

    // Sanity: every glenlivet row should now be in mac-native form.
    let canon_subs: i64 = conn
        .query_row(
            "SELECT COUNT(*) FROM subscriptions WHERE job_path LIKE 'C:%'",
            [],
            |row| row.get(0),
        )
        .unwrap_or(-1);
    let canon_meta: i64 = conn
        .query_row(
            "SELECT COUNT(*) FROM item_metadata WHERE item_path LIKE 'C:%'",
            [],
            |row| row.get(0),
        )
        .unwrap_or(-1);
    let weird_cols: i64 = conn
        .query_row(
            "SELECT COUNT(*) FROM column_definitions WHERE job_path LIKE '\\Volumes%'",
            [],
            |row| row.get(0),
        )
        .unwrap_or(-1);
    eprintln!(
        "post-migration foreign-form residue — subs={} meta={} weird-cols={}",
        canon_subs, canon_meta, weird_cols
    );

    assert_eq!(canon_subs, 0, "subscriptions still in canonical form");
    assert_eq!(canon_meta, 0, "item_metadata.item_path still in canonical form");
    assert_eq!(weird_cols, 0, "column_definitions still in \\Volumes form");
}
