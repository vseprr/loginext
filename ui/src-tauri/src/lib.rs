mod daemon;
mod ipc_bridge;
mod service;

use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;

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

    // Step 0 — locate a KWin script package on disk.
    //
    // CRITICAL: `kpackagetool6 --upgrade` and `--install` install INTO
    // the user's `~/.local/share/kwin/scripts/` directory. If we pass
    // the user-local path AS the source, source == destination and
    // kpackagetool can wipe the directory before re-copying — which
    // is exactly what was happening: the script appeared after
    // install.sh, then disappeared the first time the UI ran. So we
    // search for kpackagetool sources ONLY in immutable, non-destination
    // locations:
    //
    //   1. /usr/share/kwin/scripts/loginext-focus  (pacman / system)
    //   2. <UI exe>/../../../../deploy/kwin/loginext-focus  (cargo target/release)
    //   3. <UI exe>/../../../../../deploy/kwin/loginext-focus  (cargo target/debug subdir)
    //
    // Separately, we ALSO check whether files are already present in the
    // user-local path (e.g. install.sh's fallback copy). If so, KWin
    // will discover them on its own — no kpackagetool registration
    // needed — and we just need to flip the `loginext-focusEnabled`
    // bit and reconfigure KWin in step 2.
    let mut script_src: Option<PathBuf> = None;
    let system_path = PathBuf::from("/usr/share/kwin/scripts/loginext-focus");
    if system_path.join("metadata.json").is_file() {
        script_src = Some(system_path);
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

    // Detect a pre-existing user-local install separately. Files at this
    // path are KWin-discoverable on their own — we never pass this path
    // to kpackagetool (would corrupt it; see comment above).
    let user_local_present = std::env::var("HOME")
        .ok()
        .map(|h| {
            PathBuf::from(format!(
                "{h}/.local/share/kwin/scripts/loginext-focus/metadata.json"
            ))
            .is_file()
        })
        .unwrap_or(false);

    // Step 1 — register the script with KPackage so KWin can find it.
    // Skipped when we have a pre-existing user-local install AND no
    // separate immutable source: nothing to register, files are already
    // where KWin reads them. When we do have an immutable source,
    // kpackagetool registration is preferred (it primes the KPackage
    // database, which makes KWin's plugin scanner see the script
    // without a logout/login cycle).
    match (script_src.as_deref(), user_local_present) {
        (Some(src), _) => {
            let registered = register_kwin_script(src, &run_silent);
            if !registered {
                eprintln!(
                    "[loginext-ui] kwin: kpackagetool6 invocation failed — \
                     {} either not installed or refused both --upgrade and --install. \
                     {}",
                    "kpackage",
                    if user_local_present {
                        "Files already present in ~/.local/share/kwin/scripts/loginext-focus, leaving as-is."
                    } else {
                        "Falling back to direct copy into ~/.local/share/kwin/scripts/."
                    }
                );
                if !user_local_present {
                    // Only copy when nothing exists at the destination.
                    // Don't overwrite a pre-existing install — a previous
                    // install.sh run owns it.
                    copy_script_to_user_dir(src);
                }
            }
        }
        (None, true) => {
            eprintln!(
                "[loginext-ui] kwin: using existing user-local install at \
                 ~/.local/share/kwin/scripts/loginext-focus (no kpackagetool source available)"
            );
        }
        (None, false) => {
            eprintln!(
                "[loginext-ui] kwin: focus-bridge script not found in /usr/share, \
                 ~/.local/share, or the source tree — install the package or \
                 run from the repo root."
            );
            return;
        }
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

/// Toggle the LogiNext window's always-on-top flag. Wired to the pin
/// button in the UI header so the user can keep the LogiNext window
/// visible while clicking around their other apps to set up per-app
/// rules. Without this, focusing a target app sends LogiNext to the
/// background, and the user can't reach the "+ Add rule" button
/// without manually tiling or invoking the WM's "Keep Above" shortcut
/// — the "window-focus paradox" the per-app rule UX otherwise has.
///
/// The frontend remembers the last value in localStorage and re-applies
/// it on every launch (see `attachAlwaysOnTopPin` in main.ts), so the
/// preference survives across UI restarts without needing a Tauri-side
/// store.
///
/// Two-tier implementation:
///   1. Tauri's `Window::set_always_on_top()` — works on X11 and
///      compositors that honour the xdg_toplevel "always on top" hint.
///   2. KWin D-Bus scripting fallback — on KDE Plasma Wayland, KWin
///      may ignore the GTK-requested hint. We call KWin's
///      `org.kde.kwin.Scripting.runScript` to set `keepAbove` directly
///      inside KWin's own JavaScript engine, which has authority over
///      window stacking. The snippet finds the LogiNext window by
///      matching the window title.
#[tauri::command]
fn set_always_on_top(window: tauri::Window, on_top: bool) -> Result<(), String> {
    // Tier 1: Tauri's native always-on-top. Works on X11 and
    // compositors that honour the xdg_toplevel "always on top" hint.
    //
    // CRITICAL: we do NOT bail on failure here. Tauri 2's
    // `Window::set_always_on_top` on Wayland goes through
    // `gtk_window_set_keep_above`, which some Wayland backends report
    // as an error even though the D-Bus fallback below would have
    // succeeded. The previous implementation used `?` here and
    // short-circuited the entire function, which is what caused the
    // frontend to see "no effect" AND the KWin journal to show no
    // D-Bus activity — the Rust code simply never reached Tier 2.
    match window.set_always_on_top(on_top) {
        Ok(()) => {
            eprintln!("[loginext-ui] pin: Tauri set_always_on_top({on_top}) ok");
        }
        Err(e) => {
            eprintln!(
                "[loginext-ui] pin: Tauri set_always_on_top({on_top}) failed: {e} \
                 — falling through to KWin D-Bus fallback"
            );
        }
    }

    // Tier 2: KWin D-Bus scripting. On KDE Plasma (Wayland in
    // particular) KWin owns window stacking and the GTK keep-above
    // hint is advisory at best. We invoke KWin's own scripting engine
    // via D-Bus — the same mechanism the "Keep Above" window-menu
    // action uses internally — which has authority over the stacking
    // order that no client-side hint does.
    let xdg = std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_default();
    let on_kde = xdg.split(':').any(|s| s.eq_ignore_ascii_case("KDE"));
    if !on_kde {
        return Ok(());
    }

    let title = window.title().unwrap_or_else(|_| "LogiNext".into());
    // Escape backslashes and quotes so the JS string literal is safe.
    let escaped_title = title.replace('\\', "\\\\").replace('"', "\\\"");
    let keep_above = if on_top { "true" } else { "false" };

    // The JS runs once at `Script.run()` time. It matches by the two
    // properties that KWin reliably exposes on both Plasma 5 and 6:
    // `caption` (window title) and `resourceClass` (WM_CLASS /
    // xdg_toplevel app_id). Matching on either widens the catch: a
    // Wayland session reports `loginext-ui` as resourceClass even when
    // the caption is blank during early startup, and X11 sessions set
    // the caption to the window title string.
    let js = format!(
        "(function() {{\n\
           var list;\n\
           try {{ list = workspace.windowList(); }}\n\
           catch (e) {{ try {{ list = workspace.clientList(); }} catch (e2) {{ list = []; }} }}\n\
           for (var i = 0; i < list.length; i++) {{\n\
             var w = list[i];\n\
             if (!w) continue;\n\
             var cap = (w.caption || '') + '';\n\
             var klass = ((w.resourceClass || '') + '').toLowerCase();\n\
             if (cap === \"{escaped_title}\" || klass.indexOf('loginext') !== -1) {{\n\
               w.keepAbove = {keep_above};\n\
             }}\n\
           }}\n\
         }})();\n"
    );

    // KWin's `org.kde.kwin.Scripting.loadScript(path, name)` takes a
    // file path and returns an integer script id. We deliberately use
    // this path (rather than the `runScript(QString)` inline variant)
    // because `runScript` has been inconsistent across Plasma 6 point
    // releases — some builds require a privileged caller, others have
    // dropped it entirely. `loadScript` is the public, documented API
    // and has worked unchanged since Plasma 5.x.
    //
    // Temp file lives under $XDG_RUNTIME_DIR so it's per-user, tmpfs,
    // and auto-cleaned by systemd on logout. Fall back to /tmp if
    // XDG_RUNTIME_DIR isn't set (e.g. a tty-only session).
    let xdg_runtime = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    let pid = std::process::id();
    let tmp_path = format!("{xdg_runtime}/loginext-keepabove-{pid}.js");
    if let Err(e) = std::fs::write(&tmp_path, &js) {
        eprintln!("[loginext-ui] pin: could not write {tmp_path}: {e}");
        return Ok(());
    }

    // Small helper — run a qdbus invocation, return (success, stderr).
    // We try Plasma 6's `qdbus6` first, then the legacy `qdbus-qt6`
    // alias some distros ship, then plain `qdbus`. All three speak the
    // same D-Bus wire protocol; the only difference is which Qt they
    // were built against and hence which meta-object cache they use.
    let tools = ["qdbus6", "qdbus-qt6", "qdbus"];
    let run_qdbus = |args: &[&str]| -> (bool, String) {
        let mut last_err = String::new();
        for tool in &tools {
            let out = Command::new(tool)
                .args(args)
                .stdin(Stdio::null())
                .output();
            match out {
                Ok(o) if o.status.success() => {
                    let stdout = String::from_utf8_lossy(&o.stdout).trim().to_string();
                    eprintln!(
                        "[loginext-ui] pin: {tool} {:?} ok (stdout={stdout:?})",
                        args
                    );
                    return (true, stdout);
                }
                Ok(o) => {
                    last_err = format!(
                        "{tool} exit={}: stderr={}",
                        o.status,
                        String::from_utf8_lossy(&o.stderr).trim()
                    );
                }
                Err(e) => {
                    last_err = format!("{tool} spawn: {e}");
                }
            }
        }
        (false, last_err)
    };

    // Step 1: load the script. Returns the script id as the stdout payload.
    let (ok, load_out) = run_qdbus(&[
        "org.kde.KWin",
        "/Scripting",
        "org.kde.kwin.Scripting.loadScript",
        &tmp_path,
    ]);
    if !ok {
        eprintln!("[loginext-ui] pin: KWin loadScript failed ({load_out}) — temp file was {tmp_path}");
        let _ = std::fs::remove_file(&tmp_path);
        return Ok(());
    }

    // KWin returns the numeric id on stdout; newer Plasma versions
    // print it bare, older ones wrap it with whitespace. Extract the
    // first integer token and use it to build the per-script object
    // path that `run`/`stop` live on.
    let id = load_out.split_whitespace().next().unwrap_or("");
    if id.is_empty() || id.parse::<i32>().is_err() {
        eprintln!(
            "[loginext-ui] pin: KWin loadScript returned non-integer id {load_out:?} — \
             temp file kept at {tmp_path} for inspection"
        );
        return Ok(());
    }
    let script_path = format!("/Scripting/Script{id}");

    // Step 2: run it. This is what actually applies keepAbove inside
    // KWin's QJSEngine — loadScript on its own is just a registration.
    let (ran, run_out) = run_qdbus(&[
        "org.kde.KWin",
        &script_path,
        "org.kde.kwin.Script.run",
    ]);
    if !ran {
        eprintln!("[loginext-ui] pin: KWin Script.run on {script_path} failed ({run_out})");
    }

    // Step 3: stop + remove so we don't leak KWin's script registry
    // entries across repeated toggles. Failures here are non-fatal —
    // Plasma cleans these up on logout anyway, but a tidy run makes
    // `dbus-monitor` output readable while debugging.
    let _ = run_qdbus(&[
        "org.kde.KWin",
        &script_path,
        "org.kde.kwin.Script.stop",
    ]);
    let _ = std::fs::remove_file(&tmp_path);

    Ok(())
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

// Legacy spawn-mode lifecycle command — retained for diagnostics. The UI's
// toggle no longer calls this; daemon lifecycle is owned by systemd.
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

// Legacy spawn-mode lifecycle command — retained for diagnostics. The UI's
// toggle no longer calls this; daemon lifecycle is owned by systemd.
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

/// `systemctl --user start loginext.service` (no enable). Used internally
/// by run() when the unit is enabled but not active, and exposed to the UI
/// for transient start/stop without touching autostart state. Heals the
/// unit body first if it's stale (same logic as service_enable).
#[tauri::command]
fn service_start() -> String {
    match service::start_only() {
        Ok(()) => {
            let s = service::query_state();
            format!(
                r#"{{"ok":true,"state":"started","available":{a},"active":{ac},"enabled":{en}}}"#,
                a = s.available, ac = s.active, en = s.enabled,
            )
        }
        Err(reason) => {
            let e = reason.replace('\\', "\\\\").replace('"', "\\\"");
            format!(r#"{{"ok":false,"state":"start_failed","err":"{e}"}}"#)
        }
    }
}

/// `systemctl --user stop loginext.service` (no disable). Stops without
/// removing the autostart symlink — useful for "pause this session" without
/// losing the boot-time autostart setting.
#[tauri::command]
fn service_stop() -> String {
    match service::stop_only() {
        Ok(()) => {
            let s = service::query_state();
            format!(
                r#"{{"ok":true,"state":"stopped","available":{a},"active":{ac},"enabled":{en}}}"#,
                a = s.available, ac = s.active, en = s.enabled,
            )
        }
        Err(reason) => {
            let e = reason.replace('\\', "\\\\").replace('"', "\\\"");
            format!(r#"{{"ok":false,"state":"stop_failed","err":"{e}"}}"#)
        }
    }
}

/// Returns a JSON envelope describing how to install the systemd unit.
/// Called by the UI's first-run wizard when service_state reports
/// available=false. The shell command is the canonical install.sh path
/// at the absolute repo location resolvable from the UI binary; if we
/// can't resolve it, fall back to a generic "run ./deploy/install.sh".
#[tauri::command]
fn service_install_hint() -> String {
    // Try to resolve the repo root by walking up from the UI exe.
    // Same algorithm as daemon::resolve_daemon_binary's dev-workflow
    // branch — looks for a deploy/install.sh sibling.
    let path = std::env::current_exe()
        .ok()
        .and_then(|exe| {
            let mut p = exe.clone();
            for _ in 0..7 {
                if let Some(parent) = p.parent() {
                    let candidate = parent.join("deploy/install.sh");
                    if candidate.is_file() {
                        return Some(candidate);
                    }
                    p = parent.to_path_buf();
                } else {
                    break;
                }
            }
            None
        });

    let cmd = path
        .as_ref()
        .map(|p| format!("bash {}", p.display()))
        .unwrap_or_else(|| "./deploy/install.sh".to_string());
    let escaped = cmd.replace('\\', "\\\\").replace('"', "\\\"");
    format!(r#"{{"ok":true,"command":"{escaped}"}}"#)
}

/// Read the last `lines` lines of the daemon log file (default 100 if 0
/// passed). The log lives at `$XDG_STATE_HOME/loginext/daemon.log`
/// (fallback: `$HOME/.local/state/loginext/daemon.log`). Returns a JSON
/// envelope. The body is escaped so it can be safely embedded in another
/// JSON document for clipboard-copy.
///
/// Tail-from-end implementation: open the file, seek to end, read backwards
/// until N newlines counted, then return that suffix. Bounded by a max
/// of 64 KiB even when the user asks for many lines — bug reports stay
/// readable on GitHub Issues.
#[tauri::command]
fn read_daemon_log(lines: u32) -> String {
    use std::io::{Read, Seek, SeekFrom};

    let n = if lines == 0 { 100 } else { lines.min(2000) };
    let path = match resolve_daemon_log_path() {
        Some(p) => p,
        None => {
            return r#"{"ok":false,"err":"could not resolve daemon log path"}"#.to_string();
        }
    };

    let mut file = match std::fs::File::open(&path) {
        Ok(f) => f,
        Err(e) => {
            let msg = format!(r#"could not open {}: {}"#, path.display(), e);
            let esc = msg.replace('\\', "\\\\").replace('"', "\\\"");
            return format!(r#"{{"ok":false,"err":"{esc}"}}"#);
        }
    };

    // Tail-from-end: read up to 64 KiB from end, then keep only the last
    // `n` lines. Avoids loading a multi-MB log into memory.
    const MAX_BYTES: u64 = 64 * 1024;
    let len = file.seek(SeekFrom::End(0)).unwrap_or(0);
    let start = len.saturating_sub(MAX_BYTES);
    let _ = file.seek(SeekFrom::Start(start));

    let mut buf = String::new();
    if file.read_to_string(&mut buf).is_err() {
        // Log may contain non-UTF8 bytes — read as bytes + lossy convert.
        let _ = file.seek(SeekFrom::Start(start));
        let mut bytes = Vec::new();
        let _ = file.read_to_end(&mut bytes);
        buf = String::from_utf8_lossy(&bytes).into_owned();
    }

    // Trim to last N lines.
    let lines_vec: Vec<&str> = buf.lines().collect();
    let take_from = lines_vec.len().saturating_sub(n as usize);
    let tail = lines_vec[take_from..].join("\n");

    let esc = tail.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n");
    format!(r#"{{"ok":true,"path":"{}","lines":{},"body":"{}"}}"#,
        path.display().to_string().replace('\\', "\\\\").replace('"', "\\\""),
        (n as usize).min(lines_vec.len()),
        esc,
    )
}

fn resolve_daemon_log_path() -> Option<std::path::PathBuf> {
    use std::path::PathBuf;
    if let Ok(xdg) = std::env::var("XDG_STATE_HOME") {
        if !xdg.is_empty() {
            return Some(PathBuf::from(format!("{xdg}/loginext/daemon.log")));
        }
    }
    let home = std::env::var("HOME").ok()?;
    Some(PathBuf::from(format!("{home}/.local/state/loginext/daemon.log")))
}

/// Returns a JSON envelope describing the host environment relevant to a
/// LogiNext bug report: OS, kernel, compositor, loginext version, systemd
/// service state, KWin focus-bridge enablement bit. Each field is a
/// best-effort probe — if a tool isn't installed (kreadconfig6, uname),
/// the field is reported as `unknown` rather than failing the whole
/// response.
#[tauri::command]
fn system_info() -> String {
    fn run(tool: &str, args: &[&str]) -> Option<String> {
        let out = Command::new(tool)
            .args(args)
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .output()
            .ok()?;
        let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if s.is_empty() { None } else { Some(s) }
    }

    let kernel = run("uname", &["-r"]).unwrap_or_else(|| "unknown".into());
    let os = std::fs::read_to_string("/etc/os-release").ok()
        .and_then(|s| s.lines()
            .find(|l| l.starts_with("PRETTY_NAME="))
            .map(|l| l.trim_start_matches("PRETTY_NAME=")
                     .trim_matches('"').to_string()))
        .unwrap_or_else(|| "unknown".into());
    let xdg_session = std::env::var("XDG_SESSION_TYPE").unwrap_or_else(|_| "unknown".into());
    let compositor = std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_else(|_| "unknown".into());
    let wayland = if std::env::var("WAYLAND_DISPLAY").is_ok() { "yes" } else { "no" };

    let svc = service::query_state();

    // KWin focus-bridge enablement bit. Returns "true" / "false" / "unknown".
    let kwin_bridge = ["kreadconfig6", "kreadconfig5"].iter().find_map(|tool| {
        run(tool, &["--file", "kwinrc", "--group", "Plugins",
                    "--key", "loginext-focusEnabled"])
    }).unwrap_or_else(|| "unknown".into());

    // Daemon binary version — for now hard-coded to match the C++
    // LOGINEXT_VERSION constant set in main.cpp. A future wave can
    // wire this through CMake configure_file so the two can't drift.
    let daemon_version = "1.1.0";

    let esc = |s: &str| s.replace('\\', "\\\\").replace('"', "\\\"");
    format!(
        r#"{{"ok":true,"os":"{}","kernel":"{}","compositor":"{}","session":"{}","wayland":"{}","loginext_version":"{}","service_available":{},"service_active":{},"service_enabled":{},"kwin_focus_bridge":"{}"}}"#,
        esc(&os),
        esc(&kernel),
        esc(&compositor),
        esc(&xdg_session),
        wayland,
        daemon_version,
        svc.available,
        svc.active,
        svc.enabled,
        esc(&kwin_bridge),
    )
}

/// Write the supplied text to the system clipboard. Tries `wl-copy` first
/// (Wayland), then `xclip`, then `xsel`. Returns a JSON envelope. We
/// shell out rather than depend on Tauri's clipboard plugin to keep this
/// shippable inside the existing Tauri 2.x setup without adding crates.
#[tauri::command]
fn copy_to_clipboard(text: String) -> String {
    use std::io::Write;

    let tools: &[(&str, &[&str])] = &[
        ("wl-copy", &[]),
        ("xclip",   &["-selection", "clipboard"]),
        ("xsel",    &["--clipboard", "--input"]),
    ];

    for (tool, args) in tools {
        let mut child = match Command::new(tool)
            .args(*args)
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(c) => c,
            Err(_) => continue,
        };
        if let Some(stdin) = child.stdin.as_mut() {
            if stdin.write_all(text.as_bytes()).is_err() {
                continue;
            }
        }
        if child.wait().map(|s| s.success()).unwrap_or(false) {
            return format!(r#"{{"ok":true,"tool":"{}"}}"#, tool);
        }
    }
    r#"{"ok":false,"err":"no clipboard tool available (install wl-clipboard, xclip, or xsel)"}"#.to_string()
}

/// Probe `/proc/bus/input/devices` for a supported Logitech device. Used by
/// the UI's Devices column when the daemon is OFF — the daemon-side
/// `list_devices` IPC isn't available, but the UI should still tell the
/// user whether their hardware is plugged in (rather than always showing
/// a hard-coded "MX Master 3S" placeholder that's wrong on systems where
/// the device isn't actually present).
///
/// `/proc/bus/input/devices` is the kernel-exported, world-readable
/// canonical source. Each block looks like:
///
///     I: Bus=0003 Vendor=046d Product=c548 Version=0111
///     N: Name="Logitech USB Receiver Mouse"
///     ...
///
/// We match Logitech (0x046d) + one of the known MX Master 3S
/// product ids (0xb034 Bolt, 0xc548 USB) and return the human Name
/// string. Returns `{"ok":true,"found":false}` when no supported device
/// is enumerated — UI surfaces this as a "no device detected" warning.
#[tauri::command]
fn detect_logitech_device() -> String {
    // Known supported product ids — keep in lockstep with
    // src/core/device.cpp and deploy/udev/99-loginext.rules.
    const LOGITECH_VENDOR: &str = "046d";
    // Add more product ids here as Phase 3 lands more devices.
    const SUPPORTED_PRODUCTS: &[&str] = &["b034", "c548"];

    let body = match std::fs::read_to_string("/proc/bus/input/devices") {
        Ok(s) => s,
        Err(e) => {
            let esc = format!("{e}").replace('\\', "\\\\").replace('"', "\\\"");
            return format!(r#"{{"ok":false,"err":"{esc}"}}"#);
        }
    };

    // Parser: block delimiter is a blank line. Within a block, lines
    // start with `I:` / `N:` / `H:` / etc. We need I (vendor/product)
    // and N (name). No heap allocation per line — split + match.
    let mut current_vendor   = String::new();
    let mut current_product  = String::new();
    let mut current_name     = String::new();
    let mut found_name: Option<String> = None;
    let mut found_vendor_product: Option<String> = None;

    fn flush(
        v: &str, p: &str, n: &str,
        found_name: &mut Option<String>,
        found_vp: &mut Option<String>,
    ) {
        if v.eq_ignore_ascii_case(LOGITECH_VENDOR)
            && SUPPORTED_PRODUCTS.iter().any(|sp| sp.eq_ignore_ascii_case(p))
            && !n.is_empty()
        {
            *found_name = Some(n.to_string());
            *found_vp = Some(format!("{}:{}", v, p));
        }
    }

    for raw in body.lines() {
        if raw.is_empty() {
            flush(&current_vendor, &current_product, &current_name,
                  &mut found_name, &mut found_vendor_product);
            if found_name.is_some() {
                break;
            }
            current_vendor.clear();
            current_product.clear();
            current_name.clear();
            continue;
        }
        if let Some(rest) = raw.strip_prefix("I: ") {
            for tok in rest.split_whitespace() {
                if let Some(v) = tok.strip_prefix("Vendor=") {
                    current_vendor = v.to_string();
                } else if let Some(p) = tok.strip_prefix("Product=") {
                    current_product = p.to_string();
                }
            }
        } else if let Some(rest) = raw.strip_prefix("N: Name=") {
            current_name = rest.trim_matches('"').to_string();
        }
    }
    // Tail block (file doesn't end with a blank line in some kernels)
    if found_name.is_none() {
        flush(&current_vendor, &current_product, &current_name,
              &mut found_name, &mut found_vendor_product);
    }

    match (found_name, found_vendor_product) {
        (Some(name), Some(vp)) => {
            let esc = |s: &str| s.replace('\\', "\\\\").replace('"', "\\\"");
            format!(
                r#"{{"ok":true,"found":true,"name":"{}","id":"{}"}}"#,
                esc(&name),
                esc(&vp),
            )
        }
        _ => r#"{"ok":true,"found":false}"#.to_string(),
    }
}

/// Wraps the initial spawn outcome so it lives for the duration of the Tauri
/// app and can be queried by the `daemon_status` command. Updated whenever
/// `daemon_respawn` runs.
struct DaemonStartup(Mutex<daemon::SpawnOutcome>);

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    apply_webkit_wayland_workarounds();

    // Probe systemd state FIRST. KWin script + heal only run when the
    // daemon is actually meant to be active (enabled or already active).
    // Doing them unconditionally was the root cause of "features keep
    // running while toggle is OFF" — KWin script enablement triggers
    // active-window events that the running daemon processes.
    let svc = service::query_state();

    let kwin_and_heal_should_run = svc.available && (svc.active || svc.enabled);
    if kwin_and_heal_should_run {
        ensure_kwin_script_enabled();
        service::heal_at_startup();
    }

    let initial = if !svc.available {
        // Unit not installed. UI will show first-run wizard.
        // Do NOT spawn-detach — that was the legacy path producing
        // the two-daemon EVIOCGRAB race when the user later toggles ON.
        daemon::SpawnOutcome::BinaryNotFound
    } else if svc.active {
        // Already running under systemd. Just wait for the socket.
        daemon::wait_for_running(Duration::from_secs(5))
    } else if svc.enabled {
        // Enabled but not active (rare — maybe crashed). Ask systemd
        // to start it; do NOT spawn-detach.
        let _ = service::start_only();
        daemon::wait_for_running(Duration::from_secs(5))
    } else {
        // Disabled. User explicitly turned it OFF. Don't start
        // anything. Toggle will read OFF and the UI works in
        // "configure but daemon not running" mode.
        daemon::SpawnOutcome::Timeout
    };

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
            // daemon_respawn and kill_daemon are kept for backward
            // compatibility but no longer called from the production UI.
            // They will be removed in a future major version.
            daemon_respawn,
            kill_daemon,
            service_state,
            service_enable,
            service_disable,
            service_start,
            service_stop,
            service_install_hint,
            set_always_on_top,
            read_daemon_log,
            system_info,
            copy_to_clipboard,
            detect_logitech_device,
        ])
        .run(tauri::generate_context!());

    if let Err(e) = result {
        eprintln!("[loginext-ui] tauri runtime failed: {e}");
        std::process::exit(1);
    }
}
