//! NetFS framework wrapper for credentialed SMB mounts.
//!
//! `mount_smbfs -N` queries the user's Keychain for `kSecClassInternetPassword`
//! keyed on host + protocol — but UFB stores its credentials as
//! `kSecClassGenericPassword` under `ufb_<key>` service names. The two
//! never meet, so the silent path falls through and Finder pops a Connect
//! to Server dialog every mount.
//!
//! `NetFSMountURLSync` takes user + password as in-memory CFStrings, so
//! we hand the agent's resolved credentials directly to the mount call:
//! no Keychain class collision, no plaintext password in argv (visible to
//! `ps`), no UI prompting (`kNAUIOptionNoUI`).
//!
//! This is the C API `mount_smbfs` itself wraps internally and what
//! Finder uses for `Cmd-K Connect to Server`. It's the Apple-blessed
//! programmatic mount path.

use core_foundation::base::TCFType;
use core_foundation::dictionary::CFMutableDictionary;
use core_foundation::number::CFNumber;
use core_foundation::string::{CFString, CFStringRef};
use core_foundation::url::{CFURL, CFURLRef};
use core_foundation_sys::array::{CFArrayGetCount, CFArrayGetValueAtIndex, CFArrayRef};
use core_foundation_sys::base::{kCFAllocatorDefault, CFRelease};
use core_foundation_sys::dictionary::CFMutableDictionaryRef;
use core_foundation_sys::url::CFURLCreateWithString;
use std::path::Path;

#[link(name = "NetFS", kind = "framework")]
extern "C" {
    /// `NetFSMountURLSync(url, mountpath, user, passwd, open_options,
    ///                    mount_options, mountpoints)`
    /// Returns 0 on success, errno-style on failure (see `<sys/errno.h>`).
    /// Notable codes: `EAUTH` (80) — bad credentials. `ECANCELED` (89)
    /// — user cancelled (only with UI allowed). `ETIMEDOUT` (60).
    /// `mountpoints` (out) is a CFArray of CFString; first entry is the
    /// path the share was actually mounted at (NetFS may pick a fallback
    /// like `/Volumes/Share-1` if the requested path was busy).
    fn NetFSMountURLSync(
        url: CFURLRef,
        mountpath: CFURLRef,
        user: CFStringRef,
        passwd: CFStringRef,
        open_options: CFMutableDictionaryRef,
        mount_options: CFMutableDictionaryRef,
        mountpoints: *mut CFArrayRef,
    ) -> i32;
}

/// `kNAUIOptionKey` from `<NetFS/NetFS.h>` — the UI-policy switch in the
/// mount-options dictionary.
const NA_UI_OPTION_KEY: &str = "UIOption";
/// `kNAUIOptionNoUI` — suppress all NetFS-side prompts. With UI allowed,
/// `NetFSMountURLSync` would block on auth failure showing a system
/// dialog; we want failures to return errnos so the GUI can render a
/// "credentials incorrect" pill instead.
const NA_UI_OPTION_NO_UI: i32 = 0;

/// `EAUTH` from `<sys/errno.h>` — surfaced by NetFS when credentials
/// are wrong or missing. Callers map this to an auth-failed mount
/// state so the sidebar can prompt the user to fix credentials.
pub const EAUTH: i32 = 80;

