//! Tier 2.4 dry-run — runs the FULL `user_version` 3→4 migration
//! (backup + transaction + classify + VACUUM) against a copy of a real
//! DB and verifies the outcome end-to-end. Local-only, `#[ignore]`.
//!
//! Copy the live DB before running:
//!   copy %LOCALAPPDATA%\ufb\ufb_v2.db  %TEMP%\ufb-migration-v4-test.db
//!   cargo test -p ufb-core --test migration_v4_dryrun -- --ignored --nocapture

use rusqlite::Connection;
use ufb_core::db::Database;

fn test_db_path() -> std::path::PathBuf {
    std::env::temp_dir().join("ufb-migration-v4-test.db")
}

#[test]
#[ignore = "local dry-run; copy live DB to <temp>/ufb-migration-v4-test.db, run with --ignored --nocapture"]
fn migration_v4_against_real_db() {
    let path = test_db_path();
    if !path.exists() {
        eprintln!("skipping: {} not present", path.display());
        return;
    }

    // Run the real migration path.
    {
        let db = Database::open(&path).expect("open test DB");
        db.run_migrations().expect("run_migrations");
    }

    // Re-open and inspect the result.
    let conn = Connection::open(&path).expect("reopen test DB");

    let uv: i64 = conn
        .query_row("PRAGMA user_version", [], |r| r.get(0))
        .unwrap();
    eprintln!("user_version after migration = {uv}");
    assert_eq!(uv, 4, "migration must bump user_version to 4");

    // A backup file must have been written next to the DB.
    let backup_found = std::fs::read_dir(path.parent().unwrap())
        .unwrap()
        .flatten()
        .any(|e| {
            e.file_name()
                .to_string_lossy()
                .starts_with("ufb-migration-v4-test.db.pre-identity-")
        });
    assert!(backup_found, "pre-migration backup file not found");
    eprintln!("pre-migration backup file: present");

    // Every synced-table path column must now be a `vol:` identity —
    // `native:` rows were quarantined out.
    for (table, col) in [
        ("subscriptions", "job_path"),
        ("item_metadata", "job_path"),
        ("item_metadata", "item_path"),
        ("column_definitions", "job_path"),
        ("column_layout_personal", "job_path"),
    ] {
        let total: i64 = conn
            .query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |r| {
                r.get(0)
            })
            .unwrap_or(0);
        let bad: i64 = conn
            .query_row(
                &format!(
                    "SELECT COUNT(*) FROM {table} WHERE {col} NOT LIKE 'vol:%'"
                ),
                [],
                |r| r.get(0),
            )
            .unwrap_or(-1);
        eprintln!("{table}.{col}: {total} rows, {bad} not vol:");
        assert_eq!(bad, 0, "{table}.{col} still has non-vol: rows");
    }

    // subscriptions.job_path is UNIQUE — no duplicate logical jobs left.
    let sub_rows: i64 = conn
        .query_row("SELECT COUNT(*) FROM subscriptions", [], |r| r.get(0))
        .unwrap();
    let sub_distinct: i64 = conn
        .query_row(
            "SELECT COUNT(DISTINCT job_path) FROM subscriptions",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(
        sub_rows, sub_distinct,
        "duplicate subscription job_path remains after migration"
    );

    // The two formerly-stuck duplicates: each logical job appears once.
    for job in ["261301_pmkn", "000000_QCView_Test"] {
        let n: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM subscriptions WHERE job_path LIKE ?1",
                [format!("%/{job}")],
                |r| r.get(0),
            )
            .unwrap_or(-1);
        eprintln!("subscription rows for {job}: {n}");
        assert!(n <= 1, "{job} still duplicated: {n} rows");
    }

    let orphans: i64 = conn
        .query_row("SELECT COUNT(*) FROM migration_orphans", [], |r| r.get(0))
        .unwrap();
    eprintln!("migration_orphans: {orphans} rows");
    {
        let mut stmt = conn
            .prepare(
                "SELECT source_table, original_path FROM migration_orphans",
            )
            .unwrap();
        for row in stmt
            .query_map([], |r| {
                Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
            })
            .unwrap()
            .flatten()
        {
            eprintln!("  orphan: {} <- {}", row.0, row.1);
        }
    }

    eprintln!("\nmigration v4 dry-run OK");
}
