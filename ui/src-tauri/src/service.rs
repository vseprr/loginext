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

use std::path::PathBuf;
use std::process::{Command, Stdio};

const UNIT_NAME: &str = "loginext.service";

/// Marker emitted into the rendered unit body. Heal compares the value
/// found in the on-disk unit (parsed out of a `# loginext-template-version:`
/// comment line) against this constant; a mismatch triggers a rewrite even
/// when ExecStart= already points at the right binary. Bump on every
/// behavioural change to the template body — the heal is then the
/// distribution channel for the fix without forcing the user to re-run
/// install.sh or hand-edit the unit.
///
/// History:
///   v1 — initial canonical template (deploy/systemd/loginext.service v1).
///   v2 — `-` prefix on ReadWritePaths entries to fix `226/NAMESPACE` on
///        boot caused by `%t/loginext.sock` not existing at service start.
///   v3 — `WantedBy=graphical-session.target` instead of `default.target`
///        so the service restarts when the graphical session restarts
///        (PartOf=graphical-session.target stops the unit on session
///        restart, but WantedBy=default.target wouldn't restart it).
///        This version REGRESSED cold-boot autostart on Plasma 6 / SDDM
///        because `graphical-session.target` has `RefuseManualStart=yes`
///        and is only activated when a desktop-session service pulls
///        it in; several Plasma 6 boot paths do not, so the `.wants/`
///        symlink stays dormant and the daemon never starts.
///   v4 — revert `WantedBy` to `default.target` (canonical user-manager
///        autostart target, always reached on bring-up). `PartOf=
///        graphical-session.target` is retained so session stop/restart
///        still propagates; `Restart=on-failure` catches the SIGTERM.
///   v5 — move `StartLimitIntervalSec` / `StartLimitBurst` from [Service]
///        to [Unit]. systemd silently ignores them under [Service]
///        (logs `Unknown key 'StartLimitIntervalSec' in section [Service]`)
///        and the unit then runs without the burst guard, which caused
///        a real production regression: a grab-EBUSY failure (two
///        daemons racing for the device grab) looped ~90 restart
///        attempts instead of giving up after 5. The grab-race root
///        cause is fixed in lib.rs::run() (systemd-aware startup) but
///        the wrong-section guard is kept correct here on its own merits.
///   v6 — runtime SIGKILL hardening + boot-race grace window. Adds
///        `MemoryHigh/MemoryMax/TasksMax/CPUQuota` cgroup ceilings so
///        a runaway pacer/IPC loop doesn't drive systemd-oomd to
///        cgroup-pressure-kill the daemon (observed on CachyOS /
///        Plasma 6: 94% CPU steady-state → SIGKILL with peak RSS 2.4 MB,
///        i.e. NOT classic OOM but oomd PSI). Adds
///        `ManagedOOMPreference=avoid` to deprioritise this cgroup for
///        oomd. Adds `RestartForceExitStatus=143` so the SIGTERM that
///        `PartOf=` propagation delivers is treated as a restart-eligible
///        failure (without it, on-failure ignores 143 = SIGTERM+128 and
///        the unit stops dead on session restart). Bumps `RestartSec`
///        3 → 15 and `StartLimitIntervalSec` 60 → 120 so the daemon's
///        new bounded-retry udev grace window (10 × 2 s in main.cpp)
///        doesn't burn through the start-limit budget on cold boot
///        when /dev/input/event* enumerates after default.target.
///   v7 — fixes a v6 directive inversion that made the SIGKILL pattern
///        WORSE rather than better. v6 set `ManagedOOMMemoryPressure=kill`
///        and `ManagedOOMSwap=kill`, which per systemd.resource-control(5)
///        opts the cgroup IN to oomd's pressure-kill list (default `auto`
///        means "exclude from oomd monitoring"). Combined with
///        `ManagedOOMPreference=avoid` (only "deprioritise"), the unit was
///        explicitly enrolled for kill consideration. v7 removes both
///        Pressure / Swap directives (defaults to `auto` = excluded) and
///        upgrades `Preference=avoid` → `Preference=omit`, the
///        strongest opt-out available. Combined with the libevdev-error
///        clean-exit fix in core/event_loop.cpp (without which the loop
///        burns 100% CPU on USB unplug regardless of cgroup limits),
///        this should end the SIGKILL cycle.
///   v8 — fixes the IPC socket never appearing on disk under the systemd
///        unit. Earlier templates relied on `ReadWritePaths=-%t/loginext.sock`
///        to punch a writable hole through `ProtectHome=read-only`'s
///        remount of /run/user/<uid>. The leading `-` makes systemd
///        TOLERATE a missing path, but it does NOT make the writable
///        bind mount lazy: when the socket file does not yet exist
///        (always true at fresh startup), systemd skips the bind mount
///        entirely and /run/user/<uid> stays read-only — bind() then
///        fails with EROFS. The previous `--quiet` flag muted the
///        resulting `LX_ERROR("ipc: bind(...) failed: Read-only file
///        system")`, so the bug was invisible in the journal.
///        v8 adds `RuntimeDirectory=loginext`, which creates
///        /run/user/<uid>/loginext as a writable per-unit directory
///        compatible with `ProtectHome=read-only`. The socket moves
///        from `%t/loginext.sock` to `%t/loginext/loginext.sock` and
///        the `ReadWritePaths` workaround is dropped (no longer
///        needed). UI's `daemon::socket_path()` and the daemon's
///        `resolve_socket_path()` are updated in lockstep — both
///        check for the directory and create it on the spawn-detached
///        path so the non-systemd code path keeps working.
const TEMPLATE_VERSION: u32 = 8;
const TEMPLATE_VERSION_MARKER: &str = "# loginext-template-version:";

