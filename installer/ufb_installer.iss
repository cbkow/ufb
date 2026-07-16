; Union File Browser (Qt) Installer Script for Inno Setup 6
; https://jrsoftware.org/isinfo.php
;
; Build the app first (from repo root):
;   cmake --preset release
;   cmake --build build/release
;   cargo build --release --manifest-path agent/Cargo.toml   (ufb-agent.exe)
;   windeployqt --qmldir app/qml --release build/release/ufb.exe
;   scripts/setup-external.ps1               (ffmpeg DLLs)
;   <copy vcpkg DLLs into build/release/>    (OpenEXR + deps)
;   <pdfium.dll already next to ufb.exe>
; Then compile this with Inno Setup 6.
;
; Components scoped to what the new Qt port currently ships:
;   * Main exe + Qt + Phosphor + cxx-qt bindings (.dll)
;   * Project template tree                     (.\templates\)
;   * ufb:/// URI scheme registration           (HKCR)
;   * union:/// URI scheme + open_union_link.ps1
;   * Mesh sync firewall rules (TCP 49221/49222, UDP 4265)
;   * Start Menu / Desktop shortcuts
;
; Transitional upgrade scrubs (Nilesoft imports, UfbAgent/
; MediaMountAgent Run keys, Explorer nav-pins + NoDrives mask, legacy
; firewall rule names) were dropped in 1.0.7 — the 1.0.6 installer
; already cleaned that debris, and upgrades come from 1.0.6.

#define MyAppName "Union File Browser"
#define MyAppVersion "1.0.9"
#define MyAppPublisher "cbkow"
#define MyAppURL "https://github.com/cbkow/ufb"
#define MyAppExeName "ufb.exe"

; Paths relative to this .iss file (repo-root/installer/).
#define ReleaseDir "..\build\release"
#define IconsDir   "..\app\icons"
#define AgentDir   "..\agent\target\release"
#define WinFspDir  "..\external\winfsp"
#define ScriptsDir "..\assets\scripts"

