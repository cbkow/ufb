//! LucidLink integration (2026-10-23): the Finder / Explorer extension's
//! "pin locally" rows, reproduced through LucidLink's own command-line
//! client (`lucid3`, falling back to `lucid`). The extension's menu rows
//! themselves cannot be hosted by another app; the CLI can do the same
//! work directly, on both OSes.
//!
//! Model: `lucid3 list` names the linked filespace instances; each
//! instance's `status` reports its mount point; a local path is on a
//! filespace when it lies under a mount point, and the CLI addresses it
//! by the path relative to the filespace root. `pin --list / --set /
//! --unset` keep or drop a local copy of a file or folder.
//!
//! Links (the extension's "Copy link" / "Create guest link"): each daemon
//! instance serves a local HTTP API on the port `list` reports.
//! `GET /fsEntry/direct-link?path=<rel>` answers `{"result": "https://
//! app.lucidlink.com/l/…"}` — the link other members of the filespace
//! open; `GET /fsEntry?path=<rel>` gives the entry id, and
//! `lucidlink://create-guest-link?wsId=&fsId=&fsEntryId=` (workspace and
//! filespace ids from `status`) opens the desktop app's guest-link
//! sheet, exactly as the extension does.
//!
//! Everything shells out, so results are cached: the instance table for
//! 30 s (mounts rarely change), the pin list for 3 s (and dropped after a
//! pin / unpin). A machine without the CLI answers "not available" from a
//! single cached probe.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

#[derive(Clone, Debug)]
pub struct Instance {
    pub id: String,
    pub name: String,
    pub mount_point: PathBuf,
    /// The instance's local REST port (the PORT column of `lucid3 list`).
    pub port: u16,
    pub workspace_id: String,
    pub filespace_id: String,
}

/// What the context menu needs to know about a selection, in one call.
#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct SelectionState {
    pub available: bool,
    pub on_filespace: bool,
    pub filespace: String,
    pub pinned_all: bool,
    pub pinned_any: bool,
}

fn cli() -> Option<&'static PathBuf> {
    static CLI: OnceLock<Option<PathBuf>> = OnceLock::new();
    CLI.get_or_init(find_cli).as_ref()
}

fn find_cli() -> Option<PathBuf> {
    let names: &[&str] = if cfg!(windows) { &["lucid3.exe", "lucid.exe"] } else { &["lucid3", "lucid"] };
    if let Some(path) = std::env::var_os("PATH") {
        for dir in std::env::split_paths(&path) {
            for n in names {
                let p = dir.join(n);
                if p.is_file() {
                    return Some(p);
                }
            }
        }
    }
    let mut candidates: Vec<PathBuf> = Vec::new();
    if cfg!(windows) {
        for var in ["ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"] {
            if let Some(base) = std::env::var_os(var) {
                let base = PathBuf::from(base);
                // <Program Files>\Lucid*\**\lucid3.exe, two levels deep.
                if let Ok(rd) = std::fs::read_dir(&base) {
                    for e in rd.flatten() {
                        let name = e.file_name().to_string_lossy().to_lowercase();
                        if !name.starts_with("lucid") {
                            continue;
                        }
                        for n in names {
                            candidates.push(e.path().join(n));
                        }
                        if let Ok(rd2) = std::fs::read_dir(e.path()) {
                            for e2 in rd2.flatten() {
                                for n in names {
                                    candidates.push(e2.path().join(n));
                                }
                            }
                        }
                    }
                }
            }
        }
    } else {
        for n in names {
            candidates.push(PathBuf::from("/usr/local/bin").join(n));
            candidates.push(PathBuf::from("/opt/homebrew/bin").join(n));
            candidates.push(PathBuf::from("/Applications/LucidLink.app/Contents/Resources").join(n));
        }
    }
    let found = candidates.into_iter().find(|p| p.is_file());
    match &found {
        Some(p) => log::info!("lucidlink: CLI at {}", p.display()),
        None => log::info!("lucidlink: CLI not found — LucidLink rows hidden"),
    }
    found
}

pub fn available() -> bool {
    cli().is_some()
}

fn run(args: &[&str]) -> Result<String, String> {
    let exe = cli().ok_or_else(|| "LucidLink CLI not found".to_string())?;
    let mut cmd = Command::new(exe);
    cmd.args(args);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let out = cmd.output().map_err(|e| format!("{}: {}", exe.display(), e))?;
    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let stderr = String::from_utf8_lossy(&out.stderr).to_string();
    if out.status.success() {
        Ok(stdout)
    } else {
        let msg = if stderr.trim().is_empty() { stdout } else { stderr };
        Err(msg.trim().to_string())
    }
}

