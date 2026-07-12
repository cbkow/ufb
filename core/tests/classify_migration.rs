//! Tier 2.1 — coverage for `Database::classify_all_path_columns`.
//!
//! Verifies the three behaviours against an in-memory DB:
//!   * convert  — a path under a volume is rewritten to `vol:…`,
//!   * dedup    — two legacy forms of one path collapse to one row,
//!   * orphan   — a synced-table row not under any volume is moved to
//!                `migration_orphans` (a bookmark instead converts in
//!                place, since `native:` is valid for bookmarks).

use ufb_core::db::Database;
use ufb_core::settings::PathMapping;
use ufb_core::volumes::{view_from_path_mappings, volume_uuid};

fn test_mapping() -> PathMapping {
    PathMapping {
        win: "C:\\Volumes\\share\\jobs".to_string(),
        mac: "/Volumes/share/jobs".to_string(),
        enabled: true,
        label: "test".to_string(),
    }
}

#[test]
fn migration_converts_dedups_and_quarantines() {
    let db = Database::open_in_memory().expect("in-memory DB");
    db.run_migrations().expect("migrations");

    let uuid = volume_uuid("C:\\Volumes\\share\\jobs", "/Volumes/share/jobs");
    let view = view_from_path_mappings(&[test_mapping()]);

    // Seed known rows: a dup pair, an orphan sub, an in-volume item, an
    // orphan item, and a personal (native) bookmark.
    db.with_conn(|conn| {
        conn.execute(
            "INSERT INTO subscriptions (job_path, job_name, subscribed_time, modified_time)
             VALUES (?1, 'J1', 0, 100)",
            ["C:\\Volumes\\share\\jobs\\J1"],
        )?;
        conn.execute(
            "INSERT INTO subscriptions (job_path, job_name, subscribed_time, modified_time)
             VALUES (?1, 'J1', 0, 200)",
            ["\\Volumes\\share\\jobs\\J1"],
        )?;
        conn.execute(
            "INSERT INTO subscriptions (job_path, job_name, subscribed_time, modified_time)
             VALUES (?1, 'local', 0, 0)",
            ["C:\\Local\\foo"],
        )?;
        conn.execute(
            "INSERT INTO item_metadata (item_path, job_path, folder_name, modified_time)
             VALUES (?1, ?2, 'ae', 0)",
            [
                "C:\\Volumes\\share\\jobs\\J1\\shot1",
                "C:\\Volumes\\share\\jobs\\J1",
            ],
        )?;
        conn.execute(
            "INSERT INTO item_metadata (item_path, job_path, folder_name, modified_time)
             VALUES (?1, ?2, 'ae', 0)",
            ["G:\\stray\\shot", "G:\\stray"],
        )?;
        conn.execute(
            "INSERT INTO bookmarks (path, display_name, created_time)
             VALUES (?1, 'Desktop', 0)",
            ["C:\\Users\\me\\Desktop"],
        )?;
        Ok(())
    })
    .unwrap();

    let stats = db
        .with_conn(|conn| Ok(Database::classify_all_path_columns(conn, &view)))
        .unwrap();

    assert_eq!(stats.converted, 3, "stats: {stats:?}");
    assert_eq!(stats.deduped, 1, "stats: {stats:?}");
    assert_eq!(stats.orphaned, 2, "stats: {stats:?}");

    db.with_conn(|conn| {
        // Subscriptions: dup collapsed to one tagged row, orphan gone.
        let sub_count: i64 =
            conn.query_row("SELECT COUNT(*) FROM subscriptions", [], |r| r.get(0))?;
        assert_eq!(sub_count, 1);
        let sub_path: String =
            conn.query_row("SELECT job_path FROM subscriptions", [], |r| r.get(0))?;
        assert_eq!(sub_path, format!("vol:{uuid}/J1"));

        // item_metadata: in-volume row converted, orphan removed.
        let item_count: i64 =
            conn.query_row("SELECT COUNT(*) FROM item_metadata", [], |r| r.get(0))?;
        assert_eq!(item_count, 1);
        let (ip, jp): (String, String) = conn.query_row(
            "SELECT item_path, job_path FROM item_metadata",
            [],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )?;
        assert_eq!(ip, format!("vol:{uuid}/J1/shot1"));
        assert_eq!(jp, format!("vol:{uuid}/J1"));

        // Bookmark: converted in place to a native: identity, not orphaned.
        let bm: String =
            conn.query_row("SELECT path FROM bookmarks", [], |r| r.get(0))?;
        assert!(bm.starts_with("native:"), "bookmark not converted: {bm}");

        // Two synced-table orphans quarantined.
        let orphans: i64 = conn
            .query_row("SELECT COUNT(*) FROM migration_orphans", [], |r| r.get(0))?;
        assert_eq!(orphans, 2);
        Ok(())
    })
    .unwrap();
}

#[test]
fn migration_is_idempotent() {
    // A second pass over already-tagged rows must be a no-op.
    let db = Database::open_in_memory().expect("in-memory DB");
    db.run_migrations().expect("migrations");
    let view = view_from_path_mappings(&[test_mapping()]);

    db.with_conn(|conn| {
        conn.execute(
            "INSERT INTO subscriptions (job_path, job_name, subscribed_time, modified_time)
             VALUES (?1, 'J1', 0, 100)",
            ["C:\\Volumes\\share\\jobs\\J1"],
        )?;
        Ok(())
    })
    .unwrap();

    let first = db
        .with_conn(|conn| Ok(Database::classify_all_path_columns(conn, &view)))
        .unwrap();
    assert_eq!(first.converted, 1);

    let second = db
        .with_conn(|conn| Ok(Database::classify_all_path_columns(conn, &view)))
        .unwrap();
    assert_eq!(second.converted, 0, "second pass should be a no-op");
    assert_eq!(second.orphaned, 0);
    assert_eq!(second.deduped, 0);
}
