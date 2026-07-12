//! Job folder helpers.
//!
//! `create_from_template` copies a template directory tree into a
//! parent jobs folder under a `{number}_{name}` subfolder. Pure I/O,
//! no Qt or settings dependencies — the bindings layer resolves
//! the template path (typically `<exe_dir>/templates/projectTemplate`)
//! and passes it in.

use std::path::{Path, PathBuf};

/// Sanitise a user-typed job-number/name to safe filename chars.
/// Strips characters that would break Windows filesystems and trims
/// surrounding whitespace.
fn sanitise(s: &str) -> String {
    s.chars()
        .filter(|c| !matches!(c, '\\' | '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|'))
        .collect::<String>()
        .trim()
        .to_string()
}

/// Format the new folder name from `{number}_{name}`. Whitespace in
/// the name is replaced with underscores to match the legacy app's
/// convention.
pub fn job_folder_name(number: &str, name: &str) -> String {
    let n = sanitise(number).replace(char::is_whitespace, "_");
    let nm = sanitise(name).replace(char::is_whitespace, "_");
    if n.is_empty() {
        nm
    } else if nm.is_empty() {
        n
    } else {
        format!("{}_{}", n, nm)
    }
}

/// Copy `template_dir` recursively into `parent / {number}_{name}`.
/// Returns the path of the new job folder on success.
///
/// Errors:
///   - parent doesn't exist or isn't a directory
///   - template_dir doesn't exist
///   - target folder name resolves to empty (both number + name blank)
///   - target folder already exists (we don't overwrite)
///   - any I/O failure during copy
pub fn create_from_template(
    parent: &Path,
    template_dir: &Path,
    number: &str,
    name: &str,
) -> Result<PathBuf, String> {
    if !parent.is_dir() {
        return Err(format!("parent is not a directory: {}", parent.display()));
    }
    if !template_dir.is_dir() {
        return Err(format!(
            "template directory not found: {}",
            template_dir.display()
        ));
    }

    let folder = job_folder_name(number, name);
    if folder.is_empty() {
        return Err("job number and name are both empty".into());
    }
    let target = parent.join(&folder);
    if target.exists() {
        return Err(format!(
            "folder already exists: {}",
            target.display()
        ));
    }

    // fs_extra::dir::copy with content_only:true puts the template
    // *contents* directly into our target (otherwise it'd nest a
    // "projectTemplate" subdir).
    let opts = fs_extra::dir::CopyOptions::new().content_only(true);
    std::fs::create_dir_all(&target)
        .map_err(|e| format!("create target {}: {}", target.display(), e))?;
    fs_extra::dir::copy(template_dir, &target, &opts)
        .map_err(|e| format!("copy template: {}", e))?;

    log::info!(
        "jobs: created {} from template {}",
        target.display(),
        template_dir.display()
    );
    Ok(target)
}
