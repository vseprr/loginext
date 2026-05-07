# LogiNext — Architecture decisions, known issues, deferred audit findings

Long-form context for the structural choices that shaped the codebase, plus the audit findings that are either already shipped (see [CHANGELOG.md](./CHANGELOG.md)) or deliberately deferred. Active rules sit in [OPTIMIZATIONS.md](./OPTIMIZATIONS.md). Roadmap lives in [progress.md](./progress.md).

This file is the **architecture decision record**. The rationale here is what stops a future session — human or AI — from re-litigating questions that already have well-tested answers.

---

## Privilege model: daemon runs as the seat user (2026-05-03)

The daemon must always run as the same user as the KDE session. `sudo loginext` is **not** a supported runtime.

**Why:** Plasma 6's session-bus broker rejects D-Bus EXTERNAL auth from uid 0 with EPIPE during the auth handshake, so the `org.loginext.WindowFocus` name claim never lands. Per-app rules silently degrade to the global preset because the listener binds the bus successfully but receives no `Activated()` calls from the KWin script.

Two prior attempts to bridge the gap failed at the same wall — the broker's policy is the constraint, not the address:
1. `sudo`-aware `sd_bus_open_user` skipping with explicit `unix:path=/run/user/$SUDO_UID/bus` resolution (Phase 2.7.1)
2. SUDO_UID heuristics + `/run/user/*` directory scan + retry on `sd_bus_open_user` failure (Phase 2.7.2)

Both got blocked by the same EPIPE rejection.

**The supported model**: hardware access (`/dev/input/event*`, `/dev/uinput`) is granted unprivileged via [deploy/udev/99-loginext.rules](./deploy/udev/99-loginext.rules). The rules use `TAG+="uaccess"` (logind ACL grant to the active seat user) and `GROUP="input"` as a non-logind fallback. Both pacman post-install and `install.sh` install the rules and run `udevadm control --reload-rules && udevadm trigger`.

If a future user reports "sudo doesn't work":
- The answer is "install the udev rules and stop using sudo", **not** "let me try harder to bridge to the user's bus".
- The IPC `quit` command (added in 2.7.1) survives in the user-mode flow because it's uid-agnostic by design.

**When extending to new Logitech devices**: add the USB `ATTRS{idVendor}/idProduct` to `deploy/udev/99-loginext.rules` *first*. If the daemon still needs sudo for new hardware after that, that's a regression to fix in the rules, not paper over with sudo docs.

---

## KWin focus bridge: enablement is the UI's responsibility (2026-05-06)

The "LogiNext Focus Bridge" KWin script (`deploy/kwin/loginext-focus/`) is the *producer* of per-app rule input — without it firing, the daemon's `active_app_hash` stays at 0 and every event resolves against the global preset.

**Why pacman post-hooks can't enable it:** Pacman post-install hooks run as root and cannot write to a user's `~/.config/kwinrc`. The package install lands the script files in `/usr/share/kwin/scripts/` but cannot flip `loginext-focusEnabled=true` in any user's per-user kwinrc.

**The Tauri shell carries the responsibility instead.** `ensure_kwin_script_enabled()` in `ui/src-tauri/src/lib.rs` runs on every UI launch when `XDG_CURRENT_DESKTOP` contains `KDE`:
1. Locate the script source (system → user → repo `deploy/kwin/loginext-focus/` walk-up)
2. `kpackagetool6 --upgrade` → fall back to `--install` → fall back to direct copy into `~/.local/share/kwin/scripts/loginext-focus/` if Plasma's tooling is missing
3. `kwriteconfig6 → kwriteconfig5` flips `loginext-focusEnabled = true`
4. `qdbus6 → qdbus-qt6 → qdbus org.kde.KWin /KWin reconfigure` reloads the script

Idempotent. Falls through silently on missing tools / dead KWin.

**The daemon's 30 s diagnostic is the early-detection net.** `kwin_dbus_loop` clamps its select() timeout so a one-shot WARN fires at +30 s if `kwin_received_any` is still false. Message names the exact `kwriteconfig6 + qdbus6` recovery commands.

If a future user reports "per-app rules don't work":
1. **Don't** start by telling them to run `kwriteconfig6`. The UI does that. If they're still seeing the symptom, the auto-enable path is broken.
2. Check `~/.local/state/loginext/daemon.log` for the `30s elapsed since kwin-dbus bind` warning. Absent → daemon never bound the bus.
3. Check `kreadconfig6 --file kwinrc --group Plugins --key loginext-focusEnabled`. Returns `false` after a UI launch → `ensure_kwin_script_enabled()` ran but kwriteconfig didn't take.
4. `dbus-monitor "interface='org.loginext.WindowFocus'"` — if no `Activated()` lines appear when focus changes, the script is registered but not loaded by KWin (toggle it in System Settings → KWin Scripts).

**Do not re-introduce manual kwriteconfig6 instructions in the README.** Phase 2.7.4 made the bridge enable itself; that's the contract.

---

## KWin script heartbeat: 2 s republish on script load + interval (2026-05-07)

The script publishes the active window on `windowActivated` (Plasma 6) / `clientActivated` (Plasma 5), once on script load, AND every 2 seconds via a periodic timer.

**Why a heartbeat at all:** When the daemon restarts (UI toggle, systemctl restart, crash + respawn), the KWin script's `windowActivated` connection stays alive (the script lives in KWin's process, not the daemon's), but no event fires until the user changes focus. Without the heartbeat, the daemon has no idea what's focused until the user clicks somewhere.

