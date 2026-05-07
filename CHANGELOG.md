# LogiNext — Changelog

User-visible changes and resolved issues, newest first. Architectural decisions and the long-form rationale behind them live in [KNOWN_ISSUES.md](./KNOWN_ISSUES.md). Roadmap and what's next live in [progress.md](./progress.md). Active performance discipline lives in [OPTIMIZATIONS.md](./OPTIMIZATIONS.md).

---

## 1.0.0 — 2026-05-08 — Stable release: cold-boot fix, listener spinner fix, secure IPC

The Phase-2 surface (thumb-wheel engine + Tauri UI + systemd lifecycle + per-app rules + KWin focus bridge) is now production-stable on Plasma 6 / CachyOS. Everything below was either a v0.x carry-over fix or new in this cycle.

### Fixed
- **Listener thread CPU spinner — sustained ~50% of a core after 30 s.** `kwin_dbus_loop` had a 30-second diagnostic deadline (`bind_us + kwin_warn_after_us`) used both to fire a help message *and* as a `select()` timeout. When `Activated()` events did arrive inside the window (the healthy case), the warning condition `!l->kwin_received_any` failed → the warning never fired → `warned_no_kwin_events` stayed `false` → the deadline kept being computed even after it was in the past → `select(timeout=0)` spun ~720 000 iterations/sec, observed via the new `--debug-perf` counter. Fix: also flip `warned_no_kwin_events = true` when events arrive inside the window. After fix, idle daemon consumes ~40 ms CPU per minute (was minutes per minute) — verified end-to-end. See [src/scope/listener.cpp](./src/scope/listener.cpp) `kwin_dbus_loop` and [CHANGELOG note](./CHANGELOG.md) for the full perf trace.
- **Cold-boot rule activation — wheel acted as raw passthrough until the UI was launched.** The listener thread gated each compositor backend on env vars (`XDG_CURRENT_DESKTOP`, `WAYLAND_DISPLAY`, `DISPLAY`). On a `WantedBy=default.target` cold boot, PAM exports `XDG_RUNTIME_DIR` *before* systemd-user starts our service but Plasma exports the session vars *after* — leaving our process's `environ` permanently empty in those slots (`getenv()` doesn't refresh in-process). Replaced env-var gates with three direct probes: `org.kde.KWin` `NameHasOwner` on the user bus, `stat()` of `$XDG_RUNTIME_DIR/wayland-{0..3}`, and an `xcb_connect()` retry across `:0` / `:1`. Wrapped in a polling loop (30 × 2 s = 60 s grace) so the listener catches up the moment any compositor surfaces. See [src/scope/listener.cpp](./src/scope/listener.cpp) `thread_main`.
- **Cold-boot KWin bootstrap — script's first `publishCurrent()` could fire before the daemon claimed its name.** When KWin and the daemon race on cold boot, the persistent `loginext-focus` script's load-time `publishCurrent()` call gets dropped by the bus broker as `NameHasNoOwner`, leaving `active_app_hash` at 0 until the next 2-second heartbeat (or until the user opened the UI, which `register_kwin_script`'d everything). Added a two-layer bootstrap right after `sd_bus_request_name` succeeds: (1) write a tiny one-shot inline KWin script to `$XDG_STATE_HOME/loginext/kwin-bootstrap-<pid>.js` and run it via `org.kde.kwin.Scripting.loadScript` + `Script.run()` — bypasses the persistent script entirely, works even if `loginext-focusEnabled` is `false` in `kwinrc`; (2) fall back to `org.kde.KWin.reconfigure` if step 1 fails (older KWin / sandbox blocking the file write). Verified: focus event published within ~50 ms of the bootstrap call.
- **Daemon failed to persist after reboot (`start-limit-hit` after 5 burst restarts).** The unit had `Restart=on-failure` with `RestartSec=3` and `StartLimitIntervalSec=60`/`Burst=5` — a slow udev enumeration could chew through the burst budget before `/dev/input/event*` appeared and latch the unit dead. Added bounded udev-retry inside `find_device()` (10 attempts × 2 s), bumped `RestartSec=15` and `StartLimitIntervalSec=120` so 5 burst attempts span ~75 s of real grace. See [src/main.cpp](./src/main.cpp) and the v6/v7/v8 history block in [ui/src-tauri/src/service.rs](./ui/src-tauri/src/service.rs).
- **Sustained 30–50% CPU triggering systemd-oomd PSI kills.** With the listener spinner gone, the unit now needs near-zero ceilings to prove it's healthy. Added cgroup limits to the unit template: `MemoryHigh=32M`, `MemoryMax=64M`, `TasksMax=32`, `CPUQuota=50%`, plus `ManagedOOMPreference=avoid` / `ManagedOOMMemoryPressure=kill` / `ManagedOOMSwap=kill` to deprioritise this cgroup if pressure ever builds. Peak observed RSS is ~2.4 MB, so the ceilings are 50× headroom — generous, but capped if the pacer state machine ever wedges.
- **UI `systemd-managed socket … did not come up within 5000ms` on cold boot.** IPC server was initialised after the device-grab + emitter + pacer + epoll bring-up, so the listener socket file didn't exist while `find_device()` was retrying udev. Split IPC bring-up into two phases: bind the UDS *before* the device retry (so the UI's existence probe passes immediately), then attach the listener fd to epoll *after* `init_loop()`. The kernel queues client connects on the listen backlog and the daemon accepts them as soon as the event loop starts. See [src/main.cpp](./src/main.cpp).

