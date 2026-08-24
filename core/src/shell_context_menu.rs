//! Show the native Windows Explorer context menu for a file or folder.
//! Uses the IContextMenu COM interface on Windows; AppleScript on macOS.

/// True while a shell menu thread is alive. Guards against stacking a
/// second `TrackPopupMenuEx` loop on top of the first if the user
/// triggers the action again before dismissing the menu.
#[cfg(windows)]
static SHELL_MENU_OPEN: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Clears [`SHELL_MENU_OPEN`] when dropped — including on unwind, so a
/// panic inside the menu thread can't leave the action wedged shut.
#[cfg(windows)]
struct ShellMenuOpenGuard;

#[cfg(windows)]
impl Drop for ShellMenuOpenGuard {
    fn drop(&mut self) {
        SHELL_MENU_OPEN.store(false, std::sync::atomic::Ordering::Release);
    }
}

#[cfg(windows)]
pub fn show_shell_context_menu(path: &str) -> Result<(), String> {
    use std::path::Path;
    use std::sync::atomic::Ordering;

    let p = Path::new(path);
    if !p.exists() {
        return Err(format!("Path does not exist: {}", path));
    }

    if SHELL_MENU_OPEN.swap(true, Ordering::AcqRel) {
        return Err("shell context menu is already open".to_string());
    }
    let guard = ShellMenuOpenGuard;

    log::info!("show_shell_context_menu: {}", path);

    // Capture cursor position NOW (before the thread spawn delay)
    let (cursor_x, cursor_y) = unsafe {
        use windows::Win32::Foundation::POINT;
        use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;
        let mut pt = POINT::default();
        let _ = GetCursorPos(&mut pt);
        (pt.x, pt.y)
    };

    // Run on a dedicated STA thread. TrackPopupMenuEx blocks (pumps its
    // own message loop) so we must not call it on the async runtime —
    // and we must NOT join it from the caller either: this is invoked
    // from the QML/GUI thread, and joining froze the main window for as
    // long as the shell menu stayed open. Windows flags the app as "not
    // responding" after 5 s of that, and any shell extension that
    // SendMessage()s back to the main window deadlocks outright. So the
    // thread is fire-and-forget; its outcome only goes to the log.
    let path_owned = path.to_string();
    std::thread::Builder::new()
        .name("ufb-shell-ctxmenu".to_string())
        .spawn(move || {
            let _guard = guard;
            if let Err(e) = show_menu_blocking(&path_owned, cursor_x, cursor_y) {
                log::warn!("show_shell_context_menu({}): {}", path_owned, e);
            }
        })
        .map_err(|e| format!("failed to spawn shell context menu thread: {}", e))?;
    Ok(())
}