**Why 2 seconds:** the daemon's `publish_and_log` returns early when the FNV-1a hash hasn't changed, so the heartbeat costs zero log lines while focus is unchanged. Its only effect is recovering after a daemon (re)bind. Faster intervals (500 ms) didn't measurably improve UX; slower (5 s+) felt sluggish on rapid daemon restarts during testing.

**Three-tier fallback inside `startHeartbeat()`:**
1. `setInterval(publishCurrent, 2000)` — Plasma 6's QJSEngine. Cleanest path.
2. `new QTimer()` + `interval=2000` + `timeout.connect()` — older QtScript engines.
3. Wider event coverage (`workspace.windowAdded`, `workspace.clientAdded`, `workspace.currentDesktopChanged`) when no timer API is available at all.

**Do not** trade the heartbeat for a "subscribe to NameOwnerChanged in the KWin script" approach. KWin scripts can `callDBus` outgoing only — they cannot register signal handlers or expose D-Bus methods. We tried; the QJSEngine's QtDBus surface is one-way.

`metadata.json` Version is bumped on every script-content change so KPackage's `--upgrade` reliably refreshes installed copies. Currently at Version 2 (heartbeat addition). Bump it on every behavioural change.

---

## Per-app rule data model: `app=preset[,mode[,invert]]` (2026-05-06 / 2026-05-07)

Each app rule carries its own preset, sensitivity mode, and invert flag. All three fields are independent — empty preset = "tracked-only chip" (UI-visible context entry, daemon ignores), empty mode/invert = "inherit from global at lookup time".

**Why each field is independent and not "all-or-nothing":** users want to override one knob without committing to all of them. "Use the global sensitivity but force invert=on for this app" is a real workflow. Inheritance is per-field, computed at every event resolution — so changing the global propagates to inheriting rules without a re-save.

**Why we kept "tracked-only chips" as a first-class concept:** deselecting the active preset on a per-app chip should not delete the chip. The chip carries metadata (mode/invert) that survives the unbind/bind cycle. Explicit deletion needs a different gesture — the × close button on chip hover.

**Hot path implementation** (`src/main.cpp::on_event`):
```cpp
PresetId effective_preset = settings.active_preset;
const Profile* effective_profile = &settings.profile;
bool effective_invert = settings.invert_hwheel;

scope::AppRule rule;
if (scope::lookup(rules, app_hash, rule)) {
    effective_preset = rule.preset;
    if (rule.mode != SensitivityMode::Inherit) {
        effective_profile = &profile_for(rule.mode);
    }
    if (rule.invert != scope::invert_inherit) {
        effective_invert = (rule.invert == scope::invert_on);
    }
}
```

`SensitivityMode::Inherit = 255` is the sentinel; `int8_t invert` uses -1 (inherit) / 0 (off) / 1 (on). Both stored at file load time in `scope::AppRule`.

**Right panel UI dispatches by `currentContext`.** Sensitivity / invert writes go through `setRuleMode` / `setRuleInvert` when an app chip is active, and through `applyCurrentSettings` (settings.json + reload) only on the global context. There's a defensive `currentContext.type !== "global"` early-return inside `applyCurrentSettings()` with a `console.warn` — if a future refactor drops the call-site gate, the function fails loudly rather than silently corrupting the global config.

**Backward compatibility:** the rules loader supports the single-field `app=preset` form. New rules are written in the extended form.

---

## systemd unit hardening: `ReadWritePaths` paths must be marked optional (2026-05-07)

The unit's `ReadWritePaths=` lists must use the leading-`-` (optional path) prefix for any path that does not exist when systemd sets up the mount namespace. The classic regression: `ReadWritePaths=%S/loginext %t/loginext.sock` with `ProtectHome=read-only` enabled.

**Why it fails:** `ProtectHome=read-only` locks `/run/user/<uid>` read-only inside the unit's mount namespace. `ReadWritePaths=%t/loginext.sock` then tries to remount `/run/user/<uid>/loginext.sock` writable — but the daemon hasn't created the socket yet (it does that during init, AFTER exec). systemd's namespace builder hits "No such file or directory" and aborts with `226/NAMESPACE` *before* exec runs. The unit auto-restart-loops forever; the daemon is never spawned.

**The fix:** prefix each `ReadWritePaths=` entry with `-` so systemd registers the writable mount lazily and tolerates the path being absent:

```
ReadWritePaths=-%S/loginext -%t/loginext.sock
```

Apply to both:
- [deploy/systemd/loginext.service](./deploy/systemd/loginext.service) — the canonical template install.sh ships
- The heal template inside `ui/src-tauri/src/service.rs::heal_unit_if_stale()` — what the UI rewrites stale units to

**Why we didn't switch to `ReadWritePaths=%t` instead:** granting RW to the entire `XDG_RUNTIME_DIR` works but exposes other apps' sockets (e.g. KDE wallet, PipeWire) to the daemon's mount namespace. The optional-path form keeps the surface to exactly the one socket the daemon writes.

**This pattern is mandatory for any future hardening directive added to the unit.** Run a smoke `systemctl --user start loginext.service && systemctl --user status loginext.service` after any change — `226/NAMESPACE` is silent in the on-disk log path but loud in `journalctl --user -u loginext.service`.

---

## systemd unit: `StartLimitIntervalSec` / `StartLimitBurst` MUST live under `[Unit]` (2026-05-07)

systemd's start-limit guards are parsed from `[Unit]`, NOT `[Service]`. Placing them under `[Service]` causes systemd to log `Unknown key 'StartLimitIntervalSec' in section [Service], ignoring.` — the directives are silently dropped and the service runs with no burst cap.