### Added
- **`RuntimeDirectory=loginext` + `RuntimeDirectoryMode=0700` in the systemd unit (template v8).** Daemon socket lives at `$XDG_RUNTIME_DIR/loginext/loginext.sock` (mode 0700, owner-only). systemd creates the directory before the daemon starts and tears it down on stop, so stale sockets across crashes / unclean shutdowns are no longer possible. The earlier `ReadWritePaths=-%t/loginext.sock` workaround for `ProtectHome=read-only` is replaced by the canonical `RuntimeDirectory=` mechanism. Both [deploy/systemd/loginext.service](./deploy/systemd/loginext.service) and the in-Rust heal template in [ui/src-tauri/src/service.rs](./ui/src-tauri/src/service.rs) carry this.
- **`--debug-perf` CLI flag.** Per-second wakeup / event / sd_bus_process counters from both threads, dumped to stderr / journald: `perf[main]: N epoll wakeups, M dev wakeups (K input_events), …` and `perf[listener]: P select wakeups, Q zero-timeout, R sd_bus_process iters in 1.00s`. Catches busy-loop regressions in one minute of capture without strace; was the diagnostic that exposed the listener spinner above. Off by default; production-path cost is two integer increments per loop iteration.
- **`tauri:build` composite npm script.** `vite build && tauri build` in one command, used by `install.sh` and `PKGBUILD`. Documents the order explicitly so a future contributor doesn't try `cargo build` directly inside `src-tauri/` (which produces a binary that loads from `devUrl` and fails to render assets unless `npm run dev` is also running). Foot-gun documented in [ui/src-tauri/README.md](./ui/src-tauri/README.md).
- **`install.sh` enables the systemd unit by default.** Previously gated behind `--enable-service`; for a stable release this is the right default — the daemon comes up at every login without the user needing to open the UI first. Pass `--no-enable` to keep the unit installed but inactive (headless / debugging setups).

### Changed
- **Tauri bundle target restricted to `deb`.** Previously `"targets": "all"`, which triggered the AppImage bundler and failed every build with "square icon required" because we ship a single non-square icon. The deb bundler is permissive on icon dimensions and is the only target `install.sh` / `PKGBUILD` actually consume, so the AppImage path never paid for itself. See [ui/src-tauri/tauri.conf.json](./ui/src-tauri/tauri.conf.json).
- **Versions bumped to 1.0.0** in [ui/src-tauri/tauri.conf.json](./ui/src-tauri/tauri.conf.json) and [ui/package.json](./ui/package.json).

### Notes for upgrading from a v0.x build
The unit template auto-heals on the first UI launch after upgrade — `service::heal_at_startup()` detects a stale `# loginext-template-version:` marker and rewrites the unit body in place, then `try-restart`s. No manual `systemctl edit` or `install.sh` re-run required. If you previously customised `~/.config/systemd/user/loginext.service` by hand, move your changes into `~/.config/systemd/user/loginext.service.d/override.conf` (which the heal does not touch).

---

## 2026-05-07 — Boot-time service self-heal, "+ Add rule" DOM-thrash flicker, pin command shipped