struct Cache {
    instances: Option<(Instant, Vec<Instance>)>,
    pins: Option<(Instant, String, Vec<String>)>, // (when, instance id, entries)
}

fn cache() -> &'static Mutex<Cache> {
    static C: OnceLock<Mutex<Cache>> = OnceLock::new();
    C.get_or_init(|| Mutex::new(Cache { instances: None, pins: None }))
}

const INSTANCES_TTL: Duration = Duration::from_secs(30);
const PINS_TTL: Duration = Duration::from_secs(3);

/// (instance id, REST port) rows of `lucid3 list`: "INSTANCE ID
/// FILESPACE  PORT  MODE" header, then one row per instance.
fn parse_instances(list: &str) -> Vec<(String, u16)> {
    list.lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with("INSTANCE"))
        .filter_map(|l| {
            let cols: Vec<&str> = l.split_whitespace().collect();
            let id = cols.first()?.to_string();
            if !id.chars().all(|c| c.is_ascii_digit()) {
                return None;
            }
            let port = cols.get(2).and_then(|p| p.parse::<u16>().ok()).unwrap_or(0);
            Some((id, port))
        })
        .collect()
}

fn status_field<'a>(status: &'a str, key: &str) -> Option<&'a str> {
    status
        .lines()
        .find_map(|l| l.strip_prefix(key).map(str::trim))
        .filter(|v| !v.is_empty())
}

fn normalize_mount(s: &str) -> PathBuf {
    let trimmed = s.trim_end_matches(['/', '\\']);
    PathBuf::from(if trimmed.is_empty() { s } else { trimmed })
}

pub fn instances() -> Vec<Instance> {
    if !available() {
        return Vec::new();
    }
    {
        let c = cache().lock().unwrap();
        if let Some((t, v)) = &c.instances {
            if t.elapsed() < INSTANCES_TTL {
                return v.clone();
            }
        }
    }
    let mut out = Vec::new();
    match run(&["list"]) {
        Ok(list) => {
            for (id, port) in parse_instances(&list) {
                match run(&["--id", &id, "status"]) {
                    Ok(st) => {
                        let mount = status_field(&st, "Mount point:");
                        let name = status_field(&st, "Filespace name:").unwrap_or("").to_string();
                        let workspace_id = status_field(&st, "Workspace id:").unwrap_or("").to_string();
                        let filespace_id = status_field(&st, "Filespace id:").unwrap_or("").to_string();
                        if let Some(m) = mount {
                            out.push(Instance {
                                id: id.clone(),
                                name,
                                mount_point: normalize_mount(m),
                                port,
                                workspace_id,
                                filespace_id,
                            });
                        }
                    }
                    Err(e) => log::warn!("lucidlink: status for instance {}: {}", id, e),
                }
            }
        }
        Err(e) => log::warn!("lucidlink: list: {}", e),
    }
    cache().lock().unwrap().instances = Some((Instant::now(), out.clone()));
    out
}

fn same_component(a: &str, b: &str) -> bool {
    if cfg!(windows) { a.eq_ignore_ascii_case(b) } else { a == b }
}

/// The instance whose mount point contains `path`, and the path relative
/// to the filespace root (the CLI's addressing), using the OS separator.
pub fn locate(path: &str) -> Option<(Instance, String)> {
    let p = Path::new(path);
    let comps: Vec<String> = p.components().map(|c| c.as_os_str().to_string_lossy().to_string()).collect();
    let mut best: Option<(Instance, String)> = None;
    for inst in instances() {
        let mcomps: Vec<String> = inst
            .mount_point
            .components()
            .map(|c| c.as_os_str().to_string_lossy().to_string())
            .collect();
        if mcomps.len() > comps.len() {
            continue;
        }
        if !mcomps.iter().zip(comps.iter()).all(|(a, b)| same_component(a, b)) {
            continue;
        }
        // Drop a RootDir component ("\\" after a bare "L:" mount point on
        // Windows): the CLI addresses entries from the filespace root with
        // no leading separator.
        let rel: Vec<&str> = comps[mcomps.len()..]
            .iter()
            .map(String::as_str)
            .filter(|c| *c != "/" && *c != "\\")
            .collect();
        let rel = rel.join(std::path::MAIN_SEPARATOR_STR);
        if best.as_ref().map_or(true, |(b, _)| mcomps.len() > b.mount_point.components().count()) {
            best = Some((inst, rel));
        }
    }
    best
}

