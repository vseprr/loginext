mod daemon;
mod ipc_bridge;
mod service;

use std::sync::Mutex;

// ─────────────────────────────────────────────────────────────────────
//  WebKitGTK Wayland workarounds
//
//  Tauri 2.x on Linux is WebKitGTK 6 (via webkit2gtk-4.1). On Plasma 6
//  Wayland and a handful of other modern compositors the default DMA-BUF
//  renderer trips a wl_surface protocol-error path inside GTK and the UI
//  dies before the first frame:
//
//      Gdk-Message: ... Error 71 (Protocol error) dispatching to Wayland display.
//
//  The workaround that the Tauri / WebKit ecosystem has converged on is
//  `WEBKIT_DISABLE_DMABUF_RENDERER=1`. It forces WebKit to use the
//  GLES → Wayland shm path, which costs a small amount of GPU acceleration
//  but is universally compatible. `WEBKIT_DISABLE_COMPOSITING_MODE=1` is a
//  belt-and-braces fallback for older / Nvidia-proprietary stacks where
//  the GLES path also misbehaves.
//
//  We set them ONLY when not already in the env so that:
//    • the user can still opt out (`WEBKIT_DISABLE_DMABUF_RENDERER=0`)
//    • a `.desktop` launcher that already exports them is not double-set
//    • CI / automated test paths can override freely
//
//  This must run before tauri::Builder constructs the WebView — i.e. at
//  the very top of run(), before any GTK init the Tauri runtime triggers.
fn apply_webkit_wayland_workarounds() {
    // SAFETY: env::set_var is sound when called before any thread that
    // reads the environment exists. We are still on the main thread, in
    // the very first instruction of run(), before tauri spins up its
    // worker pool. Any later set_var would be a real soundness hazard.
    let set_if_unset = |key: &str, value: &str| {
        if std::env::var_os(key).is_none() {
            // Safety contract documented above.
            unsafe { std::env::set_var(key, value); }
        }
    };
    set_if_unset("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    // Only set the compositing-mode escape on Wayland — under X11 it tends
    // to make text rendering blurry and isn't needed. Detecting Wayland by
    // env var avoids pulling in any GDK headers from this layer.
    if std::env::var_os("WAYLAND_DISPLAY").is_some() {
        set_if_unset("WEBKIT_DISABLE_COMPOSITING_MODE", "1");
    }
}

// ─────────────────────────────────────────────────────────────────────
//  KWin "LogiNext Focus Bridge" auto-enable
//
//  Pacman post-hooks run as root and cannot write to a user's kwinrc, so
//  the script files install correctly into /usr/share/kwin/scripts/ but
//  the per-user enablement flag in ~/.config/kwinrc never lands. The
//  result: `loginext-focusEnabled` stays unset, KWin never loads our
//  script, no `Activated()` calls reach the daemon, and per-app rules
//  silently fall back to the global preset (the exact symptom we kept
//  hitting).
//
//  This function papers over that gap on the UI side. Every UI launch:
//    1. Confirms we're on KDE (XDG_CURRENT_DESKTOP contains "KDE").
//    2. Runs `kwriteconfig{6,5} --file kwinrc --group Plugins --key
//       loginext-focusEnabled true`. Idempotent — already-true values
//       are no-ops, and a value of "false" *will* be flipped to true
//       (intentional: the UI is the manager of this knob; if a user
//       wants to disable it, they should disable the script in System
//       Settings → KWin Scripts, which removes the entry rather than
//       writing "false").
//    3. Asks the running KWin to reload its plugin set via
//       `qdbus{6,-qt6,} org.kde.KWin /KWin reconfigure`.
//
//  Failures at every step are best-effort and silent in the common case
//  — a single line goes to stderr so a curious user running from a
//  terminal sees what happened, but a missing tool / dead KWin doesn't
//  block the UI from starting.
fn ensure_kwin_script_enabled() {
    use std::path::{Path, PathBuf};
    use std::process::{Command, Stdio};

    let xdg = std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_default();
    if !xdg.split(':').any(|s| s.eq_ignore_ascii_case("KDE")) {
        return;
    }

    let run_silent = |tool: &str, args: &[&str]| -> bool {
        Command::new(tool)
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    };

    // Step 0 — locate a KWin script package on disk. The PKGBUILD installs
    // it system-wide; the dev workflow leaves it in the source tree. Both
    // are valid sources for kpackagetool6 to register from.
    //
    // Search order:
    //   1. /usr/share/kwin/scripts/loginext-focus  (pacman / system)
    //   2. ~/.local/share/kwin/scripts/loginext-focus  (already installed)
    //   3. <UI exe>/../../../../deploy/kwin/loginext-focus  (cargo target/release)
    //   4. <UI exe>/../../../../../deploy/kwin/loginext-focus  (cargo target/debug subdir)
    //
    // The walk-up loop in (3)/(4) lets `tauri dev` and a raw release-build
    // launch find the source without an env var override.
    let mut script_src: Option<PathBuf> = None;
    let candidates: [PathBuf; 2] = [
        PathBuf::from("/usr/share/kwin/scripts/loginext-focus"),
        std::env::var("HOME")
            .ok()
            .map(|h| PathBuf::from(format!("{h}/.local/share/kwin/scripts/loginext-focus")))
            .unwrap_or_default(),
    ];
    for c in &candidates {
        if c.join("metadata.json").is_file() {
            script_src = Some(c.clone());
            break;
        }
    }
    if script_src.is_none() {
        if let Ok(exe) = std::env::current_exe() {
            let mut p = exe.clone();
            for _ in 0..7 {
                if let Some(parent) = p.parent() {
                    let candidate = parent.join("deploy/kwin/loginext-focus");
                    if candidate.join("metadata.json").is_file() {
                        script_src = Some(candidate);
                        break;
                    }
                    p = parent.to_path_buf();
                } else {
                    break;
                }
            }
        }
    }

    // Step 1 — register the script with KPackage so KWin can find it.
    // `kpackagetool6 --upgrade` is idempotent: it overwrites a stale local
    // copy and gracefully fails over to `--install` for first-time runs.
    // Without this, even though the files are on disk in step 0's candidate
    // path, KWin's plugin scanner may not see them — that's the exact bug
    // a fresh `makepkg -si` keeps hitting on Plasma 6.
    if let Some(src) = script_src.as_deref() {
        let registered = register_kwin_script(src, &run_silent);
        if !registered {
            eprintln!(
                "[loginext-ui] kwin: kpackagetool6 not available — \
                 falling back to direct copy into ~/.local/share/kwin/scripts/."
            );
            // Direct copy is a safety net: KWin always discovers scripts
            // in the per-user XDG directory, no kpackagetool registration
            // required. Slower than kpackagetool6 (it does extra metadata
            // validation) but works on hosts where Plasma's tooling isn't
            // fully installed.
            copy_script_to_user_dir(src);
        }
    } else {
        eprintln!(
            "[loginext-ui] kwin: focus-bridge script not found in /usr/share, \
             ~/.local/share, or the source tree — install the package or \
             run from the repo root."
        );
        return;
    }

    // Step 2 — flip the enablement bit. Try the Plasma 6 tool first;
    // fall back to Plasma 5 on hosts that haven't switched yet.
    let wrote = ["kwriteconfig6", "kwriteconfig5"].iter().any(|tool| {
        run_silent(
            tool,
            &[
                "--file", "kwinrc",
                "--group", "Plugins",
                "--key", "loginext-focusEnabled", "true",
            ],
        )
    });

    if !wrote {
        eprintln!(
            "[loginext-ui] kwin: kwriteconfig{{5,6}} not found — \
             enable 'LogiNext Focus Bridge' manually via System Settings → \
             Window Management → KWin Scripts."
        );
        return;
    }

    // Step 3 — ask the running KWin to reload its plugin set. Harmless
    // if KWin isn't up (SSH first-run / headless install).
    let reconfigured = ["qdbus6", "qdbus-qt6", "qdbus"].iter().any(|tool| {
        run_silent(tool, &["org.kde.KWin", "/KWin", "reconfigure"])
    });

    eprintln!(
        "[loginext-ui] kwin: focus bridge ready{}",
        if reconfigured { " (KWin reloaded)" } else { " (KWin reload skipped — log out / in to apply)" }
    );

    fn register_kwin_script(src: &Path, run_silent: &dyn Fn(&str, &[&str]) -> bool) -> bool {
        let src_str = src.to_string_lossy().into_owned();
        for tool in ["kpackagetool6", "kpackagetool5"] {
            // --upgrade succeeds on both fresh installs (treated as install)
            // and existing packages (overwrites) on Plasma 6. Plasma 5's
            // kpackagetool5 distinguishes them, so we try --install too.
            if run_silent(tool, &["--type", "KWin/Script", "--upgrade", &src_str])
                || run_silent(tool, &["--type", "KWin/Script", "--install", &src_str])
            {
                return true;
            }
        }
        false
    }

    fn copy_script_to_user_dir(src: &Path) {
        let Ok(home) = std::env::var("HOME") else { return };
        let dst = PathBuf::from(format!(
            "{home}/.local/share/kwin/scripts/loginext-focus"
        ));
        let Some(parent) = dst.parent() else { return };
        let _ = std::fs::create_dir_all(parent);

        // Recursive copy. The script is two files (metadata.json + a tiny
        // main.js), so a hand-rolled walker is cheaper than pulling fs_extra.
        fn copy_dir(from: &Path, to: &Path) {
            let _ = std::fs::create_dir_all(to);
            let Ok(entries) = std::fs::read_dir(from) else { return };
            for entry in entries.flatten() {
                let src_path = entry.path();
                let dst_path = to.join(entry.file_name());
                if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                    copy_dir(&src_path, &dst_path);
                } else {
                    let _ = std::fs::copy(&src_path, &dst_path);
                }
            }
        }
        copy_dir(src, &dst);
    }
}

