//! Shadow dry-run for the path-identity migration (Tier 1).
//!
//! Runs `classify` over every path-bearing column of a real DB copy
//! WITHOUT mutating it (the DB is opened read-only), and reports what
//! Tier 2's migration would do:
//!   * Volume vs Native counts per column,
//!   * the `native:` count in SYNCED tables — rows Tier 2 must
//!     quarantine rather than drop (Risk R4),
//!   * a dedup preview — how many duplicate rows collapse once the
//!     three legacy path forms map to one `vol:` identity.
//!
//! Once this is clean on several real DBs it tightens into the Tier 2
//! gate (residue assertions). For now it only reports.
//!
//! Local-only. Copy your live DB next to the harness path, then:
//!   cargo test -p ufb-core --test classify_shadow_dryrun -- --ignored --nocapture

use rusqlite::{Connection, OpenFlags};
use std::collections::HashMap;
use ufb_core::identity::{classify, Identity, VolumeView};
use ufb_core::settings::AppSettings;
use ufb_core::volumes::view_from_path_mappings;

/// Shared with `normalize_paths_dryrun` — copy the live DB here.
fn test_db_path() -> std::path::PathBuf {
    std::env::temp_dir().join("ufb-test-migration.db")
}

/// (table, column, is_synced, is_unique).
/// `is_synced` tables must hold `vol:` identities only — a `native:`
/// classification there is an orphan. `is_unique` marks columns with a
/// `UNIQUE`/PK constraint on that single column: only there does a
/// repeated identity mean a real collision Tier 2 must collapse;
/// elsewhere (e.g. many items per job) repeats are expected.
const PATH_COLUMNS: &[(&str, &str, bool, bool)] = &[
    ("subscriptions", "job_path", true, true),
    ("item_metadata", "job_path", true, false),
    ("item_metadata", "item_path", true, true),
    ("column_definitions", "job_path", true, false),
    ("column_layout_personal", "job_path", true, false),
    ("bookmarks", "path", false, true),
];

fn read_column(conn: &Connection, table: &str, column: &str) -> Vec<String> {
    let mut out = Vec::new();
    if let Ok(mut stmt) =
        conn.prepare(&format!("SELECT {column} FROM {table}"))
    {
        if let Ok(rows) = stmt.query_map([], |r| r.get::<_, String>(0)) {
            out.extend(rows.flatten());
        }
    }
    out
}

#[test]
#[ignore = "local dry-run; copy live DB to <temp>/ufb-test-migration.db, run with --ignored --nocapture"]
fn shadow_classify_real_db() {
    let path = test_db_path();
    if !path.exists() {
        eprintln!("skipping: {} not present", path.display());
        return;
    }
    let conn =
        Connection::open_with_flags(&path, OpenFlags::SQLITE_OPEN_READ_ONLY)
            .expect("open test DB read-only");

    let view: VolumeView =
        view_from_path_mappings(&AppSettings::load().path_mappings);
    assert!(
        !view.is_empty(),
        "no path mappings configured — the shadow run needs them to classify"
    );

    let mut total_volume = 0usize;
    let mut total_native = 0usize;
    let mut synced_native = 0usize;
    let mut total_collapses = 0usize;

    for (table, column, is_synced, is_unique) in PATH_COLUMNS {
        let values = read_column(&conn, table, column);
        if values.is_empty() {
            continue;
        }
        let mut vol = 0usize;
        let mut nat = 0usize;
        let mut groups: HashMap<String, usize> = HashMap::new();
        for v in &values {
            let id = classify(v, &view);
            match &id {
                Identity::Volume { .. } => vol += 1,
                Identity::Native { .. } => {
                    nat += 1;
                    if *is_synced {
                        synced_native += 1;
                        eprintln!("  [synced-native] {table}.{column} = {v:?}");
                    }
                }
            }
            *groups.entry(id.to_storage()).or_insert(0) += 1;
        }
        total_volume += vol;
        total_native += nat;
        if *is_unique {
            // A repeated identity on a UNIQUE column is a real
            // collision Tier 2's dedup collapses.
            let collapses: usize =
                groups.values().filter(|&&n| n > 1).map(|&n| n - 1).sum();
            total_collapses += collapses;
            eprintln!(
                "{table}.{column} (UNIQUE): {} rows -> {vol} vol, {nat} native; \
                 {collapses} duplicate row(s) collapse",
                values.len()
            );
        } else {
            // Non-unique column — repeats are expected (many rows per
            // job); just report how many distinct identities remain.
            eprintln!(
                "{table}.{column}: {} rows -> {vol} vol, {nat} native; \
                 {} distinct identities",
                values.len(),
                groups.len()
            );
        }
    }

    eprintln!(
        "\nTOTAL: {total_volume} vol, {total_native} native, \
         {total_collapses} collapsing dup(s); \
         {synced_native} native row(s) in SYNCED tables (Tier 2 quarantines these)"
    );
}
