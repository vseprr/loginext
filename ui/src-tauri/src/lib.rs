mod daemon;
mod ipc_bridge;

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
    use std::process::{Command, Stdio};

    let xdg = std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_default();
    if !xdg.split(':').any(|s| s.eq_ignore_ascii_case("KDE")) {
        return;
    }

    // Step 1 — flip the enablement bit. Try the Plasma 6 tool first;
    // fall back to Plasma 5 on hosts that haven't switched yet.
    let mut wrote = false;
    for tool in ["kwriteconfig6", "kwriteconfig5"] {
        match Command::new(tool)
            .args([
                "--file", "kwinrc",
                "--group", "Plugins",
                "--key", "loginext-focusEnabled", "true",
            ])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
        {
            Ok(s) if s.success() => {
                wrote = true;
                break;
            }
            _ => continue,
        }
    }

    if !wrote {
        eprintln!(
            "[loginext-ui] kwin: kwriteconfig{{5,6}} not found — \
             enable 'LogiNext Focus Bridge' manually via System Settings → \
             Window Management → KWin Scripts."
        );
        return;
    }

    // Step 2 — ask KWin to reload its plugin set. If KWin isn't running
    // (headless install / SSH first-run), the call fails and is harmless;
    // the change picks up at the next session login.
    let mut reconfigured = false;
    for tool in ["qdbus6", "qdbus-qt6", "qdbus"] {
        match Command::new(tool)
            .args(["org.kde.KWin", "/KWin", "reconfigure"])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
        {
            Ok(s) if s.success() => {
                reconfigured = true;
                break;
            }
            _ => continue,
        }
    }

    eprintln!(
        "[loginext-ui] kwin: ensured loginext-focusEnabled=true{}",
        if reconfigured { " (KWin reloaded)" } else { " (KWin reload skipped — log out / in to apply)" }
    );
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
        ])
        .run(tauri::generate_context!());

    // Don't panic on Tauri runtime errors — log + exit non-zero so a launcher
    // surfaces the failure cleanly rather than leaving a SIGABRT corefile.
    if let Err(e) = result {
        eprintln!("[loginext-ui] tauri runtime failed: {e}");
        std::process::exit(1);
    }
}