[Setup]
AppId={{B3C9D5E7-4F8A-6B2C-9D1E-7A3F5C8E2D4B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) 2026 {#MyAppPublisher}

DefaultDirName={autopf}\Union File Browser
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes

LicenseFile=..\LICENSE

OutputDir=.
; Artifact naming standard: UFB-<version>-<arch>.<ext> (matches the
; macOS UFB-<ver>-arm64.dmg convention). The WinSparkle appcast
; enclosure URL must match this filename on the update host.
OutputBaseFilename=UFB-{#MyAppVersion}-x64
Compression=lzma2/max
SolidCompression=yes

SetupIconFile={#IconsDir}\icon.ico
; Point Programs-and-Features at the on-disk .ico rather than the
; exe's embedded resource. Embedding works locally, but remote
; machines occasionally show a generic icon if the exe resource
; isn't picked up cleanly (icon cache, antivirus rewriting the
; binary). The .ico file is unambiguous.
UninstallDisplayIcon={app}\icons\icon.ico
WizardStyle=modern

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0.17763

UninstallDisplayName={#MyAppName}
UninstallFilesDir={app}\uninstall

AllowNoIcons=yes
DisableWelcomePage=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation (recommended)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "core"; Description: "Core application files"; Types: full custom; Flags: fixed
Name: "uri_protocol"; Description: "Register ufb:/// + union:/// URI protocols for project links"; Types: full
Name: "firewall"; Description: "Add Windows Firewall rules for Mesh Sync (TCP 49221/49222, UDP 4265)"; Types: full
Name: "shortcuts"; Description: "Create shortcuts"
Name: "shortcuts\desktop"; Description: "Create desktop shortcut"; Types: full
Name: "shortcuts\startmenu"; Description: "Create Start Menu shortcuts"; Types: full; Flags: fixed

; [Dirs] C:\Volumes\ufb is GONE (plans/17 slice B): mounts are drive
; letters now — plain mounts are OS-restored persistent WNet mappings,
; sync mounts are WinFsp letters. The junction/symlink farm and its
; admin-created base dir are dead. Leftover links inside an existing
; C:\Volumes\ufb are cleaned by the agent per-mount on sync spawn; the
; empty dir itself is harmless and left to the user.

[Tasks]
Name: "app_autostart"; Description: "Start {#MyAppName} in the background at Windows login (tray icon; mounts come up automatically)"; GroupDescription: "Startup:"; Flags: unchecked
Name: "cleansettings"; Description: "Remove user preferences (%LOCALAPPDATA%\ufb\settings.json) - NOT RECOMMENDED"; GroupDescription: "User data cleanup:"; Flags: unchecked
Name: "cleandb"; Description: "Remove database (%LOCALAPPDATA%\ufb\ufb_v5.db) - NOT RECOMMENDED"; GroupDescription: "User data cleanup:"; Flags: unchecked
Name: "cleanall"; Description: "Remove ALL user data and preferences (%LOCALAPPDATA%\ufb\) - NOT RECOMMENDED"; GroupDescription: "User data cleanup:"; Flags: unchecked
Name: "launchafter"; Description: "Launch {#MyAppName} after installation"; GroupDescription: "Post-installation:"; Flags: unchecked

[Files]
; Main executable
Source: "{#ReleaseDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion; Components: core

; UFB sync agent. Built separately by `cargo build --release` in the
; agent/ workspace - lands at agent\target\release\ufb-agent.exe.
; Slice B: the agent is the sync VFS host ONLY (WinFsp letters for
; sync-enabled mounts). The GUI owns plain mounts and spawns the
; agent on demand when a sync mount exists (see bindings::services::
; mount::spawn_agent_if_needed). (Mesh sync runs in ufb.exe, not the
; agent.)
Source: "{#AgentDir}\ufb-agent.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: core

; All runtime DLLs from build/release/. windeployqt + setup-external.ps1
; populate Qt + ffmpeg + pdfium + OpenEXR + GraphicsMagick (vcpkg) DLLs
; here. windeployqt on Windows places Qt plugins as flat top-level
; sibling dirs (platforms\, imageformats\, ...), NOT under a single
; plugins\ subdir - earlier versions of this script looked for a
; plugins\ that never existed and silently shipped a broken bundle
; (no qwindows.dll => app refuses to start).
Source: "{#ReleaseDir}\*.dll";              DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\qml\*";              DestDir: "{app}\qml";              Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\platforms\*";        DestDir: "{app}\platforms";        Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\imageformats\*";     DestDir: "{app}\imageformats";     Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\iconengines\*";      DestDir: "{app}\iconengines";      Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\tls\*";              DestDir: "{app}\tls";              Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
; SQLite driver — required by the thumbnail disk cache (ThumbCache, Qt SQL).
; windeployqt stages it under sqldrivers\qsqlite.dll; without this line the
; installed app has no QSQLITE driver and the cache silently no-ops.
Source: "{#ReleaseDir}\sqldrivers\*";       DestDir: "{app}\sqldrivers";       Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\generic\*";          DestDir: "{app}\generic";          Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\translations\*";     DestDir: "{app}\translations";     Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core

; Project templates (used by NewJobDialog)
Source: "{#ReleaseDir}\templates\*"; DestDir: "{app}\templates"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core

; Transcode subprocess binaries. ffmpeg.exe + ffprobe.exe drive the
; encode + frame-count probe; exiftool.exe (with its bundled Perl
; tree under exiftool_files\) copies metadata onto the output MP4.
; bindings/src/services/transcode.rs::bundled_tool resolves these
; relative to the running exe, so the layout next to {app}\ufb.exe
; is what the runtime expects. Sourced from external/ via the
; cmake POST_BUILD copies in app/CMakeLists.txt, which in turn pull
; from setup-external.ps1's QCView + ufb-tauri mirrors.
Source: "{#ReleaseDir}\ffmpeg.exe";        DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\ffprobe.exe";       DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\exiftool.exe";      DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: core
Source: "{#ReleaseDir}\exiftool_files\*";  DestDir: "{app}\exiftool_files"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: core

; Icons
Source: "{#IconsDir}\32x32.png"; DestDir: "{app}\icons"; Flags: ignoreversion; Components: core
Source: "{#IconsDir}\icon.ico";  DestDir: "{app}\icons"; Flags: ignoreversion; Components: core

; Documentation
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion; Components: core
Source: "..\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core

; WinFsp redistributable. `dontcopy` keeps it out of {app} - we
; extract to {tmp} and run msiexec only when WinFsp isn't already
; installed on the target. Stable filename (winfsp.msi) is enforced
; by scripts/setup-external.ps1; the actual version is in
; external/winfsp/version.txt for traceability.
Source: "{#WinFspDir}\winfsp.msi"; Flags: dontcopy noencryption; Components: core

; open_union_link.ps1 backs the union:/// protocol handler below.
Source: "{#ScriptsDir}\open_union_link.ps1";    DestDir: "{app}\assets\scripts"; Flags: ignoreversion; Components: core

[Icons]
; Pin the icon source explicitly (rather than relying on Windows
; reading the embedded resource off ufb.exe) so the right icon
; surfaces on machines where antivirus / SmartScreen rewrote the
; binary, the exe-embedded resource isn't picked up cleanly, or
; the shell icon cache previously latched onto a generic fallback.
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icons\icon.ico"; IconIndex: 0; Components: shortcuts\startmenu
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"; Components: shortcuts\startmenu
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icons\icon.ico"; IconIndex: 0; Components: shortcuts\desktop

[Registry]
; ufb:/// protocol — registered HKCR so any user on this machine can
; open ufb:// links. The %1 placeholder is the URI; main.cpp reads
; argv[1] and FileOps.resolve_ufb_uri(argv[1]) parses + applies the
; cross-OS path-mapping swap before navigating.
Root: HKCR; Subkey: "ufb"; ValueType: string; ValueName: ""; ValueData: "URL:Union File Browser Protocol"; Flags: uninsdeletekey; Components: uri_protocol
Root: HKCR; Subkey: "ufb"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Components: uri_protocol
Root: HKCR; Subkey: "ufb\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\icons\icon.ico,0"; Components: uri_protocol
Root: HKCR; Subkey: "ufb\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: uri_protocol

; union:// — sibling protocol that opens the resolved path in
; Explorer instead of in-app. Handled by open_union_link.ps1, which
; strips the OS tag, applies cross-OS path mappings from
; %LOCALAPPDATA%\ufb\settings.json, and calls explorer.exe /select.
; -ExecutionPolicy Bypass + -NoProfile so the handler doesn't fail
; on machines with restricted policy or slow profile scripts.
Root: HKCR; Subkey: "union"; ValueType: string; ValueName: ""; ValueData: "URL:Union Protocol"; Flags: uninsdeletekey; Components: uri_protocol
Root: HKCR; Subkey: "union"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Components: uri_protocol
Root: HKCR; Subkey: "union\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\icons\icon.ico,0"; Components: uri_protocol
Root: HKCR; Subkey: "union\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """powershell.exe"" -NoProfile -ExecutionPolicy Bypass -File ""{app}\assets\scripts\open_union_link.ps1"" ""%1"""; Components: uri_protocol

; App User Model ID (Windows 10/11 taskbar grouping). Two halves:
;
; 1. Map ufb.exe → AUMID via Software\Classes\Applications\<exe>. This
;    is what Windows reads to figure out which AUMID a launched .exe
;    belongs to when no explicit AUMID has been set yet.
;
; 2. Register the AUMID itself under Software\Classes\AppUserModelId\
;    <AUMID> with a DisplayName + an explicit RelaunchIconResource that
;    points at the on-disk .ico. Without this second half Windows has
;    no icon associated with our AUMID, and the taskbar / Alt-Tab grup
;    falls back to a generic icon even when WM_SETICON has set the
;    correct one on the running window. The path syntax is
;    "fully-qualified-icon-file,resource-index" - 0 picks the default.
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExeName}"; ValueType: string; ValueName: "AppUserModelID"; ValueData: "com.unionfiles.ufb"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\AppUserModelId\com.unionfiles.ufb"; ValueType: string; ValueName: ""; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\AppUserModelId\com.unionfiles.ufb"; ValueType: string; ValueName: "DisplayName"; ValueData: "{#MyAppName}"
Root: HKLM; Subkey: "Software\Classes\AppUserModelId\com.unionfiles.ufb"; ValueType: string; ValueName: "RelaunchIconResource"; ValueData: "{app}\icons\icon.ico,0"
Root: HKLM; Subkey: "Software\Classes\AppUserModelId\com.unionfiles.ufb"; ValueType: string; ValueName: "RelaunchCommand"; ValueData: """{app}\{#MyAppExeName}"""
Root: HKLM; Subkey: "Software\Classes\AppUserModelId\com.unionfiles.ufb"; ValueType: string; ValueName: "RelaunchDisplayNameResource"; ValueData: "{#MyAppName}"

; Optional: launch UFB in background mode at Windows login (plans/17
; slice E/B: one user-facing app). --background starts resident with
; the in-app tray and no window; plain-mount letters are OS-restored
; at logon anyway, and the GUI touches each one as the reconnect
; nudge + spawns the agent only when a sync mount needs the VFS host.
; Supersedes the old UfbAgent Run key (the agent no longer owns
; mounts); 1.0.6 scrubbed that legacy value (and the Tauri-era
; MediaMountAgent one) on upgrade.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "UFB"; ValueData: """{app}\{#MyAppExeName}"" --background"; Flags: uninsdeletevalue; Tasks: app_autostart

[Code]
var
  DataCleanupPage: TInputOptionWizardPage;

procedure InitializeWizard();
begin
  DataCleanupPage := CreateInputOptionPage(wpSelectTasks,
    'User Data Cleanup - WARNING',
    'Carefully review these options before proceeding',
    'The options below will DELETE your user data. This is usually NOT what you want unless you are completely removing the app from your system.',
    False, False);
  DataCleanupPage.Add('I understand that checking data cleanup tasks will DELETE my settings and/or database');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  if PageID = DataCleanupPage.ID then
    Result := not (WizardIsTaskSelected('cleansettings') or
                   WizardIsTaskSelected('cleandb') or
                   WizardIsTaskSelected('cleanall'))
  else
    Result := False;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  AppDir, LocalAppData, SettingsFile, DbFile: String;
begin
  if CurStep = ssInstall then
  begin
    // Stop running UFB processes before overwriting binaries. Both
    // ufb.exe (the GUI) and ufb-agent.exe (the mount/sync sidecar)
    // hold file handles in {app}; missing the agent kill leaves the
    // old binary locked and DelTree below fails silently. Kill
    // ufb.exe first so it doesn't keep respawning the agent in the
    // race window before we kill it.
    Exec('taskkill.exe', '/f /im {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('taskkill.exe', '/f /im ufb-agent.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(750);

    // Clean old program files
    AppDir := ExpandConstant('{app}');
    if DirExists(AppDir) then
      DelTree(AppDir, True, True, True);

    LocalAppData := ExpandConstant('{localappdata}\ufb');

    if WizardIsTaskSelected('cleanall') then
    begin
      if DirExists(LocalAppData) then
        DelTree(LocalAppData, True, True, True);
    end
    else
    begin
      if WizardIsTaskSelected('cleansettings') then
      begin
        SettingsFile := LocalAppData + '\settings.json';
        if FileExists(SettingsFile) then
          DeleteFile(SettingsFile);
      end;
      if WizardIsTaskSelected('cleandb') then
      begin
        DbFile := LocalAppData + '\ufb_v5.db';
        if FileExists(DbFile) then DeleteFile(DbFile);
        if FileExists(DbFile + '-wal') then DeleteFile(DbFile + '-wal');
        if FileExists(DbFile + '-shm') then DeleteFile(DbFile + '-shm');
      end;
    end;
  end;

  if CurStep = ssPostInstall then
  begin
    // ── WinFsp redistributable ─────────────────────────────────────
    // Agent's mount/sync stack delay-loads winfsp-x64.dll. Without
    // WinFsp installed, the first mount-start hits a load failure.
    // Detect via HKLM\SOFTWARE\WinFsp - either the 32-bit or 64-bit
    // view, since WinFsp installs a 32-bit registry view too. Run
    // msiexec silently with /qn /norestart; ExtractTemporaryFile
    // unpacks our bundled winfsp.msi to {tmp}.
    if not (RegKeyExists(HKLM, 'SOFTWARE\WinFsp') or
            RegKeyExists(HKLM64, 'SOFTWARE\WinFsp')) then
    begin
      try
        ExtractTemporaryFile('winfsp.msi');
        Exec('msiexec.exe',
          '/i "' + ExpandConstant('{tmp}\winfsp.msi') + '" /qn /norestart',
          '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
        // ResultCode 0 = success, 3010 = success but reboot required.
        // Anything else: log + keep going so the rest of the install
        // still completes; user just won't have working mounts until
        // they install WinFsp manually.
        if (ResultCode <> 0) and (ResultCode <> 3010) then
          Log('WinFsp silent install returned code ' + IntToStr(ResultCode));
      except
        Log('WinFsp install threw an exception; continuing without it.');
      end;
    end;

    // Nudge the shell icon cache so freshly-created shortcuts pick
    // up our pinned IconFilename (instead of a generic-fallback that
    // got cached when the install dir was being populated). Equivalent
    // of running `ie4uinit.exe -ClearIconCache` + a SHChangeNotify;
    // ie4uinit.exe ships with every supported Windows version.
    Exec(ExpandConstant('{sys}\ie4uinit.exe'), '-ClearIconCache', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);

    // ── SmartScreen prime ─────────────────────────────────────────
    // Launch ufb.exe and ufb-agent.exe with --prime-smartscreen so
    // Windows' first-run reputation check completes BEFORE the user
    // ever launches them. Without this, the user's first traversal of
    // a WinFsp mount from a process Windows hasn't ratified yet can
    // return ERROR_UNTRUSTED_MOUNT_POINT (448) and the browser spins.
    // The retry in core/src/file_ops::read_dir_with_448_retry has a
    // finite budget and can miss when the scan is slow.
    //
    // Both EXEs handle the flag by sleeping 3s and exiting cleanly
    // (no Qt setup, no IPC pipe, no mount work). Launch them
    // detached, let them sit through their sleep, the scan completes
    // alongside. Sleep here is longer than the EXE sleeps so they
    // exit on their own.
    Exec(ExpandConstant('{app}\{#MyAppExeName}'), '--prime-smartscreen', '',
         SW_HIDE, ewNoWait, ResultCode);
    Exec(ExpandConstant('{app}\ufb-agent.exe'), '--prime-smartscreen', '',
         SW_HIDE, ewNoWait, ResultCode);
    Sleep(4000);

    if WizardIsComponentSelected('firewall') then
    begin
      // Mesh sync ports. Two TCP listeners (control + data) and one
      // UDP heartbeat. v6 epoch ports: TCP 49221/49222, UDP 4265
      // (see core/src/mesh/mod.rs + utils.rs). Delete-before-add so
      // reinstalls/upgrades don't accumulate duplicate allow rules.
      Exec('netsh.exe', 'advfirewall firewall delete rule name="UFB Mesh Control (TCP 49221)"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec('netsh.exe', 'advfirewall firewall delete rule name="UFB Mesh Data (TCP 49222)"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec('netsh.exe', 'advfirewall firewall delete rule name="UFB Mesh Heartbeat (UDP 4265)"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec('netsh.exe', 'advfirewall firewall add rule name="UFB Mesh Control (TCP 49221)" dir=in action=allow protocol=TCP localport=49221 program="' + ExpandConstant('{app}\{#MyAppExeName}') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec('netsh.exe', 'advfirewall firewall add rule name="UFB Mesh Data (TCP 49222)" dir=in action=allow protocol=TCP localport=49222 program="' + ExpandConstant('{app}\{#MyAppExeName}') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec('netsh.exe', 'advfirewall firewall add rule name="UFB Mesh Heartbeat (UDP 4265)" dir=in action=allow protocol=UDP localport=4265 program="' + ExpandConstant('{app}\{#MyAppExeName}') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;

  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  UserDataDir: String;
  Response: Integer;
begin
  // usUninstall fires before any files are removed. Kill any
  // running UFB processes so the uninstaller can actually delete
  // the binaries instead of leaving behind locked files (the
  // user would then think uninstall failed / left junk behind).
  if CurUninstallStep = usUninstall then
  begin
    Exec('taskkill.exe', '/f /im {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('taskkill.exe', '/f /im ufb-agent.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(500);
  end;

  if CurUninstallStep = usPostUninstall then
  begin
    // Drop the firewall rules this version's install added.
    Exec('netsh.exe', 'advfirewall firewall delete rule name="UFB Mesh Control (TCP 49221)"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('netsh.exe', 'advfirewall firewall delete rule name="UFB Mesh Data (TCP 49222)"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('netsh.exe', 'advfirewall firewall delete rule name="UFB Mesh Heartbeat (UDP 4265)"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // Prompt to delete user data
    UserDataDir := ExpandConstant('{localappdata}\ufb');
    if DirExists(UserDataDir) then
    begin
      Response := MsgBox('Do you want to delete your user data, settings, and database?' + #13#10 +
                         'Location: ' + UserDataDir + #13#10#13#10 +
                         'Choose "Yes" for a clean uninstall.' + #13#10 +
                         'Choose "No" to keep data for future installations (RECOMMENDED).',
                         mbConfirmation, MB_YESNO);
      if Response = IDYES then
        DelTree(UserDataDir, True, True, True);
    end;
  end;
end;

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent; Tasks: launchafter

[UninstallDelete]
Type: filesandordirs; Name: "{app}\cache"
Type: filesandordirs; Name: "{app}\temp"
Type: filesandordirs; Name: "{app}\logs"

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nUnion File Browser is a file browser and project management tool designed for visual effects and post-production workflows.%n%nIt is recommended that you close all other applications before continuing.
FinishedLabel=Setup has finished installing [name] on your computer.%n%nThe application may be launched by selecting the installed shortcuts.