#[cfg(windows)]
fn show_menu_blocking(path: &str, cursor_x: i32, cursor_y: i32) -> Result<(), String> {
    use windows::core::{w, HSTRING, PCSTR};
    use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
    use windows::Win32::System::Com::{
        CoInitializeEx, CoUninitialize, COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE,
    };
    use windows::Win32::UI::Shell::{
        IContextMenu, IShellItem, SHCreateItemFromParsingName, BHID_SFUIObject,
        CMINVOKECOMMANDINFO, CMF_EXPLORE, CMF_NORMAL,
    };
    use windows::Win32::UI::WindowsAndMessaging::*;

    unsafe extern "system" fn wnd_proc(
        hwnd: HWND,
        msg: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        DefWindowProcW(hwnd, msg, wparam, lparam)
    }

    unsafe {
        let hr = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        let we_init = hr.is_ok();

        let result = (|| -> Result<(), String> {
            // Hidden popup window on THIS thread. TrackPopupMenuEx requires
            // an hwnd whose message queue belongs to the calling thread.
            // Message-only windows (HWND_MESSAGE) cause dismiss problems, so
            // use a zero-size WS_POPUP instead.
            let class_name = w!("UFBShellCtxMenuHost");
            let wc = WNDCLASSEXW {
                cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
                lpfnWndProc: Some(wnd_proc),
                lpszClassName: class_name,
                ..Default::default()
            };
            RegisterClassExW(&wc); // OK if already registered

            let hwnd = CreateWindowExW(
                WINDOW_EX_STYLE::default(),
                class_name,
                w!(""),
                WS_POPUP,
                0,
                0,
                0,
                0,
                None,
                None,
                None,
                None,
            )
            .map_err(|e| format!("CreateWindowExW failed: {}", e))?;

            let wide_path = HSTRING::from(path);
            let shell_item: IShellItem = SHCreateItemFromParsingName(&wide_path, None)
                .map_err(|e| format!("SHCreateItemFromParsingName failed: {}", e))?;

            let context_menu: IContextMenu = shell_item
                .BindToHandler(None, &BHID_SFUIObject)
                .map_err(|e| format!("BindToHandler for IContextMenu failed: {}", e))?;

            let hmenu =
                CreatePopupMenu().map_err(|e| format!("CreatePopupMenu failed: {}", e))?;

            context_menu
                .QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE)
                .map_err(|e| format!("QueryContextMenu failed: {}", e))?;

            // MSDN requirement: owner window must be foreground.
            let _ = SetForegroundWindow(hwnd);

            let cmd = TrackPopupMenuEx(
                hmenu,
                (TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN).0,
                cursor_x,
                cursor_y,
                hwnd,
                None,
            );

            // MSDN requirement: post WM_NULL so menu-tracking finishes cleanly.
            let _ = PostMessageW(hwnd, WM_NULL, WPARAM(0), LPARAM(0));

            if cmd.0 > 0 {
                let verb_index = (cmd.0 as u32).wrapping_sub(1);
                let ici = CMINVOKECOMMANDINFO {
                    cbSize: std::mem::size_of::<CMINVOKECOMMANDINFO>() as u32,
                    hwnd,
                    lpVerb: PCSTR(verb_index as usize as *const u8),
                    nShow: 1, // SW_SHOWNORMAL
                    ..Default::default()
                };

                if let Err(e) = context_menu.InvokeCommand(&ici) {
                    log::warn!("InvokeCommand failed: {}", e);
                }
            }

            drop(context_menu);
            drop(shell_item);

            let _ = DestroyMenu(hmenu);
            let _ = DestroyWindow(hwnd);
            Ok(())
        })();

        if we_init {
            CoUninitialize();
        }

        result
    }
}

/// macOS: reveal in Finder + simulate Ctrl+click via AppleScript.
/// Note: requires Accessibility permissions for System Events on first use.
#[cfg(target_os = "macos")]
pub fn show_shell_context_menu(path: &str) -> Result<(), String> {
    let p = std::path::Path::new(path);
    if !p.exists() {
        return Err(format!("Path does not exist: {}", path));
    }

    log::info!("show_shell_context_menu (macOS): {}", path);

    let escaped_path = path.replace('\\', "\\\\").replace('"', "\\\"");
    let script = format!(
        r#"tell application "Finder"
    activate
    reveal POSIX file "{}"
end tell
delay 0.3
tell application "System Events"
    tell process "Finder"
        set frontmost to true
        keystroke return using {{control down}}
    end tell
end tell"#,
        escaped_path
    );

    let path_owned = path.to_string();
    std::thread::spawn(move || {
        let result = std::process::Command::new("osascript")
            .args(["-e", &script])
            .output();

        match result {
            Ok(output) if !output.status.success() => {
                let stderr = String::from_utf8_lossy(&output.stderr);
                log::warn!(
                    "Context menu script failed for {}: {}",
                    path_owned,
                    stderr.trim()
                );
            }
            Err(e) => {
                log::warn!("Failed to run osascript for {}: {}", path_owned, e);
            }
            _ => {}
        }
    });

    Ok(())
}