**The regression this caused:** when the user's on-disk v4 unit shipped with both directives under `[Service]`, a transient `grab failed: Device or resource busy` (see the two-daemon race entry below) failed ~89 restart attempts in a row before the user noticed and stopped the unit manually. With the guards correctly under `[Unit]`, the same failure stops after 5 attempts in 60 s and the unit drops to `failed` state so the frontend toggle can surface the problem.

Mandatory location in every source-of-truth copy:

```
[Unit]
…
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
…
Restart=on-failure
RestartSec=3
# StartLimit* MUST NOT appear here
```

Applied to:
- [`deploy/systemd/loginext.service`](deploy/systemd/loginext.service:27) — canonical template shipped by PKGBUILD and install.sh
- [`ui/src-tauri/src/service.rs`](ui/src-tauri/src/service.rs:152) — heal-rewrite body (TEMPLATE_VERSION 4→5)

`TEMPLATE_VERSION` bumped to 5 so existing units pick up the fix on the next UI launch via [`heal_at_startup()`](ui/src-tauri/src/service.rs:360). If you bump the template body in the future, verify the placement with `systemd-analyze verify ~/.config/systemd/user/loginext.service` — it emits the same "Unknown key" warning systemd uses at load time, so the check is cheap and doesn't require an actual restart.

---

## Two-daemon grab race: UI must NOT spawn-detach when systemd is managing the unit (2026-05-07)

The C++ daemon calls `libevdev_grab(LIBEVDEV_GRAB)` unconditionally on the MX Master 3S event node during init. The grab is exclusive — only one process at a time can hold it — so any scenario that puts two daemons on disk simultaneously ends with one of them spinning in a restart loop emitting `[loginext] grab failed: Device or resource busy` / `status=1/FAILURE`.

**How two daemons started co-existing:** [`lib.rs::run()`](ui/src-tauri/src/lib.rs:537) used to call [`heal_at_startup()`](ui/src-tauri/src/service.rs:360) followed immediately by [`daemon::ensure_running()`](ui/src-tauri/src/daemon.rs:403). `heal_at_startup()` ran `systemctl --user daemon-reload && try-restart loginext.service` when it detected a template-version bump — fine in isolation, but in the brief window between "old daemon exits on SIGTERM" and "new daemon binds the socket", `ensure_running()`'s socket probe saw a dead socket and spawn-detached an *additional* daemon on top of systemd's restart. Both processes then raced for `EVIOCGRAB`; whichever got there first won, the other exited with EBUSY, and systemd restarted it (see the StartLimit entry above for why it then looped ~90 times instead of giving up after 5).

