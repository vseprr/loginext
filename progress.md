# LogiNext — Progress

A C++ daemon that gives Logitech mice on Linux the per-control customisation Logitech Options+ offers on Windows / macOS. First milestone shipped: gesture-aware tab switching on the MX Master 3S thumb wheel. Current focus: a polished UI and production-grade lifecycle.

For implementation rules see [agents.md](./agents.md). For active performance discipline see [OPTIMIZATIONS.md](./OPTIMIZATIONS.md). For shipped fixes see [CHANGELOG.md](./CHANGELOG.md).

---

## Completed (Phase 1 + 2 highlights)

**Phase 1 — Thumb-wheel engine.** MX Master 3S auto-detection (`046d:b034` / `046d:c548`), exclusive `libevdev` grab, virtual keyboard + mouse via `uinput`, single-thread `epoll` + `timerfd` loop, `SIGINT`/`SIGTERM` shutdown, `SIGHUP` config reload. Heuristics: leaky-bucket accumulator, velocity-aware threshold (fast/slow Δt lerp), idle reset, leading-edge confirmation window, emit cooldown, axis invert, ring-buffer pacing queue with damping. Three sensitivity profiles (`low` / `medium` / `high`) — `constexpr Profile`. Flat dependency-free JSON parser, CLI overrides (`--mode`, `--invert`, `--config`). Single binary, `-O2 -Werror` clean.

**Phase 2.1 — Daemon IPC.** UDS listener at `$XDG_RUNTIME_DIR/loginext.sock` registered with the same epoll, line-delimited JSON dispatch (`ping`, `get_settings`, `list_devices`, `list_controls`, `list_presets`, `reload`). `reload` is deferred-ack: the daemon only responds after the config is actually live.

**Phase 2.2 — UI skeleton.** Tauri 2.x + vanilla TypeScript + Vite. Neumorphism dark design tokens with reusable `Card` / `Toggle` / `Slider` / `Segmented` / `ListItem` primitives. Per-request `UnixStream` from the Rust shell.

**Phase 2.3 — Device + control browser.** Three-column Options+-style layout populated from `list_devices` / `list_controls` / `list_presets`.

**Phase 2.4 (partial) — Tab-nav preset panel.** Sensitivity segmented control + invert-axis toggle, both DOM-source-of-truth with redundant-apply guards. Initial-state sync bug from 2026-04-25 fixed (see [CHANGELOG.md](./CHANGELOG.md)).

**Phase 2.4 — Heuristic / Action decoupling (2026-04-26).** The heuristic engine no longer references action types: `process_hwheel()` now returns `heuristics::Direction { None, Left, Right }` and has zero knowledge of key combos, presets, or output devices. A new `presets/` module owns the `(PresetId, Direction) → KeyCombo` mapping; NBT (Ctrl+Tab / Ctrl+Shift+Tab) is the first entry. The pacer queues resolved `KeyCombo`s, and the emitter writes them via a generic two-phase combo path. Switching the active preset is O(1) (constexpr table lookup, single switch in `presets::preset_for()`); `Settings::active_preset` is the runtime knob and travels through the existing config/SIGHUP reload path. The Low / Medium / High profile constants in `config::Profile` are byte-for-byte unchanged — `evemu-record` timings on each mode match the pre-refactor capture.

**Production lifecycle (2026-04-26).** Detached daemon spawn from the Tauri shell (`setsid`, stdio → `/dev/null`); UI close no longer kills the daemon, reopening silently reconnects. Backoff-aware heartbeat with auto-respawn. File logger writes to `$XDG_STATE_HOME/loginext/daemon.log`; stderr is now lifecycle-only. CSS rebuilt as true soft neumorphism — segmented options are individually-raised pills that deboss into the surface when active. Daemon default mode flipped to `Medium` so the UI's first paint matches reality. Systemd user-unit template + `loginext-logs` tail helper landed under [deploy/](./deploy).