/// User-local unit file path. We deliberately write here (not
/// `/usr/lib/systemd/user/`) when self-healing — the same write the
/// install.sh path performs. Pacman's package install lives in
/// /usr/lib/, so a self-heal in ~/.config/ shadows the package copy
/// without conflicting on disk.
fn user_unit_path() -> Option<PathBuf> {
    let home = std::env::var("HOME").ok()?;
    Some(PathBuf::from(format!(
        "{home}/.config/systemd/user/{UNIT_NAME}"
    )))
}

/// Returns the absolute path of the daemon binary the UI process
/// will actually be able to exec, or None if no candidate is found.
/// Mirrors `daemon::resolve_daemon_binary()` so the unit file ends
/// up pointing at exactly the binary `service_enable` would otherwise
/// try to launch — keeps the systemd path identical to the manual
/// `kill_daemon → daemon_respawn` path.
fn resolved_daemon_path() -> Option<PathBuf> {
    crate::daemon::resolve_daemon_binary()
}

/// Read the `ExecStart=` line from the user's unit file and return
/// the binary path it references. None on a missing file or
/// malformed unit. Used to decide whether the unit needs healing
/// before we ask systemd to start it.
fn current_exec_start() -> Option<PathBuf> {
    let path = user_unit_path()?;
    let body = std::fs::read_to_string(&path).ok()?;
    for line in body.lines() {
        let trimmed = line.trim_start();
        if let Some(rest) = trimmed.strip_prefix("ExecStart=") {
            // Strip flags like `--quiet` so we compare just the binary.
            // The unit format is whitespace-separated, no shell parsing.
            let bin = rest.split_whitespace().next()?.to_string();
            return Some(PathBuf::from(bin));
        }
    }
    None
}

/// Parse the `# loginext-template-version: N` comment out of the user's
/// unit file. Returns 0 when the marker is absent (older heal write or
/// stock canonical install) — anything < TEMPLATE_VERSION triggers a
/// rewrite. Tolerates whitespace and trailing comments so a future
/// minor format tweak doesn't accidentally bump every user's unit.
fn current_template_version() -> u32 {
    let Some(path) = user_unit_path() else { return 0 };
    let Ok(body)  = std::fs::read_to_string(&path) else { return 0 };
    for line in body.lines() {
        let trimmed = line.trim_start();
        if let Some(rest) = trimmed.strip_prefix(TEMPLATE_VERSION_MARKER) {
            if let Ok(n) = rest.trim().parse::<u32>() {
                return n;
            }
        }
    }
    0
}