**The fix is two-pronged.** Belt and braces: either alone would prevent the specific scenario, both together also catch every neighbouring variant (external `systemctl --user restart` on a UI that's already open, a UI launched seconds after cold-boot autostart, etc.).

1. **[`heal_at_startup()`](ui/src-tauri/src/service.rs:360) now blocks until `query_state()` reports `active=true` after its `try-restart` (budget: 3 s).** This shrinks the socket-dead window so `ensure_running()` would observe the new daemon as already up on the vast majority of hosts.

2. **[`lib.rs::run()`](ui/src-tauri/src/lib.rs:537) now branches on `service::query_state()`.** When the unit is available AND either active or enabled, we call the new [`daemon::wait_for_running()`](ui/src-tauri/src/daemon.rs:401) — which polls the socket for up to 5 s and NEVER spawn-detaches — instead of `ensure_running()`. The legacy spawn-detach path is kept strictly as a fallback for hosts with no unit file installed (fresh cmake-only builds).

The rule generalises: any future UI code path that acts on the daemon socket must check `service::query_state()` first and route through `daemon::wait_for_running()` when systemd is in charge. The grab is the single most sensitive shared resource in the stack, and it has exactly one allowed owner at a time. Spawn-detaching "just to be safe" is a regression vector.

**Verification recipe when touching either the heal path or the UI startup order:**

```sh
# Kill any running daemons, disable the unit, close the UI.
systemctl --user disable --now loginext.service
pkill -9 loginext

# Run the solo-daemon smoke test — must print "exclusive grab acquired".
timeout 3 ./build/loginext

# Now enable via UI toggle, then reboot. journalctl --user -u loginext.service
# must NOT show "grab failed: Device or resource busy" on first boot.
```

If step 3 ever shows the EBUSY line again, grep the codebase for any new `daemon::ensure_running()` call that hasn't been gated on `query_state()` — that's the regression.

---

## systemd unit self-heal: must run at every UI launch, not only on toggle click (2026-05-07)

`heal_unit_if_stale()` was originally wired only into `service::enable_now()`, which fires on the DAEMON OFFLINE→ONLINE toggle click. That works for an *idle* unit (user clicks ON, heal runs, unit gets fixed, daemon starts) but it does not work for an *enabled, failing* unit.

**The boot-loop pattern the heal-on-click flow could not break:**
1. User toggles ON in some past session. Unit is enabled.
2. User reboots. systemd autostarts the unit at boot. Unit hits `226/NAMESPACE` (or any other systemd-side regression) on every restart attempt.
3. User opens the UI to investigate. The status toggle reads "ON" because `is-active` returns `activating` (which the toggle treats as on).
4. User has no reason to click the toggle — it already says ON. `enable_now()` never runs. `heal_unit_if_stale()` never runs.
5. The unit stays broken across every reboot, indefinitely.

**The fix:** add `service::heal_at_startup()` and call it unconditionally from `lib.rs::run()` *before* the daemon spawn-detached probe. The heal:
- Skips work entirely when `service::query_state().available` is false (no unit installed → don't write one out of nowhere; the user hasn't asked for systemd integration).
- Compares **both** `ExecStart=` AND a `# loginext-template-version:` comment marker to the constants in `service.rs`. Either drift triggers a rewrite.
- After a rewrite, runs `daemon-reload` and `try-restart` so the change is picked up *immediately*. `try-restart` is a no-op when the unit is inactive, so a healthy host pays nothing.

**The template-version marker is the channel for shipping unit-body fixes.** Bump `TEMPLATE_VERSION` in `service.rs` whenever the body changes for behavioural reasons (a new hardening directive, a new `Restart=` policy, etc.). Existing user units pick up the new body the next time they launch the UI — no manual `systemctl edit`, no install.sh re-run, no support thread.

**Where user customisations should live instead:** `~/.config/systemd/user/loginext.service.d/override.conf`. systemd's drop-in directory mechanism merges these on top of the main unit body without being touched by the heal. We document this in the heal log line and the rewritten unit's header comment.

---

## DAEMON ONLINE/OFFLINE toggle: systemd is the source of truth (2026-05-07)

The toggle drives `systemctl --user enable/disable --now loginext.service` directly. The toggle's position on UI launch is read from `is-active` + `is-enabled`, not from a localStorage flag. Click ON → service enabled (autostarts at next login) AND started now. Click OFF → disabled AND stopped.

**Why systemd, not an IPC `quit` flag:** the user's "ON" intent should mean "start now AND start at next login". The legacy `daemon_forced_off` localStorage approach didn't survive logout — a user who left the toggle ON had to reopen the UI after every reboot to spawn the daemon. systemd is the canonical Linux place for this state.

**Why we kept the spawn/kill path as a fallback:** if the unit file isn't installed (older build, manual cmake-only flow), `service_state()` returns `available=false` and the toggle reverts to `LifecycleMode::spawn` — the legacy spawn-detached + `kill_daemon` path. No functional regression, the user just loses the autostart bit. The mode is decided once on first paint based on systemd's response.

**The unit file self-heal** (`service::heal_unit_if_stale()`): the user's `~/.config/systemd/user/loginext.service` may have been written before the daemon binary moved (`/usr/local/bin/loginext` → `/usr/bin/loginext` between 2026-04-26 and 2026-05-07, OR `~/.local/bin/loginext` after install.sh). On the first `service_enable()` from the toggle, we check whether `ExecStart=`'s binary path matches `daemon::resolve_daemon_binary()`. If not, we rewrite the unit file from a canonical template that uses the resolved path. The full template — not just the ExecStart line — is rewritten because patching a half-customised unit is fragile. **User customisations should go in `~/.config/systemd/user/loginext.service.d/override.conf`**, which the heal does not touch.

We never write to `/usr/lib/systemd/user/`. That's the package's responsibility, and a user-mode UI shouldn't need root.

**Heartbeat re-reads service state every 5 s** so an externally-issued `systemctl --user start/stop` is reflected in the toggle without a UI restart.

---

## Window-focus paradox: Always-on-top pin in the header (2026-05-07)

Adding a per-app rule requires focusing the target app to populate "Currently focused", which sends LogiNext to the back. The user can't reach the "+ Add rule" button without WM-level "Keep Above" or tiling.

**The fix is a pin button in the UI header** that calls Tauri's `Window::set_always_on_top(true)`. State persists in `localStorage` and is reapplied on every UI launch.

**Why we didn't go with the alternative "Recent Apps history list":** keeping a list of last-N detected apps in the frontend is also useful, but it doesn't solve the actual paradox — the user STILL has to focus the target app to populate the list. The pin solves the immediate workflow gap; the history list would be a refinement that builds on it. The pin is the canonical solution for this kind of UX.

**Wayland compositors that gate Always-on-Top behind a permission** can refuse the call. The frontend `console.warn`s on failure and reverts the visual pin state — the persisted bit isn't flipped on failure either, so a user's intent is never silently misrepresented.

### KWin D-Bus scripting fallback (2026-05-07)

Tauri's `Window::set_always_on_top()` calls `gtk_window_set_keep_above` internally. On KDE Plasma 6 Wayland, KWin may ignore this GTK-requested hint — the compositor has final authority over window stacking, and KWin's focus-stealing prevention can override client-requested states.

**The fix is a two-tier approach** in [`ui/src-tauri/src/lib.rs::set_always_on_top`](ui/src-tauri/src/lib.rs:273):

1. **Tier 1 — Tauri native:** `window.set_always_on_top(on_top)` sets the xdg_toplevel hint. Works on X11 and compositors that honour the hint. Always called first so non-KDE hosts get correct behaviour. **Failures here do NOT short-circuit the function.** Tauri 2 on Wayland can report an error from `set_always_on_top` (via the GTK3 backend) even on compositors where Tier 2 would have succeeded — the previous implementation used the `?` operator here and silently skipped Tier 2, which is what caused the production regression where the pin button did nothing and the KWin journal showed zero D-Bus activity.

2. **Tier 2 — KWin D-Bus scripting via `loadScript` + `Script.run`:** When `XDG_CURRENT_DESKTOP` contains `KDE`, the JS snippet is written to `$XDG_RUNTIME_DIR/loginext-keepabove-<pid>.js`, then registered with KWin via `org.kde.kwin.Scripting.loadScript(path, name)` (service name: `org.kde.KWin`, path: `/Scripting`). The returned integer script id is parsed out of the qdbus stdout and used to build `/Scripting/Script<id>`, which we then invoke via `org.kde.kwin.Script.run`. A follow-up `Script.stop` keeps KWin's script registry tidy across repeated toggles. The snippet matches windows by caption *or* resourceClass (`loginext` substring) — the dual match widens the catch because Wayland sessions sometimes report a blank caption during early startup while always exposing the correct xdg_toplevel app_id.

   **Why `loadScript` and not `runScript(QString)` (inline JS):** `runScript` has been inconsistent across Plasma 6 point releases — some builds require a privileged caller, others have dropped it entirely from the scripting surface. `loadScript` is the public, documented, stable API since Plasma 5.x; the extra temp-file hop costs roughly one disk write of ~500 B and is immaterial next to the D-Bus round-trip itself. A previous version of this file attempted `runScript` with inline JS against a wrong *service* name (`org.kde.kwin.Scripting`, which is the *interface* — the service is `org.kde.KWin`). Every call failed with "Service not found", no traffic reached KWin, the user saw a silent no-op. Both bugs are fixed in the current implementation; keep them fixed.

3. **Tool fallbacks:** we try `qdbus6` → `qdbus-qt6` → `qdbus` in that order. stdout/stderr are captured via `.output()` (not `.status()`) so we can parse the script id and log a useful diagnostic if every tool fails. Each attempt emits a line to stderr — `[loginext-ui] pin: qdbus6 [… args …] ok (stdout="12")` on success — so a terminal-launched debug build shows exactly which tier did the work.

The D-Bus call is best-effort: it silently no-ops when qdbus isn't available, KWin isn't running, or the scripting interface is disabled. In all these cases, Tier 1's hint is already set, so the running window manager gets the right signal.

**Do not** remove the Tauri native call and rely solely on the KWin path — that would break the pin on X11, non-KDE Wayland compositors (Hyprland, Sway), and KDE X11 sessions. **Do not** reintroduce the `?` operator after the Tauri call — the explicit `match { Ok, Err }` pattern is load-bearing and the only reason Tier 2 runs on Wayland KDE at all. **Do not** switch to `runScript(QString)` even if Plasma ships a new build that exposes it — the variance across point releases is a regression waiting to happen.

---

## systemd unit: `WantedBy=default.target`, not `graphical-session.target` (2026-05-07 / reverted and superseded)

The unit's `[Install]` section started as `WantedBy=default.target`, was briefly moved to `WantedBy=graphical-session.target` in a same-day iteration, and is now back on `WantedBy=default.target`. The second iteration regressed cold-boot autostart and had to be reverted. Both decisions are documented so a future session doesn't re-litigate the question.

**The failure mode the `graphical-session.target` move was trying to fix:** `PartOf=graphical-session.target` stops the service when the target stops OR restarts. If `graphical-session.target` restarts mid-login (KDE does this on some Plasma 6 / SDDM hand-offs), the service stops — and a `WantedBy=default.target` symlink does not re-fire because `default.target` was already reached earlier in user-manager bring-up. Net: service stays stopped until the UI's `ensure_running()` spawns it manually.

**The failure mode `graphical-session.target` introduced:** `graphical-session.target` is a reverse-dependency-only target. Per `systemd.special(7)` it is declared with `RefuseManualStart=yes` and is activated *only* when a desktop-session service (Plasma's `plasma-workspace.service`, GNOME's `gnome-session-manager@.service`, etc.) explicitly pulls it in with `Wants=graphical-session.target`. On several Plasma 6 boot paths — specifically SDDM → Wayland on Arch / CachyOS, which is the agents.md target platform — the user-manager session bring-up does NOT pull the target in. The `.wants/loginext.service` symlink created by `systemctl --user enable` sits there forever, the target never activates, and the daemon never starts at boot. `is-enabled` correctly returns `enabled`, so the UI toggle reads ON and the user has no reason to click it — exactly the silent-no-op class of bug `heal_at_startup` was written to prevent.

**The settled answer:** `WantedBy=default.target`. `default.target` is the canonical user-manager autostart target and is always reached on bring-up regardless of display-manager wiring. Session-lifecycle coupling is preserved by keeping `PartOf=graphical-session.target` in `[Unit]`: stop and restart propagation are independent of the Install target. If the graphical session restarts mid-login, `PartOf=` stops the unit, the daemon exits on SIGTERM, `Restart=on-failure` with `RestartForceExitStatus=143` catches the signal-143 exit code, and the unit restarts. This is the correct interaction between `PartOf=` and `Restart=` — no `WantedBy=` acrobatics needed.

Applied to both source-of-truth files:
- [`deploy/systemd/loginext.service`](deploy/systemd/loginext.service:80) — canonical template shipped by PKGBUILD and install.sh
- [`ui/src-tauri/src/service.rs`](ui/src-tauri/src/service.rs:193) — heal template body (TEMPLATE_VERSION 3→4)

`TEMPLATE_VERSION` is 4 so existing users whose units were rewritten at v3 pick up the correction on the next UI launch via `heal_at_startup()`. A user whose daemon has been silently failing to autostart since the v3 rewrite sees it come back the first time they open the UI after the new binary lands — no manual `systemctl edit`, no install.sh re-run.

**The mandatory pattern for future services that use `PartOf=`:** rely on `PartOf=` + `Restart=on-failure` + `RestartForceExitStatus=143` for stop/restart propagation from the parent target. Do NOT couple `WantedBy=` to a reverse-dependency-only target just because `PartOf=` points there — the two directives have different lifecycles, and `WantedBy=default.target` is the right answer for "autostart when the user logs in".

---

## Per-app scope listener quirks (2026-05-03)

- **Hash 0 is reserved as the global sentinel.** `scope::hash_app()` re-rolls into `fnv_prime` if the FNV-1a result happens to land on 0. A different rule store implementation (e.g. cuckoo, robin-hood) must preserve this invariant: the hot-path `lookup()` short-circuits on 0 and an in-table 0 would mark the slot empty. Don't change without coordinated edits to both `app_hash.hpp` and `rules.hpp`.
- **Listener publishes via `memory_order_relaxed`.** The atomic carries an integer, not a pointer; the only happens-before requirement is "eventually visible". A focus change racing with a thumb-wheel emit can produce one event resolved against the previous app's preset — that's an accepted behaviour, not a bug. Do not add stronger orderings unless you can demonstrate a correctness violation.
- **Hyprland backend uses `socket2.sock` (event stream), not `socket.sock` (request channel).** The two sockets have similar names and very different semantics; using the request channel would require active polling, which defeats the whole point of an async listener.
- **X11 backend prefers `WM_CLASS.instance_name` over `class_name`.** Firefox reports `Navigator` as the class but `navigator` as the instance — and Chrome / Chromium follow the same pattern. Instance name is more specific and matches what a user typing `firefox` in `app_rules.txt` actually expects (after lower-casing). Some niche apps publish only the class name; the loop falls back to it automatically.
- **One D-Bus name per host.** `org.loginext.WindowFocus` is exclusive — running two daemons on the same user session means the second one's `sd_bus_request_name` fails with EEXIST, the listener falls through to the next backend, and that daemon silently ends up on X11 / wayland-protocol. Logged at WARN.
- **Sway / Mir and other non-KDE Wayland compositors are not yet wired.** The wayland branch tries the KDE protocol, fails on those compositors, and falls through to X11 (which works under XWayland for legacy apps). Adding `ext_foreign_toplevel_list_v1` or `wlr_foreign_toplevel_management_v1` is a new bind block alongside the existing `org_kde_plasma_window_management` registry handler; the surrounding wayland event-loop scaffold is already correct.

---

## UI scroll perf: GPU-layer columns + content-visibility cards (2026-05-06)

`.app__col` is a scrollable container. Without intervention, every scroll frame triggered repaints of the whole shell — the heavy 18 px box-shadow blurs on every `.card` and `.list-item` are the bottleneck on WebKitGTK 6.

**The fix is two-fold:**

1. **`contain: paint` + `transform: translateZ(0)` on `.app__col`.** Promotes each column to its own compositor layer so scrolling is GPU-composited, and walls off paints so a column's scroll doesn't repaint sibling columns or the status bar.
2. **`content-visibility: auto` + `contain-intrinsic-size: auto 240px` on `.app__col > .card`.** Cards that scroll past the viewport stop participating in layout / paint until they return. The intrinsic-size hint keeps scrollbar geometry stable across virtualisation transitions.

Lift: ~30 → 60 FPS on Plasma 6 / 1440p+ during heavy column scrolling.

**`will-change` was deliberately removed from `.status-toggle`.** Combined with the breathing keyframe animation, it caused the badge's compositor layer to be pinned to stale layout for ~2 s after a window resize. The loss of "perf hint" doesn't matter for a single low-frequency element; correctness wins.

`-webkit-overflow-scrolling: touch` is set on `.app__col` for WebKit's smooth-scroll heuristic — even on desktop WebKitGTK respects it.

---

## "+ Add rule" flicker: the real cause was DOM thrash from polling, not CSS (2026-05-07)

The `+ Add rule` button flickered at exactly 4 Hz when the cursor sat on it. Two earlier fixes were applied to the CSS — first removing a `translateY(-1px)` hover-lift, then padding the `box-shadow` declarations to matching shadow counts — and **neither resolved the symptom**. The second fix is documented as a discipline below because it's still good hygiene, but the actual bug was elsewhere.

**The actual cause:** `renderActive()` in `ui/src/views/rules.ts` rebuilds the entire `.rules-active__row` (including the `+ Add rule` button) every time the active-app poll ticks. The poll cadence is 250 ms = 4 Hz. Each rebuild:
1. Destroys the existing button → cursor's `:hover` pseudo-class drops
2. Re-creates an identical button → `:hover` re-detected on the next frame
3. Browser strobes the visual hover state during the gap

The previous CSS fix could not address this — no `box-shadow` transition curve is the right answer when the element itself is being destroyed and re-created four times per second.

**The fix:** compute a fingerprint over every input `renderActive()` reads (active-app name/source/global preset, rule existence + preset id, presets-list length) and short-circuit the rebuild when the fingerprint matches the previous render. In the steady state — focused app unchanged, rule list unchanged — `renderActive()` becomes a single string compare and an early return. The button stays mounted. `:hover` survives.

**Why a fingerprint and not a more granular reactive system:** the rendered tree is small (4–5 elements), the fingerprint is cheap to compute, and the cost of getting the dependency tracking wrong on a more granular system is the same flicker we just fixed. A coarse "did anything change?" gate at the entry of the render function is the smallest fix that works.

**Discipline retained from the prior CSS fix (still good hygiene):** when adding a new neumorphic button, base/hover/active `box-shadow` declarations should still have matching shadow counts. Mismatched counts force a discrete crossfade (~50 ms strobe) on every transition. The cost of a `0 0 12px transparent` placeholder slot is a single skipped paint per shadow — much cheaper than discovering the discrete-crossfade gotcha on the next button. `box-shadow` interpolates per-shadow only when the source and target lists have the same number of declarations.

**For any future widget that's re-rendered on a poll:** apply the fingerprint pattern. The active-app row is the canonical example.

---

## Quirks of the `--debug-events` flag (2026-05-02)

- **Dump goes to stderr, not stdout.** Stdout is reserved for the daemon's line-delimited JSON IPC stream; mixing the raw event dump in would corrupt any consumer reading from a pipe. Stderr also matches the rest of the daemon's lifecycle output, so `sudo ./build/loginext --debug-events 2>&1 | grep KEY` works as expected during discovery.
- **Bypasses `LX_*` log levels on purpose.** The dump uses `std::fprintf(stderr, …)` directly so it appears even with `--quiet`, and so it doesn't pollute the structured file log at `$XDG_STATE_HOME/loginext/daemon.log` with Trace-volume entries.
- **Requires DAEMON OFFLINE in the UI before launch.** The auto-spawned daemon already holds the exclusive `libevdev` grab; a second instance will fail to grab. Toggle the daemon off first (the systemd path will disable + stop the unit; on hosts without the unit it kills via IPC), run the discovery binary in a terminal, then toggle it back on.
- **The hot-path branch is `__builtin_expect(debug_events, 0)`.** Do not refactor it into a `std::function` callback or a virtual hook — the whole point is that production runs pay one predicted-not-taken byte test and nothing else.

---

## UI state-sync correctness (DOM as source of truth)

UI components that own state treat the DOM attribute (`aria-selected`, `aria-checked`) as the single source of truth. Closure-captured state is a known trap.

**The bug we already fixed (2026-04-25):** `Segmented` was holding a stale closure copy of the active option. `fetchInitialState()` updated `aria-selected` on the DOM but never touched the closure, so the first click on the already-highlighted segment short-circuited and did nothing. `segmented.ts` and `toggle.ts` are now both DOM-driven; new components must follow the same pattern.

`applyCurrentSettings` short-circuits when `(mode, invert, preset)` matches the last-applied tuple. `fetchInitialState` seeds `lastAppliedMode` / `lastAppliedInvert` / `lastAppliedPreset` from the daemon so the first click only fires an IPC when the value actually changes. Without this seeding the first user gesture would always re-write a config that already matches.

---

## WebKitGTK Wayland Error 71 fix (2026-05-03)

`Gdk-Message: Error 71 (Protocol error) dispatching to Wayland display.` on Plasma 6 + WebKitGTK 6.

**Fix:** `apply_webkit_wayland_workarounds()` runs at the very top of `run()` in `ui/src-tauri/src/lib.rs`, before any GTK init. Sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` (always on Linux) and `WEBKIT_DISABLE_COMPOSITING_MODE=1` (only on Wayland).

Both are conditional `set_if_unset`: a user-set value (or one from the `.desktop` launcher's `Exec=env WEBKIT_DISABLE_DMABUF_RENDERER=1 …` prefix) is never clobbered. This had to live in the binary, not just the launcher, because terminal-launched test runs (`./ui/src-tauri/target/release/loginext-ui` from the repo) bypass the launcher entirely.

If the WebKit env vars are insufficient on a particular host (residual Nvidia / Mesa-mismatch), the documented escape hatch is `GDK_BACKEND=x11 ./loginext-ui` to force XWayland. We don't make this the default — XWayland forfeits native Wayland integration (clipboard quirks, scaling, fractional DPI).

---

## Deferred audit findings — *do not "fix" these without reading the rationale*

### F5 — `default_config_path()` allocates `std::string` on every call

- **Severity:** Low.
- **Why deferred:** It is only ever called once, at startup, when the result is stashed into `AppContext::config_path`. The reload path reads the cached field, never re-resolves the path. The two or three transient `std::string` allocations are paid once on a cold startup; rewriting them as a static buffer would buy nothing measurable and complicate the API.
- **What to revisit:** If we ever support multi-profile configs (path resolved per request) or per-user reloading inside an event handler, switch to a fixed-capacity char buffer at that point.

### F6 — `device.cpp` opens every `/dev/input/event*` node sequentially

- **Severity:** Low.
- **Why deferred:** ~1 ms total on a typical desktop (10–20 event nodes). Hot-pluggability isn't a Phase-2 goal; the daemon enumerates once at startup. `libudev` would violate the "no frameworks" rule and pre-filtering via `/proc/bus/input/devices` is the right shape but only worth it once we add live attach/detach.
- **What to revisit:** When Phase 3 introduces hot-plug for additional Logitech devices, switch enumeration to `/proc/bus/input/devices` parsing + per-event-node open only on candidates whose vendor:product matches.

### F10 — `check_damping()` runs on every `REL_HWHEEL` event

- **Severity:** Low.
- **Why deferred:** Cost is two comparisons + one subtraction per event (≈5 ns). When called from the event handler the silence delta is always near zero, so the function returns at the first guard — no work. Moving it to a separate timer would cost more in scheduling complexity than it saves.
- **What to revisit:** Only if profiling ever shows `on_event` itself becoming a hotspot. It currently isn't — the `process_hwheel` heuristics dominate.

### F13 — `Parser` class in `loader.cpp` is the only OOP construct

- **Severity:** Low (style only).
- **Why deferred:** The class is contained in an anonymous namespace, doesn't leak, and is genuinely the right shape for a streaming parser — splitting it into free functions would just spread the cursor `pos_` across a dozen signatures without making anything clearer. The agents.md rule is "no OOP *bloat*"; this is OOP *fit*.
- **What to revisit:** When the IPC schema or config schema gains nesting / numbers / arrays, replace the whole parser with a single-header library (per the agents.md note). At that point delete the class entirely; do not extend it.

---

## Shipped audit findings — historical detail (F1–F4, F7–F9, F11–F12, F14–F15)

Full descriptions retained for posterity. The fix is already in the codebase; the entry is here as breadcrumbs for "why is this written this way?" archaeology.

### F1 — `volatile bool g_stop` was not async-signal-safe

The C++ standard does not guarantee that a `volatile bool` written in a signal handler is visible to the main thread without a data race. Only `volatile sig_atomic_t` or `std::atomic<T>` are safe. Under `-O2`+ the loop in `run_loop` (`while (!*stop)`) could theoretically be hoisted into an infinite loop. Fixed by switching to `volatile sig_atomic_t`, matching the existing `g_reload` pattern.

### F2 — `process_timer()` ignored the `timerfd` read result

`[[maybe_unused]] auto r = read(...)` silenced the warning but did not handle failure. If `read()` returned `-1` with `EAGAIN` (possible after races) or `0`, the timerfd stayed readable because the expiration counter was never consumed. The level-triggered epoll registration would fire again immediately → busy loop → 100% CPU. Fixed by checking the return value and short-circuiting on partial reads.

### F3 — Silent `write()` failure on the passthrough path

`emit_passthrough()` and `write_event()` dropped events on `write()` failure with no diagnostic. uinput buffer-full is rare but the failure mode was a total mouse freeze with nothing in the log. Fixed by routing the error through the structured logger at Warn level (so it lands in the file log without spamming stderr in normal operation).

### F4 — Config reload allocated on the event-loop thread

`std::ifstream` + `std::stringstream` + repeated `std::string` operations inside `load_settings()`, called from `on_reload()` on the same thread as the event loop. Reload is rare but the rule is "no heap allocation in the event loop." Replaced with a single `open()` + `read()` into a 4 KiB stack buffer. Config files larger than 4 KiB are now rejected — fine for the flat schema, will be revisited if v2 nests.

### F7 — `<cmath>` for a single `std::abs(int)`

`<cmath>` pulls in floating-point math symbols never used in `scroll_state.cpp`. Switched to `<cstdlib>` (where `std::abs(int)` actually lives).

### F8 — `on_reload()` reset `ScrollState` field-by-field

Six manual assignments, easy to forget when adding a new field. Replaced with `app->scroll = {}` aggregate init. Future-proof and one line.

### F9 — Ring-buffer used `% max_queued_actions`

The compiler *should* optimise `% 8` to `& 7` since `max_queued_actions` is `constexpr int = 8`, but the cast to `uint8_t` may interfere on some toolchains. Replaced with explicit `& (max_queued_actions - 1)` plus `static_assert((max_queued_actions & (max_queued_actions - 1)) == 0)` so the constraint is documented in code.

### F11 — Missing LTO and `-march=native`

Added optional CMake flags `LOGINEXT_LTO` (sets `INTERPROCEDURAL_OPTIMIZATION TRUE`) and `LOGINEXT_NATIVE` (`-march=native`), both default ON. Appropriate for a single-machine daemon; non-portable binary is the explicit tradeoff.

### F12 — 4–6 `write()` syscalls per tab switch

`tap_key_combo()` was emitting Ctrl-down, Tab-down, SYN, Tab-up, Ctrl-up, SYN as six separate `write()`s. uinput accepts batched writes, so the whole sequence now packs into a single `write()` of an `input_event[8]` array. Saves ~10–20 µs per emission and keeps the kernel from re-entering on each event.

### F14 — Virtual mouse missing `EV_MSC` capability

The passthrough path re-emitted `EV_MSC` events but `setup_mouse()` never registered `EV_MSC` + `MSC_SCAN` capabilities, so uinput silently dropped them. The MX Master 3S emits `MSC_SCAN` for button presses; consumers that care (some apps under X11) lost them. Fixed by registering the capability.

### F15 — `SYN_DROPPED` re-emitted on the virtual device

`SYN_DROPPED` is libevdev's signal that the kernel buffer overflowed. Re-emitting it on the virtual device was semantically wrong (the virtual device has its own buffer). Now filtered out before passthrough.

---

## Validation strategy (still applicable)

These are the verification recipes for the fixes above. Run them after any change to the hot path or signal handlers.

1. **Signal safety (F1).** Send `SIGINT` to the daemon under a sustained event load (`evemu-record` + `evemu-play`). Verify clean exit within 100 ms, ×100.
2. **Timerfd drain (F2).** Stress the pacer (`enqueue_action` × 100 in a tight loop) and watch CPU usage. CPU must stay < 5 %.
3. **Write batching (F12).** `strace -e write -c loginext` before and after a Low → High → Low cycle. Compare syscall counts per tab switch.
4. **Memory profile (F4).** `valgrind --tool=massif` across 10 reload cycles. Heap must not grow beyond initial allocation.
5. **MSC passthrough (F14).** `evtest` on the virtual mouse device while clicking the MX Master 3S buttons; verify `MSC_SCAN` events appear.
6. **Regression sweep.** Manual scroll on each sensitivity mode after every change to `heuristics/` or `core/`. Verify ghost filtering, cooldown, damping still behave.