**Phase 2.4 — Zoom preset (2026-05-03).** Second preset entry alongside NBT: `Ctrl+=` (right tick → zoom in) / `Ctrl+-` (left tick → zoom out). Strictly additive — a new arm in `presets/preset.hpp::preset_for()`, a new constexpr `preset_zoom`, and a new branch in `preset_id_str` / `preset_name` / `preset_id_from_str`. NBT timings, the heuristic engine, the pacer, and the emitter were not touched. The IPC `list_presets` handler iterates `preset_count` so the UI dropdown auto-populates with no surface change. No new emitter mode either: zoom uses the universal `Ctrl+=` / `Ctrl+-` keystroke that browsers, IDEs, GTK apps, and image viewers all bind to zoom.

**Phase 2.5 — Per-app vs global scope (2026-05-03).** New `src/scope/` module: a fixed-capacity (64-slot, power-of-two, open-addressed) flat hash table mapping FNV-1a app-id hashes to `presets::PresetId`, plus a `pthread` background listener that publishes the focused window's hash into a single `std::atomic<uint32_t>`. Hot path adds exactly one relaxed atomic load + one masked array probe; on hash==0 (no specific app) or miss, the global preset wins (most-specific-match-first semantics with global as fallback). Listener picks its backend at startup: Hyprland IPC (`/tmp/hypr/$HIS/.socket2.sock` event stream) if `HYPRLAND_INSTANCE_SIGNATURE` is set, libxcb + `_NET_ACTIVE_WINDOW` PropertyNotify if `DISPLAY` is set, otherwise idle (graceful no-op). Per-app rules live in a sidecar text file at `$XDG_CONFIG_HOME/loginext/app_rules.txt` (one `app=preset_id` per line) — keeps the existing flat JSON parser untouched per agents.md rule 2. Reload happens on the same `SIGHUP` as the main config. Strict performance discipline: zero `std::string` comparisons on the hot path, zero dynamic allocation, no STL containers, no thread synchronisation primitives heavier than a relaxed atomic.

**Phase 2.6 — Native Arch PKGBUILD (2026-05-03).** `deploy/PKGBUILD` builds the project as a `loginext-git` VCS package: `pkgver()` resolves at build time from `git describe` (with a commit-count + short-hash fallback for an untagged trunk), so a `git pull` followed by `makepkg -si` cleanly upgrades the installed copy via pacman. Builds both halves (C++ daemon + Tauri UI), installs to `/usr/bin`, drops the desktop entry + icon under `/usr/share`, stages the systemd user unit at `/usr/lib/systemd/user/loginext.service`. Distribution-friendly: `-DLOGINEXT_NATIVE=OFF` so the package is portable across x86_64 microarchitectures.

**Hardware discovery / `--debug-events` flag (2026-05-02).** New CLI flag dumps every raw `input_event` drained from `libevdev` to stderr (`type` + name, `code` + name, `value`) for mapping unknown buttons (Gesture, Back/Forward, Mode-shift) on the MX Master 3S. Lives entirely in the C++ daemon — UI is untouched. The check inside the drain loop is `__builtin_expect(debug_events, 0)`, so the production path (flag off) takes one predicted-not-taken branch and no fprintf. Workflow: SYSTEM OFFLINE in the UI → `sudo ./build/loginext --debug-events` in a terminal → press buttons → Ctrl+C → SYSTEM ONLINE. Stderr-only on purpose so the dump never collides with the line-delimited JSON IPC stream on stdout.

