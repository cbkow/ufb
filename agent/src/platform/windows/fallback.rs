use windows::Win32::Foundation::WIN32_ERROR;

/// Why establishing an SMB session failed. The caller cares about the
/// difference between "credentials were rejected" (user-fixable, surface
/// a prompt) and "couldn't reach the server" (transient, retry path).
#[derive(Debug, Clone)]
pub enum SmbSessionError {
    /// Server reachable, credentials rejected. Includes the Win32 code.
    Auth(String),
    /// Anything else - network unreachable, internal, etc.
    Other(String),
}

/// Trim a UNC path down to its share root (`\\server\share`).
///
/// `WNetAddConnection2W` is documented to take a UNC path to a share;
/// behavior with deeper sub-directory walks (`\\server\share\sub\dir`)
/// is implementation-defined and on some Windows builds returns
/// ERROR_BAD_NET_NAME (67) / ERROR_BAD_NETPATH (53). Mounts whose
/// `nas_share_path` walks into a sub-directory therefore look "up"
/// (the symlink got created) but never actually established an
/// authenticated session, and first directory access only works if
/// the LSA cache happens to hold a session from Explorer.
///
/// Symlink/junction targets still want the full deep path - only the
/// SMB session call needs to be normalized.
pub(crate) fn share_root(unc: &str) -> String {
    let s = unc.replace('/', "\\");
    let stripped = match s.strip_prefix("\\\\") {
        Some(rest) => rest,
        None => return unc.to_string(),
    };
    let mut parts = stripped.splitn(3, '\\');
    let server = parts.next().unwrap_or("");
    let share = parts.next().unwrap_or("");
    if server.is_empty() || share.is_empty() {
        return unc.to_string();
    }
    format!("\\\\{}\\{}", server, share)
}

impl std::fmt::Display for SmbSessionError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SmbSessionError::Auth(m) | SmbSessionError::Other(m) => write!(f, "{}", m),
        }
    }
}

/// Establish an authenticated SMB session to a NAS share without mapping a drive letter.
/// This enables UNC path access (for sync watcher/filter) using the provided credentials.
pub fn establish_smb_session(
    share_path: &str,
    username: &str,
    password: &str,
) -> Result<(), SmbSessionError> {
    use windows::core::PCWSTR;
    use windows::Win32::NetworkManagement::WNet::{
        WNetAddConnection2W, NETRESOURCEW, NET_CONNECT_FLAGS,
    };

    let session_target = share_root(share_path);
    let remote_name: Vec<u16> = format!("{}\0", session_target).encode_utf16().collect();
    let user: Vec<u16> = format!("{}\0", username).encode_utf16().collect();
    let pass: Vec<u16> = format!("{}\0", password).encode_utf16().collect();

    let nr = NETRESOURCEW {
        dwType: windows::Win32::NetworkManagement::WNet::RESOURCETYPE_ANY,
        lpLocalName: windows::core::PWSTR::null(),
        lpRemoteName: windows::core::PWSTR(remote_name.as_ptr() as *mut _),
        ..Default::default()
    };

    let user_ptr = if username.is_empty() {
        PCWSTR::null()
    } else {
        PCWSTR(user.as_ptr())
    };
    let pass_ptr = if password.is_empty() {
        PCWSTR::null()
    } else {
        PCWSTR(pass.as_ptr())
    };

    let result = unsafe { WNetAddConnection2W(&nr, pass_ptr, user_ptr, NET_CONNECT_FLAGS(0)) };

    if result == WIN32_ERROR(0) {
        log::info!("SMB session established for {}", share_path);
        return Ok(());
    }

    // ERROR_SESSION_CREDENTIAL_CONFLICT (1219)
    if result == WIN32_ERROR(1219) {
        // Session already exists (possibly from a drive mount or another process).
        // This is fine — the existing session will work for UNC path access.
        log::info!("SMB session already exists for {} (reusing)", share_path);
        return Ok(());
    }

    // ERROR_ALREADY_ASSIGNED (85) — shouldn't happen without a drive letter, but handle it
    if result == WIN32_ERROR(85) {
        log::info!("SMB session already established for {}", share_path);
        return Ok(());
    }

    // Classify auth-class Win32 errors so the orchestrator can transition
    // the mount into AuthFailed (and surface the "Fix credentials" pill)
    // rather than the generic Error state. Codes per MSDN:
    //   5    ERROR_ACCESS_DENIED
    //   86   ERROR_INVALID_PASSWORD
    //   1326 ERROR_LOGON_FAILURE
    const ERROR_ACCESS_DENIED: WIN32_ERROR = WIN32_ERROR(5);
    const ERROR_INVALID_PASSWORD: WIN32_ERROR = WIN32_ERROR(86);
    const ERROR_LOGON_FAILURE: WIN32_ERROR = WIN32_ERROR(1326);
    let msg = format!(
        "Failed to establish SMB session for {}: error {:?}",
        share_path, result
    );
    if result == ERROR_ACCESS_DENIED
        || result == ERROR_INVALID_PASSWORD
        || result == ERROR_LOGON_FAILURE
    {
        return Err(SmbSessionError::Auth(msg));
    }
    Err(SmbSessionError::Other(msg))
}