/// `pin --list` prints "PATH  STATE" and one row per entry, the path padded
/// to the column and a one-word state ("Pinned") after it. Paths may hold
/// spaces, so the state is the last whitespace-separated word, not a split.
fn parse_pins(list: &str) -> Vec<String> {
    list.lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .filter(|l| !l.starts_with("There are currently no pinned"))
        .filter(|l| !l.starts_with("PATH"))
        .filter_map(|l| {
            let cut = l.rfind(char::is_whitespace)?;
            let (path, state) = (l[..cut].trim_end(), l[cut..].trim());
            if path.is_empty() || !state.chars().all(char::is_alphabetic) {
                return None;
            }
            Some(path.trim_start_matches(['/', '\\']).trim_end_matches(['/', '\\']).to_string())
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pin_list_rows() {
        let out = "\nPATH                                          STATE        \n261327_TheClimb/audition/projects/.gitkeep    Pinned       \nfolder with spaces/clip.mov    Pinned\n";
        assert_eq!(parse_pins(out), vec!["261327_TheClimb/audition/projects/.gitkeep", "folder with spaces/clip.mov"]);
        assert!(parse_pins("There are currently no pinned entries.").is_empty());
    }

    #[test]
    fn list_rows_carry_ports() {
        let out = "\nINSTANCE ID        FILESPACE                      PORT        MODE        \n2000               union-jobs.union-ny-gfx        9778        live        \n";
        assert_eq!(parse_instances(out), vec![("2000".to_string(), 9778)]);
    }

    #[test]
    fn query_encoding() {
        assert_eq!(pct("a b/c.mov"), "a%20b%2Fc.mov");
        assert_eq!(rest_rel("job\\ae"), "job/ae");
    }

    #[test]
    fn pinned_by_prefix() {
        let pins = vec!["job/ae".to_string()];
        assert!(is_pinned("job/ae", &pins));
        assert!(is_pinned("job/ae/comp.aep", &pins));
        assert!(!is_pinned("job/aerial", &pins));
    }
}

/// Pinned entries (filespace-relative, no leading separator) for an instance.
pub fn pinned(inst: &Instance) -> Vec<String> {
    {
        let c = cache().lock().unwrap();
        if let Some((t, id, v)) = &c.pins {
            if id == &inst.id && t.elapsed() < PINS_TTL {
                return v.clone();
            }
        }
    }
    let v = match run(&["--id", &inst.id, "pin", "--list"]) {
        Ok(s) => parse_pins(&s),
        Err(e) => {
            log::warn!("lucidlink: pin --list: {}", e);
            Vec::new()
        }
    };
    cache().lock().unwrap().pins = Some((Instant::now(), inst.id.clone(), v.clone()));
    v
}

fn is_pinned(rel: &str, pins: &[String]) -> bool {
    let norm = |s: &str| s.replace('\\', "/");
    let r = norm(rel);
    pins.iter().any(|p| {
        let p = norm(p);
        same_component(&r, &p) || (r.len() > p.len() && same_component(&r[..p.len()], &p) && r[p.len()..].starts_with('/'))
    })
}

pub fn selection_state(paths: &[String]) -> SelectionState {
    let mut st = SelectionState { available: available(), ..Default::default() };
    if !st.available || paths.is_empty() {
        return st;
    }
    let located: Vec<(Instance, String)> = paths.iter().filter_map(|p| locate(p)).collect();
    if located.len() != paths.len() || located.iter().any(|(_, rel)| rel.is_empty()) {
        return st; // the whole selection must be on a filespace, and never its root
    }
    let inst = &located[0].0;
    if !located.iter().all(|(i, _)| i.id == inst.id) {
        return st;
    }
    st.on_filespace = true;
    st.filespace = inst.name.clone();
    let pins = pinned(inst);
    let flags: Vec<bool> = located.iter().map(|(_, rel)| is_pinned(rel, &pins)).collect();
    st.pinned_all = flags.iter().all(|&b| b);
    st.pinned_any = flags.iter().any(|&b| b);
    st
}

// ── Links ────────────────────────────────────────────────────────────

/// Percent-encode for a query value (everything but the unreserved set).
fn pct(s: &str) -> String {
    let mut out = String::with_capacity(s.len() * 3);
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => out.push(b as char),
            _ => out.push_str(&format!("%{:02X}", b)),
        }
    }
    out
}