**Persistent daemon toggle + advanced UI status feedback (2026-04-26).** Status bar now hosts a single DAEMON ONLINE / DAEMON OFFLINE neumorphic button that doubles as the indicator and the kill-switch. Click flips a `daemon_forced_off` flag in `localStorage`; intent survives UI restarts so a user-stopped daemon stays stopped after a relaunch (the Tauri shell's auto-spawn is countermanded by an immediate `kill_daemon` IPC on first paint). Online state breathes a green `box-shadow` pulse via a custom `@keyframes`; offline state holds a static red glow; both keep the dual-shadow neumorphic depth, with `transform: scale(0.98)` + debossed inset on press. New `kill_daemon` Tauri command walks `/proc` to locate `loginext`, sends SIGTERM, polls the socket for clean shutdown, and escalates to SIGKILL only on a wedge.

**Phase 2.7 — Context-aware preset management (2026-05-03).** Replaced the static preset list and the separate "Defined rules" section with a unified context-aware workflow. Clicking the Thumb Wheel control now reveals a context selector bar (Global + per-app icons) below it. The "Available Presets" list dynamically highlights whichever preset is active for the selected context. Click-to-bind sets a preset; click-to-deselect removes the binding (for per-app: deletes the rule; for global: sets `active_preset` to `"none"` which makes the daemon passthrough HWHEEL events as raw input). New `PresetId::None` in `preset.hpp` — when effective, the daemon's `on_event()` short-circuits before heuristics and forwards HWHEEL to the virtual mouse. The preset resolution was moved earlier in `on_event()` (before `tick_leak`) so the passthrough branch never touches heuristic state. Fixed the root-cause bug where `write_config()` in `ipc_bridge.rs` never persisted `active_preset` — the global preset was permanently stuck on the compile-time default (`tab_nav`). Now `write_config()` accepts and writes `active_preset` into `config.json`; the daemon's existing `config/loader.cpp` parser already handles the key. Status toggle labels renamed from SYSTEM ONLINE/OFFLINE to DAEMON ONLINE/OFFLINE. The right-column preset header dynamically updates to reflect the active preset for the current context.

**Phase 2.7.4 — KWin script auto-enable + 30s no-events diagnostic (2026-05-03).** The 2.7.3 udev path made the daemon connect to the user session bus cleanly, but a fresh `makepkg -si` left `loginext-focusEnabled` unset in the user's `kwinrc` (pacman post-hooks run as root and cannot reach `~/.config`). Symptom: KWin DBus listener bound on the daemon side but received zero `Activated()` events, so `active_app_hash` stayed at 0 and per-app rules silently fell back to the global preset on every event. Two fixes converged on it:
  - Tauri shell `ensure_kwin_script_enabled()` (in `ui/src-tauri/src/lib.rs`) runs `kwriteconfig6 → kwriteconfig5` then `qdbus6 → qdbus-qt6 → qdbus org.kde.KWin /KWin reconfigure` on every UI launch. Gated on `XDG_CURRENT_DESKTOP` containing `KDE`, idempotent, no error on missing tools / dead KWin. Removes the one remaining manual step from the package install path.
  - Daemon-side `kwin_dbus_loop` 30s diagnostic: `Listener::kwin_received_any` flips on the first `Activated()` callback; the loop's select() timeout is clamped so the warning fires even on an otherwise-idle bus. Message names the exact `kwriteconfig6 + qdbus6` recovery commands so a power-user inspection of the log gives them the fix in one line. UI's "Currently focused" row also refines the empty-name copy to "(unknown — enable LogiNext Focus Bridge in System Settings → KWin Scripts)" when `source === "kwin-dbus"`, so the same hint is visible in three places (daemon log, UI body, post-install hint).

**Phase 2.7.3 — udev rules + drop sudo path (2026-05-03).** Plasma 6's session-bus broker rejects EXTERNAL auth from uid 0 outright (`EPIPE` before the name claim), so the 2.7.2 root-side bus bridge was a dead-end no matter how cleverly we resolved the address. Switched the supported runtime model to "daemon runs as the seat user" and shipped [deploy/udev/99-loginext.rules](./deploy/udev/99-loginext.rules): grants `TAG+="uaccess"` ACL access on `/dev/uinput` + the MX Master 3S event nodes (`046d:b034` Bolt, `046d:c548` USB) plus `GROUP="input" MODE="0660"` as a non-logind fallback. New `deploy/loginext-git.install` post-hook in the PKGBUILD runs `udevadm control --reload-rules && udevadm trigger --subsystem-match=input --subsystem-match=misc` after install/upgrade so rules apply without replug. `deploy/install.sh` mirrors that with a sudo-prompted `install` to `/etc/udev/rules.d/`. `kwin_dbus_loop` reverted to a single-line `sd_bus_open_user()` — the SUDO_UID/EUID branch + `/run/user/*` scan from 2.7.2 are gone, replaced by a single warning that points the user back to the udev rules. The IPC `quit` lifecycle from 2.7.1 stays — useful even in the all-user flow because it's uid-agnostic by design.

**Phase 2.7.2 — sudo session-bus + WebKit Wayland (2026-05-03, superseded by 2.7.3 on the daemon side).** Follow-up on 2.7.1 after `sudo ./build/loginext --verbose` still landed on the X11 backend on Plasma 6 Wayland. The 2.7.1 fallback only fired *after* `sd_bus_open_user()` returned an error, but at uid 0 that call frequently returns success while pointing at a useless `/run/user/0/bus` (root's empty session) or an unauthenticated handle — neither produces a usable connection to the human session, but neither fails fast enough for the fallback to kick in. The new `kwin_dbus_loop` checks `geteuid()` first: when running as root *and* `resolve_session_bus_address` can produce an explicit path, `sd_bus_open_user` is skipped entirely and the daemon connects via `sd_bus_new` + `sd_bus_set_address` + `sd_bus_set_bus_client` + `sd_bus_start`. Diagnostic strings were also bumped from `LX_DEBUG` to `LX_INFO`/`LX_WARN` so the path the listener took is visible without `--verbose`. `resolve_session_bus_address` now considers SUDO_UID before `DBUS_SESSION_BUS_ADDRESS` when EUID==0 (the latter is sudo-replaceable, the former is not), and ends with a bounded `/run/user/*` directory scan as a last-resort hint for `su -c` style launches that strip SUDO_UID.

WebKitGTK Wayland Error 71 fix landed in the Tauri shell: `apply_webkit_wayland_workarounds()` runs at the very top of `run()` and sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` (always on Linux) and `WEBKIT_DISABLE_COMPOSITING_MODE=1` (only on Wayland) — both conditionally, so user-set values are never clobbered. The fix had to live on the Rust side rather than just the `.desktop` file: terminal-launched test runs (`./ui/src-tauri/target/release/loginext-ui` from the repo) bypass the launcher entirely. README documents the `GDK_BACKEND=x11` escape hatch for the residual Nvidia / Mesa-mismatch cases the WebKit env vars can't catch.

**Phase 2.7.1 — Cross-uid lifecycle + KWin DBus rescue + responsive overflow (2026-05-03).** Three convergent fixes for issues that surface together when the daemon is launched as a different uid than the UI session (typically `sudo loginext` for raw `/dev/input` access).
- **IPC `quit` command (cooperative shutdown).** New `quit` handler in `ipc/dispatch.cpp` flips `g_stop` exactly the way SIGTERM would, replying `{"ok":true,"state":"stopping"}` before the loop unwinds. `DispatchCtx` carries a new `stop_flag` pointer so the handler doesn't need to import the daemon's signal globals. The Tauri `kill_daemon` (Rust side) tries this round-trip first; signal-based kill is now a fallback for a wedged daemon. The bug it cures: `pid=NNN SIGKILL: Operation not permitted (os error 1)` when the user clicked DAEMON OFFLINE — kill(2) EPERMs across uid boundaries, but the listener UDS is already chowned to the invoking user during init, so the IPC path always works.
- **KWin D-Bus session-bus fallback.** When `sd_bus_open_user()` fails (the typical `sudo` symptom — `DBUS_SESSION_BUS_ADDRESS` is stripped from the env), `kwin_dbus_loop` now resolves `unix:path=/run/user/$SUDO_UID/bus` (or the env var, then `XDG_RUNTIME_DIR/bus`) and reconnects via `sd_bus_new` + `sd_bus_set_address` + `sd_bus_set_bus_client` + `sd_bus_start`. The KWin script (`deploy/kwin/loginext-focus`) was already publishing `Activated(resourceClass, resourceName)` correctly; the daemon just couldn't claim the well-known name on the right bus. After the fix the per-app rule UI sees the focused window even when the daemon is owned by root.
- **UI overflow plumbing for small windows.** `body { overflow: auto }` (was `hidden`), `.app-shell` uses `min-height: 100%` instead of a hard `height`. New `@media (max-height: 620px)` and the existing `<900px` width breakpoint relax `.app__col` from `overflow-y: auto` to `visible` so document-level scroll takes over instead of stranding controls inside columns that out-scrolled the viewport. Document scrollbars use the same `--neu-light` palette as the per-column ones — visual identity is preserved at every size.

---

## In progress — Phase 2.4 / 2.5

### Phase 2.4 — Preset polish
- [x] Second preset entry — `Zoom` (`Ctrl+=` / `Ctrl+-`) added alongside NBT in `presets/preset.hpp`. `list_presets` IPC now returns both; the UI dropdown auto-populates.
- [x] Preset selector — context-aware click-to-bind/unbind in the "Available Presets" list. Global context sets `active_preset` in `config.json`; per-app context writes `app_rules.txt`.
- [x] `PresetId::None` — passthrough sentinel; when bound, HWHEEL events bypass heuristics and forward as raw input.
- [ ] Live preview overlay: stream emitted Tab events back to the UI while the window is open (multi-message IPC channel — currently every request is one round-trip).

### Phase 2.5 — Device profiles for new MX Master 3S buttons (next up)
- [ ] Use `--debug-events` to capture raw signatures for: Gesture button (under thumb), Back, Forward, Mode-shift, vertical wheel click. Record the (`type`, `code`, `value`) sequences and pin them to a doc note for reproducibility.
- [ ] Codify the mapping as a constexpr "MX Master 3S" device profile (`presets/mx_master_3s.hpp` or extend `presets/preset.hpp`) — additive only, must not retune NBT or touch `config::Profile`.
- [ ] Extend `heuristics/` with per-button state where needed (e.g. Gesture-button + REL_X/REL_Y → directional gesture); the engine still emits only abstract directions, the preset table does the action lookup.
- [ ] Wire each new control through `ipc/list_controls` so the UI can populate them without further daemon changes.

### Phase 2.5 — Scope: global vs per-app
- [x] Rule-scope selector — context-aware selector with Global + per-app buttons appears below the Thumb Wheel. Click-to-bind/unbind replaces the old "Defined rules" list.
- [x] Active window detection — Hyprland (`HYPRLAND_INSTANCE_SIGNATURE` → `/tmp/hypr/$HIS/.socket2.sock`) and X11 (`_NET_ACTIVE_WINDOW` via libxcb + xcb-icccm). Wayland-other compositors fall through to the no-op idle backend (global preset only); add per-compositor backends as needed.
- [x] Daemon-side binding lookup keyed on active window. Push-based: a dedicated `pthread` listener publishes the focused app's FNV-1a hash into `std::atomic<uint32_t>` on every focus change; the hot path does one relaxed load + one masked open-addressing probe.
- [x] Conflict resolution: most specific match wins. `scope::lookup()` returns false on hash==0 or on miss, and `on_event` falls back to `settings.active_preset` in that case.

### Phase 2.7 — Context-aware UI (completed)
- [x] Dynamic context selector below Thumb Wheel (Global + per-app icons).
- [x] Context-aware preset list with click-to-bind / click-to-deselect.
- [x] Active preset header dynamically updates per context.
- [x] Removed obsolete "Defined rules" list — unbinding is handled by deselecting in the preset list.
- [x] Fixed global preset persistence bug (`write_config` now writes `active_preset`).
- [x] DAEMON ONLINE / DAEMON OFFLINE toggle labels.

---

## Phase 3 — Other controls (future)

Once Phase 2 is done, these controls join the same `bindings` system. Each needs its own preset family.

- Back / Forward buttons.
- Gesture button (under thumb).
- Vertical scroll wheel — investigate SmartShift toggle integration.
- Mode-shift button (if the device permits a side-channel change).

Preset candidates for the new families: Window switcher, Workspace switch, Volume, Zoom, Custom keystroke, Run command.

---

## Permanent design notes

- No heap allocation on the hot path. Reload path is a documented exception (4 KiB stack buffer + flat parser; see [OPTIMIZATIONS.md](./OPTIMIZATIONS.md)).
- UI writes the config file + sends `reload`. Never touches the hot path directly.
- Per-app lookup will live on the hot path → O(1) hash + window-change subscription is mandatory before that lands.
- New presets follow the pattern: state under `heuristics/`, output via `core/emitter`. Wire-up belongs in `bindings` + `ipc`.
- UI components that own state treat the DOM attribute (`aria-selected`, `aria-checked`) as the source of truth. Closure-captured state is a known trap (`segmented.ts`, `toggle.ts` already converted).
