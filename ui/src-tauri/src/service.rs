//! systemd --user service control for the LogiNext daemon.
//!
//! The DAEMON ONLINE/OFFLINE toggle in the UI used to drive the spawn/kill
//! IPC pair directly. That worked fine while the daemon was always launched
//! detached from the UI, but it had two ergonomic gaps:
//!
//!   1. The toggle did not survive logout. A user who left it ON had to
//!      reopen the UI after every reboot to spawn the daemon again.
//!   2. The toggle did not coordinate with `systemctl --user enable`.
//!      Power users who'd manually enabled the unit kept finding it
//!      stopped because the UI's spawn-detached path didn't notice it.
//!
//! Both issues collapse if the toggle is the *systemd* enable bit. ON →
//! `systemctl --user enable --now loginext.service` (starts now AND
//! installs the WantedBy symlink so it autostarts at next login). OFF →
//! `systemctl --user disable --now loginext.service` (stops now AND
//! removes the symlink). State on UI launch is read with
//! `is-enabled` + `is-active`.
//!
//! Failures are reported as JSON envelopes the frontend can render —
//! a missing service file is the most common one and we surface it
//! explicitly so the user knows to run `deploy/install.sh` or reinstall
//! the package.

use std::process::{Command, Stdio};

const UNIT_NAME: &str = "loginext.service";

/// What the UI needs to render the toggle correctly: is the unit
/// installed at all, is it currently running, and would systemd start
/// it at next login? `available = false` means the systemctl call
/// failed because the unit file isn't on the user's unit search path
/// — almost always a missing `loginext.service` symlink (fresh install
/// without the systemd unit copied / package install missed).
#[derive(Debug, Clone, Default)]
pub struct ServiceState {
    pub available: bool,
    pub active:    bool,
    pub enabled:   bool,
}

fn run_unit_query(arg: &str) -> Option<String> {
    // `is-enabled` / `is-active` exit non-zero on "no", but they still
    // print one of {active, inactive, enabled, disabled, …, not-found,
    // not-loaded} on stdout. We capture stdout regardless of exit code
    // so we can distinguish "unit absent" from "unit present but off".
    let out = Command::new("systemctl")
        .args(["--user", arg, UNIT_NAME])
        .stdin(Stdio::null())
        .output()
        .ok()?;
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if s.is_empty() { None } else { Some(s) }
}

/// Probe systemd-user for the LogiNext service's current install +
/// activation state. Cheap (two short subprocess calls; systemctl
/// short-circuits when its unit cache is warm).
pub fn query_state() -> ServiceState {
    let active_word  = run_unit_query("is-active");
    let enabled_word = run_unit_query("is-enabled");

    // systemctl reports "not-found" / "not-loaded" / "masked" / "static"
    // / "alias" when the unit isn't a normal user-installable service.
    // For our purposes, anything that isn't the affirmative {active,
    // enabled} word is treated as "not on" — we don't need the finer
    // taxonomy.
    let not_present = |word: &Option<String>| -> bool {
        match word.as_deref() {
            None => true,
            Some(w) => matches!(
                w,
                "not-found" | "not-loaded" | "" | "no-such-unit" | "no" | "unknown"
            ),
        }
    };

    let available = !(not_present(&active_word) && not_present(&enabled_word));

    ServiceState {
        available,
        active:  matches!(active_word.as_deref(),  Some("active") | Some("activating") | Some("reloading")),
        enabled: matches!(enabled_word.as_deref(), Some("enabled") | Some("alias") | Some("static")),
    }
}

/// Run a systemctl --user command and return Ok(()) on success or the
/// captured stderr on failure. We capture stderr because systemctl's
/// failure messages are the only thing that lets us tell the user what
/// to do (missing unit file, polkit refusal, daemon-reload required).
fn run_systemctl(args: &[&str]) -> Result<(), String> {
    let out = Command::new("systemctl")
        .args(["--user"].iter().chain(args.iter()).copied().collect::<Vec<_>>())
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("systemctl spawn failed: {e}"))?;
    if out.status.success() {
        return Ok(());
    }
    let err = String::from_utf8_lossy(&out.stderr).trim().to_string();
    Err(if err.is_empty() {
        format!("systemctl exited with status {}", out.status)
    } else {
        err
    })
}

/// `systemctl --user enable --now loginext.service`
///
/// Starts the daemon and installs the `WantedBy=default.target` symlink
/// so the daemon autostarts at next login. If the unit file is missing,
/// the error is surfaced verbatim — the frontend renders it in a toast
/// so the user immediately knows whether to copy the unit or reinstall.
pub fn enable_now() -> Result<(), String> {
    // `daemon-reload` is cheap and only matters the first time a freshly
    // installed unit file is being picked up; running it always means
    // post-package-install enables succeed without a manual reload step.
    let _ = run_systemctl(&["daemon-reload"]);
    run_systemctl(&["enable", "--now", UNIT_NAME])
}

/// `systemctl --user disable --now loginext.service`
///
/// Stops the daemon and removes the autostart symlink. The IPC `quit`
/// path the toggle previously used is now redundant — systemd handles
/// SIGTERM delivery itself, including the cooperative-stop semantics.
pub fn disable_now() -> Result<(), String> {
    run_systemctl(&["disable", "--now", UNIT_NAME])
}