### Fixed
- **Daemon never starts on reboot even though `systemctl --user is-enabled` returns `enabled` (`status=226/NAMESPACE`).** The systemd unit's `ReadWritePaths=%S/loginext %t/loginext.sock` listed the socket *file* — but the daemon only creates that file during init, and `ProtectHome=read-only` covers `/run/user/<uid>`. Result: systemd's mount-namespace builder bailed on the missing socket file *before* exec ran, the unit auto-restart-looped on every boot, and the daemon never came up. Both paths now use the `-` optional-path prefix (`ReadWritePaths=-%S/loginext -%t/loginext.sock`), letting systemd register the writable mount lazily so the daemon creates the socket as it normally would. Applied to both [deploy/systemd/loginext.service](./deploy/systemd/loginext.service) and the heal template in [ui/src-tauri/src/service.rs](./ui/src-tauri/src/service.rs).
- **`heal_unit_if_stale()` now runs at every UI launch, not only on toggle click.** The previous heal was gated behind the DAEMON OFFLINE→ONLINE click — which never fires when the unit is enabled+failing on every reboot, because the toggle reads "ON" (`is-active=activating`) and the user has no reason to click it. New `service::heal_at_startup()` runs unconditionally from `lib.rs::run()`, checks both ExecStart= drift AND a `# loginext-template-version:` marker, and rewrites the unit + `try-restart`s when either is stale. The fix for any future hardening regression now ships through the heal channel without requiring the user to re-run `install.sh`.
- **"+ Add rule" hover flicker (real cause this time: 4 Hz DOM thrash).** The previous CSS shadow-count fix did not address the actual bug — `renderActive()` called `activeRowEl.innerHTML = ""` and rebuilt the entire row, including the `+ Add rule` button, on every 250 ms active-app poll tick. A cursor sitting on the button lost its `:hover` state on every rebuild, presenting as a 4 Hz strobe. `renderActive()` now computes a fingerprint over its inputs (active-app name/source/global preset, rule existence + preset, presets-list length) and short-circuits when the fingerprint hasn't changed since the last render. The button now stays mounted across polls, and the cursor's `:hover` survives uninterrupted.
- **Pin button "doesn't work" — was actually a stale binary.** The May 3 install of `loginext-ui` predated the `set_always_on_top` Tauri command. The button rendered (frontend was rebuilt) but every click was rejected by the runtime as "unknown command". This release rebuilds + reinstalls the UI binary so the command is present.

### Notes for upgrading from any previous build
If your `loginext.service` was previously written by the earlier heal (template v1), it will be rewritten to v2 the first time you launch the UI after this update; no manual `systemctl edit` required. If you customised the unit body, move your changes into `~/.config/systemd/user/loginext.service.d/override.conf` (which the heal does not touch).

---

## 2026-05-07 — Always-on-top pin, systemd self-heal, scroll perf, focus latency

