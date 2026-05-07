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

    // Self-heal the systemd user unit at every UI launch. This catches
    // the boot-time auto-restart loop the original heal-on-toggle path
    // could not: when the unit is enabled+failing on every reboot, the
    // toggle reads "ON" (is-active=activating), the user never clicks
    // it, and the heal never runs. Now the heal runs unconditionally at
    // startup so a buggy hardening directive (e.g. v1's missing `-`
    // prefix on ReadWritePaths that 226/NAMESPACE'd at boot) is fixed
    // the next time the user opens the UI, with no manual systemctl
    // commands required. Cheap when there's nothing to fix.
    service::heal_at_startup();

    // Decide the lifecycle path. When systemd is managing the daemon
    // (unit is installed and either active or enabled), we MUST NOT
    // spawn-detach a competing instance: both processes would race for
    // `EVIOCGRAB` on the mouse and the loser would spin in a restart
    // loop until the `[Unit] StartLimitBurst` cap kicks in (previous
    // regression: ~90 restart attempts before the guard engaged,
    // because the Start-limit directives were mis-placed under
    // [Service] and silently ignored — fixed separately in
    // TEMPLATE_VERSION=5). When systemd isn't in play (unit absent,
    // e.g. fresh cmake-only build), fall back to the legacy
    // spawn-detached path so the toggle remains functional.
    let svc = service::query_state();
    let initial = if svc.available && (svc.active || svc.enabled) {
        // Up to 5 s is enough for the daemon to bind the socket after
        // a fresh `systemctl --user start` or the heal-triggered
        // try-restart that heal_at_startup() may have just issued.
        daemon::wait_for_running(Duration::from_secs(5))
    } else {
        daemon::ensure_running()
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
            daemon_respawn,
            kill_daemon,
            service_state,
            service_enable,
            service_disable,
            set_always_on_top,
        ])
        .run(tauri::generate_context!());

    // Don't panic on Tauri runtime errors — log + exit non-zero so a launcher
    // surfaces the failure cleanly rather than leaving a SIGABRT corefile.
    if let Err(e) = result {
        eprintln!("[loginext-ui] tauri runtime failed: {e}");
        std::process::exit(1);
    }
}