#[tauri::command]
fn ipc_request(line: String) -> String {
    ipc_bridge::request(line)
}

#[tauri::command]
fn write_config(sensitivity: String, invert_hwheel: bool, active_preset: String) -> Result<(), String> {
    ipc_bridge::write_config(&sensitivity, invert_hwheel, &active_preset)
}

/// Returns the parsed contents of `~/.config/loginext/app_rules.txt` as a
/// JSON envelope `{"ok":true,"rules":[...]}`. Missing file → empty rules.
#[tauri::command]
fn read_app_rules() -> String {
    ipc_bridge::read_app_rules()
}

/// Atomically rewrite the rules file from the supplied list. The frontend
/// then issues `request("reload")` so the daemon re-hashes the table.
/// Splitting save + reload keeps the failure modes independent — a config
/// validation error blocks the write before the daemon ever sees it.
#[tauri::command]
fn write_app_rules(rules: Vec<ipc_bridge::AppRuleEntry>) -> Result<(), String> {
    ipc_bridge::write_app_rules(&rules)
}

/// Returns a small JSON envelope describing the daemon-side bring-up. The UI
/// renders this into the status bar on first paint so the user knows whether
/// the backend was already running, freshly spawned, or failed.
#[tauri::command]
fn daemon_status(state: tauri::State<'_, DaemonStartup>) -> String {
    let outcome = state.0.lock().expect("daemon-startup mutex poisoned");
    match &*outcome {
        daemon::SpawnOutcome::AlreadyRunning => {
            r#"{"ok":true,"state":"already_running"}"#.to_string()
        }
        daemon::SpawnOutcome::Spawned { pid } => {
            format!(r#"{{"ok":true,"state":"spawned","pid":{pid}}}"#)
        }
        daemon::SpawnOutcome::SpawnFailed { reason } => {
            let escaped = reason.replace('\\', "\\\\").replace('"', "\\\"");
            format!(r#"{{"ok":false,"state":"spawn_failed","err":"{escaped}"}}"#)
        }
        daemon::SpawnOutcome::BinaryNotFound => {
            r#"{"ok":false,"state":"binary_not_found","err":"loginext binary not found"}"#
                .to_string()
        }
        daemon::SpawnOutcome::Timeout => {
            r#"{"ok":false,"state":"timeout","err":"daemon did not open socket in time"}"#
                .to_string()
        }
    }
}

/// Locate and terminate the running `loginext` daemon. Used by the
/// "SYSTEM ONLINE/OFFLINE" toggle in the UI to honour explicit user intent
/// to take the daemon down. SIGTERM first; SIGKILL only if the daemon
/// ignores TERM past the grace window. Updates the cached startup state so
/// `daemon_status` reflects reality without a respawn.
#[tauri::command]
fn kill_daemon(state: tauri::State<'_, DaemonStartup>) -> String {
    let outcome = daemon::kill_daemon();
    let payload = match &outcome {
        daemon::KillOutcome::Killed { pid } => {
            format!(r#"{{"ok":true,"state":"killed","pid":{pid}}}"#)
        }
        daemon::KillOutcome::NotRunning => {
            r#"{"ok":true,"state":"not_running"}"#.to_string()
        }
        daemon::KillOutcome::SignalFailed { reason } => {
            let e = reason.replace('\\', "\\\\").replace('"', "\\\"");
            format!(r#"{{"ok":false,"state":"signal_failed","err":"{e}"}}"#)
        }
        daemon::KillOutcome::Timeout => {
            r#"{"ok":false,"state":"timeout","err":"daemon ignored SIGTERM and SIGKILL"}"#
                .to_string()
        }
    };
    // The cached SpawnOutcome is no longer authoritative once the user has
    // explicitly killed the daemon — the front-end's intent flag + heartbeat
    // are the source of truth from this point. We deliberately leave the
    // cache alone rather than invent a misleading "down" SpawnOutcome
    // variant; the next daemon_respawn() refreshes it cleanly.
    let _ = state;
    payload
}

/// Read the systemd-user state of `loginext.service`. Used by the
/// status toggle on UI launch to set its position from the source of
/// truth (systemd) rather than a localStorage flag, and on every
/// heartbeat tick so an externally-issued `systemctl --user stop/start`
/// is reflected in the UI within ~5 s.
#[tauri::command]
fn service_state() -> String {
    let s = service::query_state();
    format!(
        r#"{{"ok":true,"available":{a},"active":{ac},"enabled":{en}}}"#,
        a  = s.available,
        ac = s.active,
        en = s.enabled,
    )
}

/// `systemctl --user enable --now loginext.service` — turn the service
/// on AND install the autostart symlink. The frontend calls this from
/// the DAEMON OFFLINE→ONLINE click. Errors come back verbatim so the
/// user can act on them (missing unit file is the most common one).
#[tauri::command]
fn service_enable() -> String {
    match service::enable_now() {
        Ok(()) => {
            // Re-query state so the UI's optimistic update matches systemd's
            // observed state after the operation lands. If `enable --now`
            // fails midway (e.g. ExecStart binary missing), the unit is
            // enabled-but-failed; surfacing both flags lets the toast
            // explain what happened rather than asserting success.
            let s = service::query_state();
            format!(
                r#"{{"ok":true,"state":"enabled","available":{a},"active":{ac},"enabled":{en}}}"#,
                a  = s.available,
                ac = s.active,
                en = s.enabled,
            )
        }
        Err(reason) => {
            let e = reason.replace('\\', "\\\\").replace('"', "\\\"");
            format!(r#"{{"ok":false,"state":"enable_failed","err":"{e}"}}"#)
        }
    }
}

/// `systemctl --user disable --now loginext.service` — stop the service
/// AND remove the autostart symlink. The frontend calls this from the
/// DAEMON ONLINE→OFFLINE click.
#[tauri::command]
fn service_disable() -> String {
    match service::disable_now() {
        Ok(()) => {
            let s = service::query_state();
            format!(
                r#"{{"ok":true,"state":"disabled","available":{a},"active":{ac},"enabled":{en}}}"#,
                a  = s.available,
                ac = s.active,
                en = s.enabled,
            )
        }
        Err(reason) => {
            let e = reason.replace('\\', "\\\\").replace('"', "\\\"");
            format!(r#"{{"ok":false,"state":"disable_failed","err":"{e}"}}"#)
        }
    }
}

/// Re-runs the spawn check on demand. The UI calls this from the status-bar
/// reconnect path when the heartbeat reports the daemon went away while the
/// window was open. Cheap when the socket is alive (single connect+close).
#[tauri::command]
fn daemon_respawn(state: tauri::State<'_, DaemonStartup>) -> String {
    let outcome = daemon::ensure_running();
    let payload = match &outcome {
        daemon::SpawnOutcome::AlreadyRunning => r#"{"ok":true,"state":"already_running"}"#.to_string(),
        daemon::SpawnOutcome::Spawned { pid } => format!(r#"{{"ok":true,"state":"spawned","pid":{pid}}}"#),
        daemon::SpawnOutcome::SpawnFailed { reason } => {
            let e = reason.replace('\\', "\\\\").replace('"', "\\\"");
            format!(r#"{{"ok":false,"state":"spawn_failed","err":"{e}"}}"#)
        }
        daemon::SpawnOutcome::BinaryNotFound => {
            r#"{"ok":false,"state":"binary_not_found","err":"loginext binary not found"}"#.to_string()
        }
        daemon::SpawnOutcome::Timeout => {
            r#"{"ok":false,"state":"timeout","err":"daemon did not open socket in time"}"#.to_string()
        }
    };
    if let Ok(mut slot) = state.0.lock() {
        *slot = outcome;
    }
    payload
}

/// Wraps the initial spawn outcome so it lives for the duration of the Tauri
/// app and can be queried by the `daemon_status` command. Updated whenever
/// `daemon_respawn` runs.
struct DaemonStartup(Mutex<daemon::SpawnOutcome>);

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // Apply WebKitGTK Wayland workarounds first — these MUST land before
    // the Tauri runtime triggers any GTK initialisation, otherwise WebKit
    // has already chosen a renderer and the env var is read too late.
    apply_webkit_wayland_workarounds();

    // Make sure the KDE focus bridge is enabled in the user's kwinrc.
    // This is the producer side of the per-app rule pipeline, and
    // pacman's post-install hook can't reach a user's $HOME, so without
    // this step a fresh package install leaves per-app rules silently
    // dead until the user runs `kwriteconfig6` by hand. Cheap (a couple
    // of subprocess calls), idempotent, and gated on KDE.
    ensure_kwin_script_enabled();

    // Spawn-or-detect runs on the main thread before the window opens — keeps
    // the first frame coherent with the daemon state we report. The probe is
    // fast (single connect attempt) when the daemon is already up.
    let initial = daemon::ensure_running();

    eprintln!(
        "[loginext-ui] socket: {}",
        daemon::socket_path().display()
    );

    let result = tauri::Builder::default()
        .manage(DaemonStartup(Mutex::new(initial)))
        .invoke_handler(tauri::generate_handler![
            ipc_request,
            write_config,
            read_app_rules,
            write_app_rules,
            daemon_status,
            daemon_respawn,
            kill_daemon,
            service_state,
            service_enable,
            service_disable,
        ])
        .run(tauri::generate_context!());

    // Don't panic on Tauri runtime errors — log + exit non-zero so a launcher
    // surfaces the failure cleanly rather than leaving a SIGABRT corefile.
    if let Err(e) = result {
        eprintln!("[loginext-ui] tauri runtime failed: {e}");
        std::process::exit(1);
    }
}