/// Rewrite the user's unit file with an ExecStart that points at the
/// resolved daemon binary, then run `systemctl --user daemon-reload`.
/// Idempotent — does nothing when the unit is already correct.
///
/// This is the load-bearing fix for the `status=203/EXEC` failure mode
/// users hit when they install via `install.sh` (which puts the daemon
/// in `~/.local/bin`) but receive a unit file whose ExecStart still
/// points at `/usr/bin/loginext` (the package-install path). Rather
/// than asking the user to re-run install.sh or hand-edit the unit,
/// the UI heals it automatically the first time the toggle flips ON.
///
/// We only ever WRITE the user-local copy at `~/.config/systemd/user/`.
/// A package-installed unit at `/usr/lib/systemd/user/` is never
/// modified — we'd need root to do that, and the user-local copy
/// shadows it cleanly under systemd's unit-file lookup order.
pub fn heal_unit_if_stale() -> Result<bool, String> {
    let resolved = resolved_daemon_path()
        .ok_or_else(|| "loginext daemon binary not found in any of the standard \
                        locations (LOGINEXT_DAEMON env var, ../build/, $PATH, \
                        ~/.local/bin, /usr/local/bin, /usr/bin)".to_string())?;
    let resolved_str = resolved.to_string_lossy().into_owned();

    // Two independent heal triggers:
    //   1. ExecStart= points at a stale binary (the classic 203/EXEC).
    //   2. Template version on disk is older than TEMPLATE_VERSION
    //      (a previous heal wrote a body with a known-broken hardening
    //      directive, e.g. v1's ReadWritePaths=%t/loginext.sock that
    //      blew up with 226/NAMESPACE on every boot).
    // Either trigger forces a full rewrite from the canonical template.
    let exec_ok = matches!(current_exec_start(), Some(p) if p == resolved);
    let version_ok = current_template_version() >= TEMPLATE_VERSION;
    if exec_ok && version_ok {
        return Ok(false);
    }

    // Build the unit body. We could read the user's existing file and
    // patch just the ExecStart, but that risks merging with a
    // half-customised copy. Writing fresh from the canonical template
    // and substituting ExecStart = the resolved path is safer.
    //
    // The body intentionally mirrors deploy/systemd/loginext.service so
    // a self-heal lands on a unit equivalent to a clean install.sh run.
    // Bump TEMPLATE_VERSION above when changing this body.
    let body = format!(
        "# Auto-generated by loginext-ui — see ui/src-tauri/src/service.rs.\n\
         # Hand edits will be overwritten the next time the UI detects a\n\
         # stale ExecStart= path or template-version drift.\n\
         {TEMPLATE_VERSION_MARKER} {TEMPLATE_VERSION}\n\
         [Unit]\n\
         Description=LogiNext — Logitech device control daemon\n\
         Documentation=https://github.com/vseprr/loginext\n\
         After=graphical-session.target\n\
         PartOf=graphical-session.target\n\
         # Start-limit guards MUST live in [Unit], not [Service]. systemd\n\
         # silently ignores them under [Service] (warns `Unknown key\n\
         # 'StartLimitIntervalSec' in section [Service]`) — without the\n\
         # burst guard a grab-EBUSY failure loops indefinitely instead of\n\
         # stopping after 5 attempts.\n\
         #\n\
         # 120 s window (was 60 s in v5): the daemon's bounded udev-retry\n\
         # in main.cpp can take up to 20 s per restart cycle while waiting\n\
         # for /dev/input/event* to enumerate at cold boot, so 5 attempts\n\
         # × (15 s RestartSec + 20 s retry budget) ≈ 175 s. Sizing the\n\
         # window at 120 s keeps the 5-burst guard meaningful while\n\
         # ensuring a single slow boot doesn't latch the unit dead.\n\
         StartLimitIntervalSec=120\n\
         StartLimitBurst=5\n\
         \n\
         [Service]\n\
         Type=simple\n\
         ExecStart={resolved_str} --quiet\n\
         ExecReload=/bin/kill -HUP $MAINPID\n\
         Restart=on-failure\n\
         RestartSec=15\n\
         # 143 = 128 + SIGTERM. Without this, `Restart=on-failure`\n\
         # treats the SIGTERM that `PartOf=graphical-session.target`\n\
         # propagation delivers as a clean stop and the unit doesn't\n\
         # come back when the session restarts.\n\
         RestartForceExitStatus=143\n\
         # Informational only — documents that the unit's lifecycle is\n\
         # tied to the graphical session. systemd does not interpret\n\
         # X-prefixed keys; the actual coupling lives in PartOf= above.\n\
         X-Restart-Triggers=graphical-session.target\n\
         # StartLimit* moved to [Unit] above — placing them here makes\n\
         # systemd ignore them with no functional guard in place.\n\
         \n\
         # Resource ceilings — without these the cgroup is unbounded and\n\
         # systemd-oomd (default-on under Plasma 6 / CachyOS) will\n\
         # SIGKILL the daemon on sustained CPU pressure (observed: 94%\n\
         # CPU for 9 min → status=9/KILL, peak RSS only 2.4 MB so this\n\
         # is PSI-driven, not classic OOM). The ceilings are 50× the\n\
         # observed RSS — generous, but cap absolute damage if the\n\
         # pacer state machine ever wedges. CPUQuota=50% likewise caps\n\
         # any runaway loop at half a core.\n\
         MemoryHigh=32M\n\
         MemoryMax=64M\n\
         TasksMax=32\n\
         CPUQuota=50%\n\
         # systemd-oomd opt-out. `omit` is the strongest exclusion: this\n\
         # cgroup is never considered for pressure-kill, regardless of\n\
         # PSI score. We don't fall through to `avoid` because the\n\
         # underlying CPU runaway (a libevdev_next_event spin on a dead\n\
         # device fd) is now caught by core/event_loop.cpp's fatal-error\n\
         # path — the daemon exits cleanly on -ENODEV and systemd's\n\
         # Restart=on-failure brings it back, so we no longer need oomd\n\
         # as the kill-of-last-resort. Requires systemd ≥ 248; Plasma 6\n\
         # baselines satisfy this.\n\
         #\n\
         # NOTE: do NOT set `ManagedOOMMemoryPressure=` or\n\
         # `ManagedOOMSwap=` here. Their default value `auto` means\n\
         # \"exclude from oomd monitoring\"; setting them to `kill`\n\
         # explicitly OPTS THE CGROUP IN to oomd's kill list — the\n\
         # opposite of what the directive name suggests. v6 had this\n\
         # backwards, which made the SIGKILL pattern worse.\n\
         ManagedOOMPreference=omit\n\
         \n\
         NoNewPrivileges=true\n\
         ProtectSystem=strict\n\
         ProtectHome=read-only\n\
         # systemd creates /run/user/<uid>/loginext on unit start, mode 0700,\n\
         # and removes it on stop. This is the canonical way to give a\n\
         # service a writable runtime location compatible with\n\
         # ProtectHome=read-only (which would otherwise lock\n\
         # /run/user/<uid> read-only and prevent the daemon from binding\n\
         # its IPC socket). The socket lives at\n\
         # %t/loginext/loginext.sock — kept in lockstep with the daemon's\n\
         # ipc/server.cpp::resolve_socket_path() and the UI's\n\
         # daemon::socket_path() so all three agree on the path.\n\
         RuntimeDirectory=loginext\n\
         RuntimeDirectoryMode=0700\n\
         # %S/loginext is $XDG_STATE_HOME/loginext (the daemon's log dir).\n\
         # Leading `-` marks the path optional in case the directory does\n\
         # not yet exist on a fresh install.\n\
         ReadWritePaths=-%S/loginext\n\
         PrivateTmp=true\n\
         ProtectKernelTunables=true\n\
         ProtectKernelModules=true\n\
         ProtectControlGroups=true\n\
         RestrictNamespaces=true\n\
         LockPersonality=true\n\
         RestrictRealtime=true\n\
         SystemCallArchitectures=native\n\
         \n\
         # `default.target` is the canonical user-manager autostart target\n\
         # — always reached on `systemctl --user` bring-up, independent of\n\
         # how the display manager wires up the graphical session.\n\
         #\n\
         # We deliberately do NOT use `WantedBy=graphical-session.target`:\n\
         # that target is reverse-dependency-only (`RefuseManualStart=yes`)\n\
         # and on several Plasma 6 boot paths (SDDM → Wayland on Arch /\n\
         # CachyOS) it is never activated at user-manager bring-up, so\n\
         # units `WantedBy=` it stay stopped at boot even though\n\
         # `is-enabled` returns `enabled`. Session-lifecycle coupling is\n\
         # preserved by `PartOf=graphical-session.target` above.\n\
         [Install]\n\
         WantedBy=default.target\n",
    );

    let path = user_unit_path()
        .ok_or_else(|| "$HOME unset — cannot write user-local unit file".to_string())?;
    let dir  = path.parent()
        .ok_or_else(|| "user unit path has no parent directory".to_string())?;
    std::fs::create_dir_all(dir).map_err(|e| format!("mkdir {}: {e}", dir.display()))?;

    // Write atomically — temp + rename — so a crash mid-write can never
    // leave a half-written unit file that systemd would refuse to load.
    let tmp = dir.join(format!("{UNIT_NAME}.tmp"));
    std::fs::write(&tmp, body).map_err(|e| format!("write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, &path).map_err(|e| {
        format!("rename {} → {}: {e}", tmp.display(), path.display())
    })?;

    eprintln!(
        "[loginext-ui] systemd: rewrote {} to ExecStart={resolved_str} (was stale)",
        path.display()
    );
    Ok(true)
}

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
/// so the daemon autostarts at next login.
///
/// Self-heals the unit file before enabling: if the user's
/// `~/.config/systemd/user/loginext.service` has an `ExecStart=` that
/// points at a binary that doesn't exist (the classic `status=203/EXEC`
/// failure — typically `/usr/bin/loginext` on a host that installed
/// the daemon to `~/.local/bin/loginext` via install.sh), we rewrite
/// the unit to the actual binary path before asking systemd to start
/// it. Pre-existing user customisations that diverge from the canonical
/// template ARE overwritten — the rationale is that this code path
/// only runs when `enable_now()` is explicitly invoked from the UI
/// toggle, which is itself an explicit user gesture, and the cost of
/// silently launching with a broken ExecStart (boot loops + journald
/// noise + auto-restart back-off) is far higher than the cost of
/// flattening a hand-edit. Manual edits should go in a drop-in at
/// `~/.config/systemd/user/loginext.service.d/override.conf` instead,
/// which the heal does NOT touch.
pub fn enable_now() -> Result<(), String> {
    let healed = heal_unit_if_stale()?;
    let _ = run_systemctl(&["daemon-reload"]);
    let result = run_systemctl(&["enable", "--now", UNIT_NAME]);
    if healed && result.is_err() {
        // If the heal landed AND the enable still failed, it's likely a
        // load-time error in the unit body (rare: systemd's parser is
        // permissive). Fall through with the original error so the user
        // sees what systemd actually rejected; the heal log line above
        // already explains what we wrote.
    }
    result
}

