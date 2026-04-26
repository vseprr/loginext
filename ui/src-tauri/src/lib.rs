mod daemon;
mod ipc_bridge;

use std::sync::Mutex;

#[tauri::command]
fn ipc_request(line: String) -> String {
    ipc_bridge::request(line)
}

#[tauri::command]
fn write_config(sensitivity: String, invert_hwheel: bool) -> Result<(), String> {
    ipc_bridge::write_config(&sensitivity, invert_hwheel)
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