/// Disconnect a deviceless SMB session to a NAS share.
pub fn disconnect_smb_session(share_path: &str) -> Result<(), String> {
    use windows::Win32::NetworkManagement::WNet::{WNetCancelConnection2W, NET_CONNECT_FLAGS};

    let session_target = share_root(share_path);
    let remote_name: Vec<u16> = format!("{}\0", session_target).encode_utf16().collect();

    let result = unsafe {
        WNetCancelConnection2W(
            windows::core::PCWSTR(remote_name.as_ptr()),
            NET_CONNECT_FLAGS(0),
            false, // don't force — other processes might be using this session
        )
    };

    if result == WIN32_ERROR(0) {
        log::info!("SMB session disconnected for {}", share_path);
        Ok(())
    } else if result == WIN32_ERROR(2250) {
        // ERROR_NOT_CONNECTED
        log::debug!("No SMB session to disconnect for {}", share_path);
        Ok(())
    } else {
        // Non-fatal — log but don't fail
        log::warn!(
            "Failed to disconnect SMB session for {}: error {:?}",
            share_path, result
        );
        Ok(())
    }
}

/// What a mapped drive letter currently points at, per the network
/// provider (`WNetGetConnectionW`). None when the letter has no
/// remembered/active network mapping (local disks included).
pub fn letter_target(letter: char) -> Option<String> {
    use windows::core::{PCWSTR, PWSTR};
    use windows::Win32::NetworkManagement::WNet::WNetGetConnectionW;

    let local: Vec<u16> = format!("{}:\0", letter).encode_utf16().collect();
    let mut buf = vec![0u16; 1024];
    let mut len = buf.len() as u32;
    let res = unsafe {
        WNetGetConnectionW(PCWSTR(local.as_ptr()), PWSTR(buf.as_mut_ptr()), &mut len)
    };
    if res != WIN32_ERROR(0) {
        return None;
    }
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    Some(String::from_utf16_lossy(&buf[..end]))
}

/// Pick the drive letter for a sync (WinFsp) mount: the configured
/// preference when available, else the first free letter scanning
/// Z:→D:. Free = no volume present AND no remembered network mapping
/// squatting the letter — WinFsp creates its own device, so unlike a
/// WNet mapping the letter must be genuinely unoccupied. A leftover
/// WNet mapping of THIS share on the preferred letter (from the
/// mount's pre-sync life) is forgotten so the letter stays stable
/// across the plain→sync transition.
pub fn choose_free_letter(preferred: Option<char>, nas_share_path: &str) -> Option<char> {
    use windows::core::PCWSTR;
    use windows::Win32::NetworkManagement::WNet::{
        WNetCancelConnection2W, CONNECT_UPDATE_PROFILE,
    };
    use windows::Win32::Storage::FileSystem::GetLogicalDrives;

    let mask = unsafe { GetLogicalDrives() };
    let is_free = |c: char| {
        let bit = 1u32 << (c.to_ascii_uppercase() as u8 - b'A');
        mask & bit == 0 && letter_target(c).is_none()
    };
    if let Some(p) = preferred {
        let p = p.to_ascii_uppercase();
        if p.is_ascii_alphabetic() {
            if is_free(p) {
                return Some(p);
            }
            let ours = letter_target(p)
                .map(|t| share_root(&t).eq_ignore_ascii_case(&share_root(nas_share_path)))
                .unwrap_or(false);
            if ours {
                let local: Vec<u16> = format!("{}:\0", p).encode_utf16().collect();
                let _ = unsafe {
                    WNetCancelConnection2W(
                        PCWSTR(local.as_ptr()),
                        CONNECT_UPDATE_PROFILE,
                        true,
                    )
                };
                if is_free(p) || letter_target(p).is_none() {
                    return Some(p);
                }
            }
        }
    }
    ('D'..='Z').rev().find(|&c| is_free(c))
}

/// Tell the shell a drive letter appeared/vanished. Explorer's This
/// PC doesn't watch WinFsp mounts made by another process; without
/// the broadcast the letter only shows up after a manual refresh.
pub fn notify_shell_drive(letter: char, added: bool) {
    use windows::Win32::UI::Shell::{
        SHChangeNotify, SHCNE_DRIVEADD, SHCNE_DRIVEREMOVED, SHCNF_PATHW,
    };
    let root: Vec<u16> = format!("{}:\\\0", letter.to_ascii_uppercase())
        .encode_utf16()
        .collect();
    unsafe {
        SHChangeNotify(
            if added { SHCNE_DRIVEADD } else { SHCNE_DRIVEREMOVED },
            SHCNF_PATHW,
            Some(root.as_ptr() as *const core::ffi::c_void),
            None,
        );
    }
}

// `disconnect_drive` deleted 2026-07-11 — its only caller was the
// elevation-era letter-migration in create_symlinks_and_exit (gone in
// slice B). Letter unmapping lives in core::windows_mounts (GUI) and
// choose_free_letter's forget path here.