/// Mount an SMB share via NetFS. Returns the actual mount path on
/// success (which may differ from the requested `mountpoint` if NetFS
/// rerouted) or the raw errno on failure.
///
/// `credentials`: `Some((user, pass))` passes explicit in-memory
/// CFStrings (legacy path). `None` passes NULL for both — NetFS then
/// consults the login Keychain's `kSecClassInternetPassword` entries
/// for this server, exactly like Finder's Cmd-K silent path. This is
/// the OS-native credential flow (plans/17 slice C): UFB never reads
/// or stores the secret itself.
///
/// `allow_ui`: when true the `kNAUIOptionNoUI` suppression is omitted,
/// so on missing/stale credentials NetAuthAgent presents Apple's own
/// Connect-to-Server auth dialog (with "Remember this password in my
/// keychain"). NetAuthAgent hosts that dialog in its own process, so
/// this works from the headless agent too. When false, failures come
/// back as errnos (EAUTH etc.) for the sidebar pill to render.
///
/// When `mountpoint` is `None`, NetFS picks its default location
/// (`/Volumes/<share>`) — that's also Apple's "standard mount path" so
/// macOS Sequoia's "wants to mount to this folder, which is unusual"
/// confirmation never fires. With `Some(path)` we land at exactly that
/// path but eat the prompt every time, so the orchestrator passes
/// `None` and re-points its user-facing symlinks at whatever NetFS
/// returns.
pub fn netfs_smb_mount(
    smb_url: &str,
    mountpoint: Option<&Path>,
    credentials: Option<(&str, &str)>,
    allow_ui: bool,
) -> Result<String, i32> {
    log::info!(
        "[netfs] mount {} → {} (creds={}, ui={})",
        smb_url,
        mountpoint
            .map(|p| p.display().to_string())
            .unwrap_or_else(|| "/Volumes/<auto>".into()),
        match &credentials {
            Some((u, _)) => format!("explicit user={}", u),
            None => "keychain".into(),
        },
        allow_ui,
    );

    let url_str = CFString::new(smb_url);
    let cf_url = unsafe {
        let raw = CFURLCreateWithString(
            kCFAllocatorDefault,
            url_str.as_concrete_TypeRef(),
            std::ptr::null(),
        );
        if raw.is_null() {
            log::warn!("netfs: CFURLCreateWithString returned null for {}", smb_url);
            return Err(libc::EINVAL);
        }
        CFURL::wrap_under_create_rule(raw)
    };

    // file:// URL of the local mountpoint we want NetFS to mount onto.
    // Optional — Apple's docs explicitly say NULL means "let NetFS pick
    // a default location under /Volumes". Wrapping in Option keeps the
    // pointer story explicit: Some → real CFURL, None → null pointer.
    let cf_mountpath = match mountpoint {
        Some(path) => match CFURL::from_path(path, true) {
            Some(u) => Some(u),
            None => {
                log::warn!("netfs: CFURL::from_path failed for {}", path.display());
                return Err(libc::EINVAL);
            }
        },
        None => None,
    };

    // None → NULL CFStringRefs → NetFS consults the login Keychain
    // (and prompts via NetAuthAgent when allow_ui).
    let cf_creds = credentials
        .map(|(u, p)| (CFString::new(u), CFString::new(p)));

    // Mount options: `{ "UIOption": 0 }` (no UI) unless the caller
    // explicitly allows the system auth dialog. NetFS wants
    // CFMutableDictionaryRef for both options arguments — internally it
    // populates default values into them, and pushing into an immutable
    // dictionary throws a CFException across the FFI boundary, which
    // Rust treats as a foreign exception and aborts the process.
    let mut mount_options: CFMutableDictionary<CFString, CFNumber> =
        CFMutableDictionary::new();
    if !allow_ui {
        mount_options.add(
            &CFString::new(NA_UI_OPTION_KEY),
            &CFNumber::from(NA_UI_OPTION_NO_UI),
        );
    }

    // Empty mutable open_options so NetFS can write its session-level
    // defaults too. Passing null was tempting, but Apple's docs are
    // ambiguous and at least one undocumented codepath dereferences
    // the pointer unconditionally — better to give it a real bag.
    let open_options: CFMutableDictionary<CFString, CFNumber> =
        CFMutableDictionary::new();

    let mut mountpoints: CFArrayRef = std::ptr::null();
    let mountpath_ref = cf_mountpath
        .as_ref()
        .map(|u| u.as_concrete_TypeRef())
        .unwrap_or(std::ptr::null_mut());
    let (user_ref, pass_ref): (CFStringRef, CFStringRef) = match &cf_creds {
        Some((u, p)) => (u.as_concrete_TypeRef(), p.as_concrete_TypeRef()),
        None => (std::ptr::null(), std::ptr::null()),
    };
    let status = unsafe {
        NetFSMountURLSync(
            cf_url.as_concrete_TypeRef(),
            mountpath_ref,
            user_ref,
            pass_ref,
            open_options.as_concrete_TypeRef() as CFMutableDictionaryRef,
            mount_options.as_concrete_TypeRef() as CFMutableDictionaryRef,
            &mut mountpoints,
        )
    };

    log::info!(
        "[netfs] NetFSMountURLSync({}) returned status={}",
        smb_url,
        status,
    );

    if status != 0 {
        if !mountpoints.is_null() {
            unsafe { CFRelease(mountpoints as _) };
        }
        return Err(status);
    }

    // Out array: CFArray of CFString (per Apple's NetFS.h). Use the sys
    // API to read index 0 by raw pointer, retain it as a CFString, then
    // release the array. Skipping the typed `CFArray<CFString>` wrapper
    // because mismatched-type assumptions there can throw a CF
    // exception across the FFI boundary, which Rust treats as a
    // foreign exception and aborts the process.
    let resolved: Option<String> = if !mountpoints.is_null() {
        let count = unsafe { CFArrayGetCount(mountpoints) };
        let first = if count > 0 {
            let raw_item = unsafe { CFArrayGetValueAtIndex(mountpoints, 0) };
            if raw_item.is_null() {
                None
            } else {
                let s = unsafe {
                    CFString::wrap_under_get_rule(raw_item as CFStringRef)
                };
                Some(s.to_string())
            }
        } else {
            None
        };
        unsafe { CFRelease(mountpoints as _) };
        first
    } else {
        None
    };

    Ok(resolved.unwrap_or_else(|| {
        // Should not happen on success per Apple's docs — fall back
        // to the requested path (or a placeholder if we asked NetFS to
        // pick).
        mountpoint
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default()
    }))
}

/// Map an NetFS errno to a short human message for log output. The raw
/// number is always preserved so error pills can key off it.
pub fn status_message(status: i32) -> String {
    let label = match status {
        EAUTH => "authentication failed",
        libc::ECANCELED => "sign-in cancelled",
        libc::EACCES => "permission denied",
        libc::EBUSY => "mountpoint busy",
        libc::ENETUNREACH => "network unreachable",
        libc::ETIMEDOUT => "operation timed out",
        libc::ECONNREFUSED => "connection refused",
        libc::EHOSTUNREACH => "no route to host",
        libc::ENOENT => "share not found",
        _ => "mount failed",
    };
    format!("{} (errno {})", label, status)
}