/// `systemctl --user disable --now loginext.service`
///
/// Stops the daemon and removes the autostart symlink. The IPC `quit`
/// path the toggle previously used is now redundant — systemd handles
/// SIGTERM delivery itself, including the cooperative-stop semantics.
pub fn disable_now() -> Result<(), String> {
    run_systemctl(&["disable", "--now", UNIT_NAME])
}

/// Proactive heal — runs at UI startup, BEFORE the user has had a chance
/// to click the toggle. Catches the case the previous heal flow missed:
/// the user's unit was enabled in a prior session, autostarts at boot,
/// fails the namespace step (226/NAMESPACE on the v1 ReadWritePaths),
/// and lands the service in `activating (auto-restart)`. The toggle
/// reads "ON" because is-active=activating, the user never clicks it,
/// and `enable_now()`'s heal never runs. The boot-time failure persists
/// across every reboot.
///
/// This function fixes that loop:
///   1. Skip work entirely if the unit isn't installed (available=false).
///   2. Run heal_unit_if_stale() — rewrites the unit body when the
///      template version on disk is older than TEMPLATE_VERSION, even
///      if ExecStart= already matches.
///   3. If a heal happened: daemon-reload + try-restart so the running
///      (failing) service picks up the new body without waiting for the
///      user to flip the toggle. try-restart is a no-op when the unit
///      is inactive, so a healthy host pays nothing for this path.
///
/// Errors are logged to stderr but never propagate — UI startup must
/// never block on a systemd quirk.
pub fn heal_at_startup() {
    // Cheap no-op when the unit isn't installed at all (e.g. a manual
    // `cmake` build with no install.sh run). Don't write a unit file
    // out of nowhere — the user hasn't asked for systemd integration.
    let s = query_state();
    if !s.available {
        return;
    }

    match heal_unit_if_stale() {
        Ok(false) => {} // unit was already current; nothing to do
        Ok(true)  => {
            eprintln!("[loginext-ui] systemd: heal-at-startup rewrote unit (template upgrade)");
            // Pick up the new body in the in-memory unit cache.
            let _ = run_systemctl(&["daemon-reload"]);
            // try-restart only acts when the unit is active/activating —
            // exactly the state a 226/NAMESPACE auto-restart loop sits in.
            // For an idle-and-disabled unit this is a no-op.
            let _ = run_systemctl(&["try-restart", UNIT_NAME]);

            // Block until systemd reports the unit active again (or give
            // up after a short budget). Without this, the caller proceeds
            // straight to daemon::ensure_running() and probes the socket
            // during the deactivating→activating window. ensure_running()
            // then sees a dead socket, spawn-detaches a competing daemon,
            // and the two processes race for `EVIOCGRAB` on the mouse —
            // whichever grabs first wins, the loser exits with EBUSY,
            // systemd restarts it, fails again, and we get the infinite
            // 203/EBUSY loop the user just reported. Capping the wait at
            // 3 s keeps UI startup responsive when systemd legitimately
            // failed to restart the service (stale binary etc.).
            let deadline = std::time::Instant::now()
                + std::time::Duration::from_millis(3000);
            while std::time::Instant::now() < deadline {
                let s = query_state();
                if s.active {
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(100));
            }
        }
        Err(e) => {
            // Most common cause: daemon binary not on disk yet (fresh
            // checkout, no `cmake --build` run). Don't surface as fatal
            // — the spawn-on-launch path will report it separately.
            eprintln!("[loginext-ui] systemd: heal-at-startup skipped: {e}");
        }
    }
}
