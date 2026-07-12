//! Stub of the pre-refactor `macos_watcher` module. Slice G of the
//! mount+sync refactor (see `macOSplans/11-mount-and-sync-refactor.md`)
//! deleted the `MacosNasWatcher` struct, the FSEvents recursive watcher,
//! the 5-second poll fallback, the `dirty_folders` set, and
//! `EchoSuppressor` — all of which were originally wired to a
//! FileProvider extension that was abandoned in favour of the NFS
//! loopback server. None of them had a live consumer after Slice B.
//!
//! Only `post_darwin_notification` survives. It's still called by
//! `mount_service::handle_freshness_sweep` as a nudge for the
//! FinderSync extension (separate process; reads via the App Group
//! container) to re-evaluate badges on a domain.

/// Post a distributed notification that the FinderSync extension
/// listens for. Uses `CFNotificationCenter` with the distributed
/// center (cross-process, works in sandboxed extensions).
pub(crate) fn post_darwin_notification(domain: &str) {
    let name = format!("com.unionfiles.ufb.nas-changed.{}", domain);
    log::info!("[macos-watcher] Posting notification: {}", name);

    extern "C" {
        fn CFNotificationCenterGetDistributedCenter() -> *const std::ffi::c_void;
        fn CFNotificationCenterPostNotification(
            center: *const std::ffi::c_void,
            name: *const std::ffi::c_void,
            object: *const std::ffi::c_void,
            user_info: *const std::ffi::c_void,
            deliver_immediately: bool,
        );
        fn CFStringCreateWithCString(
            alloc: *const std::ffi::c_void,
            c_str: *const std::ffi::c_char,
            encoding: u32,
        ) -> *const std::ffi::c_void;
        fn CFRelease(cf: *const std::ffi::c_void);
    }

    let c_name = std::ffi::CString::new(name).unwrap();
    unsafe {
        let center = CFNotificationCenterGetDistributedCenter();
        // 0x08000100 = kCFStringEncodingUTF8
        let cf_name =
            CFStringCreateWithCString(std::ptr::null(), c_name.as_ptr(), 0x08000100);
        CFNotificationCenterPostNotification(
            center,
            cf_name,
            std::ptr::null(),
            std::ptr::null(),
            true,
        );
        CFRelease(cf_name);
    }
}