/// One HTTP/1.0 GET against the instance's local daemon (it answers with
/// Connection: Close). std only — the request is a loopback round trip.
fn local_get(port: u16, path_and_query: &str) -> Result<String, String> {
    use std::io::{Read, Write};
    use std::net::{SocketAddr, TcpStream};
    if port == 0 {
        return Err("LucidLink instance has no REST port".into());
    }
    let addr: SocketAddr = ([127, 0, 0, 1], port).into();
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(3))
        .map_err(|e| format!("LucidLink daemon on port {}: {}", port, e))?;
    let _ = s.set_read_timeout(Some(Duration::from_secs(5)));
    let _ = s.set_write_timeout(Some(Duration::from_secs(5)));
    s.write_all(format!("GET {} HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path_and_query).as_bytes())
        .map_err(|e| e.to_string())?;
    let mut raw = Vec::new();
    s.read_to_end(&mut raw).map_err(|e| e.to_string())?;
    let text = String::from_utf8_lossy(&raw).to_string();
    let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
    let status: u16 = head
        .split_whitespace()
        .nth(1)
        .and_then(|c| c.parse().ok())
        .unwrap_or(0);
    if (200..300).contains(&status) {
        Ok(body.to_string())
    } else {
        Err(format!("LucidLink API {} for {}: {}", status, path_and_query, body.trim()))
    }
}

/// The filespace-relative path as the REST API spells it (forward slashes).
fn rest_rel(rel: &str) -> String {
    rel.replace('\\', "/")
}

/// The share link other members of the filespace open ("Copy link").
pub fn direct_link(path: &str) -> Result<String, String> {
    let (inst, rel) = locate(path).ok_or_else(|| format!("{} is not on a LucidLink filespace", path))?;
    let body = local_get(inst.port, &format!("/fsEntry/direct-link?path={}", pct(&rest_rel(&rel))))?;
    let v: serde_json::Value = serde_json::from_str(&body).map_err(|e| format!("direct-link reply: {}", e))?;
    v.get("result")
        .and_then(|r| r.as_str())
        .map(str::to_string)
        .ok_or_else(|| format!("direct-link reply had no result: {}", body.trim()))
}

/// The `lucidlink://create-guest-link…` URL that opens the desktop app's
/// guest-link sheet for `path` (a public, expiring link).
pub fn guest_link_url(path: &str) -> Result<String, String> {
    let (inst, rel) = locate(path).ok_or_else(|| format!("{} is not on a LucidLink filespace", path))?;
    if inst.workspace_id.is_empty() || inst.filespace_id.is_empty() {
        return Err("LucidLink status reported no workspace / filespace id".into());
    }
    let body = local_get(inst.port, &format!("/fsEntry?path={}", pct(&rest_rel(&rel))))?;
    let v: serde_json::Value = serde_json::from_str(&body).map_err(|e| format!("fsEntry reply: {}", e))?;
    let id = v
        .get("id")
        .and_then(|r| r.as_str())
        .ok_or_else(|| format!("fsEntry reply had no id: {}", body.trim()))?;
    // Ids go in raw, as the extension passes them (UUIDs and "59:319531";
    // a colon is legal in a query and the app may not decode %3A).
    Ok(format!(
        "lucidlink://create-guest-link?wsId={}&fsId={}&fsEntryId={}",
        inst.workspace_id, inst.filespace_id, id
    ))
}

/// Open the desktop app's guest-link sheet for `path`.
pub fn create_guest_link(path: &str) -> Result<(), String> {
    let url = guest_link_url(path)?;
    opener::open(&url).map_err(|e| format!("Could not open LucidLink: {}", e))
}

/// Pin (`on`) or unpin every path. Stops at the first failure.
pub fn set_pinned(paths: &[String], on: bool) -> Result<String, String> {
    let mut n = 0;
    for p in paths {
        let (inst, rel) = locate(p).ok_or_else(|| format!("{} is not on a LucidLink filespace", p))?;
        let flag = if on { "--set" } else { "--unset" };
        run(&["--id", &inst.id, "pin", flag, &rel]).map_err(|e| format!("{}: {}", p, e))?;
        n += 1;
    }
    cache().lock().unwrap().pins = None;
    Ok(format!("{} {} item(s)", if on { "Pinned" } else { "Unpinned" }, n))
}