### Added
- **Always-on-top pin button** in the UI header. Click the pin icon to keep LogiNext above other windows; click again to release. State persists across launches via `localStorage`. Fixes the "window-focus paradox" where focusing a target app to add a per-app rule sent the LogiNext window behind it and the user couldn't reach the "+ Add rule" button.
- **systemd unit self-heal** in the DAEMON ONLINE/OFFLINE toggle. The first `systemctl --user enable --now loginext.service` call from the toggle now detects a stale `ExecStart=` (binary path that doesn't exist on disk — the classic `status=203/EXEC` failure on a host where install.sh's daemon path differs from the unit's) and rewrites `~/.config/systemd/user/loginext.service` to point at the resolved binary before enabling. No more manual edits or re-runs of install.sh.
- **systemd-driven DAEMON ONLINE/OFFLINE.** The toggle now wraps `systemctl --user is-enabled / is-active / enable --now / disable --now`. Click ON → service is enabled (autostarts at next login) and started now; click OFF → disabled and stopped. The toggle's position on UI launch is read from systemd directly, not localStorage, so an externally-issued `systemctl --user start/stop` is reflected within a heartbeat.
- **KWin focus-bridge heartbeat.** The KWin script now re-publishes the current active window every 2 seconds. The daemon dedupes on hash, so this generates zero noise while focus is unchanged — its only effect is to recover within 2 seconds when the daemon (re)acquires the bus name. Previously, restarting the daemon left the "Currently focused" row stale until the user switched windows.

### Fixed
- **"+ Add rule" hover flicker.** Two distinct causes converged on this button: a 1 px `translateY` hover-lift that yanked the bounding box off the cursor (causing a hover/no-hover loop) AND a `box-shadow` shadow-count mismatch between the base and hover states (which forced WebKitGTK to discrete-crossfade instead of interpolating). Both paths now padded to the same shadow count, no transform on hover.
- **Daemon-status badge layout lag.** The breathing animation kept its compositor layer pinned to the old layout for ~2 s after a window resize; `.status-bar` now opts in to `contain: layout style` and `will-change` was dropped from the pulsing toggle. The badge snaps to the resized layout instantly.
- **Per-app sensitivity AND invert decoupled.** Clicking "code" then changing the Sensitivity / Reverse-scroll-direction now writes to that rule's per-app override, not the global. Each chip carries an independent mode + invert; the global config is only touched when the Global chip is active. A defensive guard inside `applyCurrentSettings()` console.warns if a future regression tries to write the global from app context.
- **Currently-focused row latency.** Frontend poll cadence dropped from 3 s → 250 ms. Local UDS round-trip is sub-millisecond, the document-hidden short-circuit still gates polling when the window is minimised.

### Changed
- **App chip lifecycle.** Deselecting the active preset on a per-app chip now *unbinds* the rule (preset → empty) instead of deleting the chip. The chip stays in the context bar so the user can re-bind without re-focusing the app, and any per-app mode/invert overrides survive the unbind. Explicit deletion now lives on a small × that fades in on chip hover.
- **App rules file format.** `app_rules.txt` extended from `app=preset` to `app=preset[,mode[,invert]]`, each comma-separated field independent and may be empty (= inherit global). Backward-compatible with the single-field form. Sample file at [config/app_rules.example.txt](./config/app_rules.example.txt).
- **Scroll FPS in the column lists.** `.app__col` scrolls on its own GPU layer (`contain: paint` + `transform: translateZ(0)`), and `.card` uses `content-visibility: auto` so off-screen cards skip layout / paint until they return. Lifts FPS from ~30 to a stable 60 on Plasma 6 / WebKitGTK 6 with the heavy 18px box-shadow blurs.

---

## 2026-05-03 — Per-app rule engine, KWin DBus bridge, udev unprivileged access, plug-and-play install

### Added
- **Per-application rules.** A sidecar text file at `$XDG_CONFIG_HOME/loginext/app_rules.txt` overrides the active preset per focused window. The daemon hashes the app id (FNV-1a) at load time so the hot path runs an integer compare against an atomic published by the listener thread. Hot path adds exactly one relaxed atomic load + one masked array probe; on miss, the global preset wins.
- **Active-window detection across all major Linux compositors.** Hyprland (`HYPRLAND_INSTANCE_SIGNATURE` → IPC event stream), KDE Plasma (KWin D-Bus bridge — see below), KDE Plasma legacy / wlroots (`org_kde_plasma_window_management` v1), X11 / XWayland (`_NET_ACTIVE_WINDOW` PropertyNotify). Non-KDE Wayland compositors fall through to the X11 backend (which still works for legacy apps via XWayland).
- **KWin focus bridge for Plasma 6.** Plasma 6's KWin no longer advertises `org_kde_plasma_window_management` to regular Wayland clients. The shipped KWin script (`deploy/kwin/loginext-focus`) listens for `workspace.windowActivated` and forwards each event to the daemon's `org.loginext.WindowFocus.Activated(ss)` D-Bus method. Same pattern `kdotool` uses.
- **Context-aware preset selector.** Clicking the Thumb Wheel control reveals a context bar (Global + per-app icons). Click a chip to switch context; click a preset to bind/unbind for that context.
- **Zoom preset.** `Ctrl+Numpad+` / `Ctrl+Numpad-` mapped to right/left ticks. Universal compatibility across browsers, IDEs, GTK apps, and image viewers; works regardless of keyboard layout.
- **Passthrough preset (`PresetId::None`).** When bound (per-app or global), HWHEEL events bypass heuristics and forward as raw input — the wheel behaves as a normal unmapped device for that context.
- **Native Arch PKGBUILD.** `deploy/PKGBUILD` builds the project as a `loginext-git` VCS package: `pkgver()` resolves at build time from `git describe`, so a `git pull` + `makepkg -si` cleanly upgrades the installed copy via pacman. Builds the C++ daemon and the Tauri UI in one shot, installs to `/usr/bin`, drops the desktop entry + icon under `/usr/share`, stages the systemd user unit at `/usr/lib/systemd/user/loginext.service`. Distribution-friendly: `-DLOGINEXT_NATIVE=OFF` so the package is portable across x86_64 microarchitectures.
- **udev rules** at `deploy/udev/99-loginext.rules` granting `TAG+="uaccess"` ACL access (or `GROUP="input"` fallback) on `/dev/uinput` and the MX Master 3S event nodes. Both pacman and `install.sh` install the rules and run `udevadm control --reload-rules && udevadm trigger`. The daemon now runs unprivileged out of the box; `sudo loginext` is no longer recommended.
- **`--debug-events` CLI flag.** Dumps every raw `input_event` drained from `libevdev` to stderr for mapping unknown buttons. Production-path cost: one predicted-not-taken branch.
- **IPC `quit` command.** Cooperative daemon shutdown via the listener UDS — works regardless of process ownership (the legacy `kill(2)` path returned EPERM across uid boundaries).

### Changed
- **Auto-enable of the KWin focus bridge.** The Tauri shell now runs `kpackagetool6 --upgrade → --install → direct-copy fallback`, then `kwriteconfig6 --file kwinrc --group Plugins --key loginext-focusEnabled true`, then `qdbus6 org.kde.KWin /KWin reconfigure` on every UI launch. Pacman's post-install hook can't reach `~/.config/`, so the UI carries the responsibility — first launch on a fresh box is now plug-and-play.
- **Daemon log diagnostic for missing KWin script enablement.** A 30 s timer fires inside `kwin_dbus_loop` if no `Activated()` events arrived, printing the exact `kwriteconfig6` / `qdbus6` recovery commands to the daemon log. The UI's "Currently focused" row also refines its empty-name copy to point the user at System Settings → Window Management → KWin Scripts.
- **WebKitGTK Wayland workarounds applied at startup.** `WEBKIT_DISABLE_DMABUF_RENDERER=1` (always) and `WEBKIT_DISABLE_COMPOSITING_MODE=1` (Wayland only) are set at the very top of `run()`, before any GTK init. Fixes the `Error 71 (Protocol error) dispatching to Wayland display` crash on Plasma 6.
- **UI handles small windows gracefully.** `body` overflow is now `auto` (was `hidden`), `.app-shell` uses `min-height: 100%` so vertical scrolling kicks in cleanly when the window is squashed.

---

## 2026-04-26 — Production lifecycle, real neumorphism, structured logging

### Added
- **Detached daemon spawn from the UI.** Launching the Tauri shell probes `$XDG_RUNTIME_DIR/loginext.sock`; if the socket is dead it spawns the C++ daemon as a fully detached background process (`setsid`, stdio → `/dev/null`). Closing the UI no longer kills the daemon; reopening reconnects to the existing socket.
- **Daemon binary discovery.** Honours `$LOGINEXT_DAEMON`, then probes `../../build/loginext` relative to the UI exe (dev workflow), then `$PATH`, then `/usr/local/bin/loginext` and `/usr/bin/loginext`.
- **`daemon_status` / `daemon_respawn` Tauri commands.** UI surfaces the spawn outcome on first paint (`already_running`, `spawned`, `binary_not_found`, `spawn_failed`, `timeout`) and re-runs the spawn check from the heartbeat when it loses the daemon mid-session.
- **File logger (`src/util/log.{hpp,cpp}`).** Detailed daemon log at `$XDG_STATE_HOME/loginext/daemon.log` (`$HOME/.local/state/loginext/daemon.log` fallback). Levels: Trace / Debug / Info / Warn / Error. Per-event traces moved off stderr — file-only by default.
- **`--quiet` and `--verbose` flags** on the daemon. `--quiet` suppresses stderr (the file sink keeps recording). `--verbose` lowers the file threshold to Trace.
- **Boot/sock paths printed to stderr.** Daemon prints config + socket + log path at start; UI prints socket path + spawn outcome.
- **`deploy/systemd/loginext.service`** — `systemctl --user` template with hardening (`NoNewPrivileges`, `ProtectSystem=strict`, etc.). Optional; the spawn-on-launch path is the default.
- **`deploy/scripts/loginext-logs`** — one-shot tail helper. Subcommands: default → `tail -F`, `--path` → print resolved log path, `--clear` → confirm-then-truncate.
- **Persistent DAEMON ONLINE / DAEMON OFFLINE toggle.** Status bar hosts a single neumorphic button that doubles as the indicator and the kill-switch. Online state breathes a green `box-shadow` pulse via a custom `@keyframes`; offline state holds a static red glow.
- **Heuristic / Action decoupling.** `process_hwheel()` returns `heuristics::Direction { None, Left, Right }`; the new `presets/` module owns the `(PresetId, Direction) → KeyCombo` mapping. NBT (Ctrl+Tab / Ctrl+Shift+Tab) is the first entry. The Low / Medium / High profile constants in `config::Profile` are byte-for-byte unchanged — the "Golden Feel" of NBT is preserved.

### Changed
- **CSS rebuilt as true soft neumorphism.** Surfaces all share the page background; depth is purely shadow-based. Each segmented option is its own raised pill (was previously a single dark-track block) and depresses into the surface when active.
- **Daemon default mode → `Medium`** (was `Low`). The UI defaults match. Eliminates the "low-flashes-then-rerenders" flicker on first paint when no config file exists.
- **Heartbeat is `setTimeout`-chained with exponential backoff.** Success → 5 s next tick; failure ramps 2 s → 4 s → 8 s → 16 s → 30 s. Re-runs the spawn check on the first three failures so a crashed daemon recovers automatically.
- **IPC errors via the structured logger.** All `[loginext] ipc: …` `fprintf` lines in `src/ipc/server.cpp` route through `LX_*` so they obey the same Quiet / Verbose contract as the rest of the daemon.

---

## 2026-04-25 — UI state-sync correctness pass

### Fixed
- **Segmented closure-state bug.** `Segmented` was holding a stale closure copy of the active option. `fetchInitialState()` updated `aria-selected` on the DOM but never touched the closure, so the first click on the already-highlighted segment short-circuited and did nothing. The component now treats the DOM (`aria-selected`) as the single source of truth — same pattern as `toggle.ts`.
- **Redundant write+reload after initial sync.** `applyCurrentSettings` short-circuits when `(mode, invert)` matches the last applied tuple. `fetchInitialState` seeds `lastAppliedMode` / `lastAppliedInvert` from the daemon so the first click only fires an IPC when the value actually changes.

### Changed
- **Card corners softened** (`--radius-card: 28px`); radius scale bumped across the tree.
- **Hover / press / active states** on every interactive widget now use a `box-shadow` accent glow + small `transform` press. No `transition: all`, no layout-thrashing properties — only `color`, `background`, `box-shadow`, `transform`.

---

## 2026-04-19 — Optimisation audit (F1–F15)

Findings and fixes applied. Detailed write-ups archived in [KNOWN_ISSUES.md](./KNOWN_ISSUES.md). Headlines:

### Fixed
- **F1 (critical):** `volatile bool g_stop` → `volatile sig_atomic_t`. Async-signal-safety compliance; prevents a hang under aggressive optimisation.
- **F2 (high):** `process_timer()` now drains the `timerfd` read fully and returns on partial / failed reads. Prevents a 100% CPU spin on a spurious wake.
- **F3 (high):** `emit_passthrough()` and `write_event()` log `write()` failures at Warn level instead of silently dropping events.
- **F4 (medium):** Config reload reads through `open()` + a 4 KiB stack buffer; `std::ifstream` / `stringstream` removed from the reload path.
- **F7:** `<cmath>` → `<cstdlib>` in `scroll_state.cpp` (single `std::abs(int)` call).
- **F8:** `on_reload()` resets `app->scroll = {}` (aggregate init) so new fields can't be silently missed.
- **F9:** Ring-buffer modulo replaced with bitmask + `static_assert` that `max_queued_actions` is a power of 2.
- **F11:** Optional CMake flags `LOGINEXT_LTO` and `LOGINEXT_NATIVE` (both default ON).
- **F12 (medium):** `tap_key_combo()` batches the full key-down + SYN + key-up + SYN sequence into a single `write()` syscall.
- **F14 (medium):** Virtual mouse now registers `EV_MSC` + `MSC_SCAN` so passthrough events for those types reach consumers.
- **F15:** `SYN_DROPPED` filtered out of the passthrough path.

### Deferred (no action — see KNOWN_ISSUES.md)
- F5 (`default_config_path` allocation — only called once at startup, fine).
- F6 (sequential `/dev/input/event*` scan — ~1 ms total, negligible).
- F10 (`check_damping` on every event — ~5 ns, negligible).
- F13 (`Parser` class shape — fine until JSON schema grows).
