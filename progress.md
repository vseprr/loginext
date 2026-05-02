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

**Hardware discovery / `--debug-events` flag (2026-05-02).** New CLI flag dumps every raw `input_event` drained from `libevdev` to stderr (`type` + name, `code` + name, `value`) for mapping unknown buttons (Gesture, Back/Forward, Mode-shift) on the MX Master 3S. Lives entirely in the C++ daemon — UI is untouched. The check inside the drain loop is `__builtin_expect(debug_events, 0)`, so the production path (flag off) takes one predicted-not-taken branch and no fprintf. Workflow: SYSTEM OFFLINE in the UI → `sudo ./build/loginext --debug-events` in a terminal → press buttons → Ctrl+C → SYSTEM ONLINE. Stderr-only on purpose so the dump never collides with the line-delimited JSON IPC stream on stdout.

**Persistent daemon toggle + advanced UI status feedback (2026-04-26).** Status bar now hosts a single SYSTEM ONLINE / SYSTEM OFFLINE neumorphic button that doubles as the indicator and the kill-switch. Click flips a `daemon_forced_off` flag in `localStorage`; intent survives UI restarts so a user-stopped daemon stays stopped after a relaunch (the Tauri shell's auto-spawn is countermanded by an immediate `kill_daemon` IPC on first paint). Online state breathes a green `box-shadow` pulse via a custom `@keyframes`; offline state holds a static red glow; both keep the dual-shadow neumorphic depth, with `transform: scale(0.98)` + debossed inset on press. New `kill_daemon` Tauri command walks `/proc` to locate `loginext`, sends SIGTERM, polls the socket for clean shutdown, and escalates to SIGKILL only on a wedge.

---

## In progress — Phase 2.4 / 2.5

### Phase 2.4 — Tab-nav preset polish
- [ ] Preset selector dropdown (Navigate-between-tabs is currently the only entry; needs a chooser when other presets land — daemon side is ready, see `presets/preset.hpp` + `Settings::active_preset`).
- [ ] Live preview overlay: stream emitted Tab events back to the UI while the window is open (multi-message IPC channel — currently every request is one round-trip).

### Phase 2.5 — Device profiles for new MX Master 3S buttons (next up)
- [ ] Use `--debug-events` to capture raw signatures for: Gesture button (under thumb), Back, Forward, Mode-shift, vertical wheel click. Record the (`type`, `code`, `value`) sequences and pin them to a doc note for reproducibility.
- [ ] Codify the mapping as a constexpr "MX Master 3S" device profile (`presets/mx_master_3s.hpp` or extend `presets/preset.hpp`) — additive only, must not retune NBT or touch `config::Profile`.
- [ ] Extend `heuristics/` with per-button state where needed (e.g. Gesture-button + REL_X/REL_Y → directional gesture); the engine still emits only abstract directions, the preset table does the action lookup.
- [ ] Wire each new control through `ipc/list_controls` so the UI can populate them without further daemon changes.

### Phase 2.5 — Scope: global vs per-app
- [ ] Rule-scope selector: "All applications" | "Only in…".
- [ ] Active window detection — `wlr-foreign-toplevel` / `ext-foreign-toplevel-list` on Wayland, `_NET_ACTIVE_WINDOW` on X11. Compositor support varies; fall back to manual app picker (executable name / `WM_CLASS`).
- [ ] Daemon-side binding lookup keyed on active window. Hot path → must be O(1) hash map + push-based invalidation, never a poll.
- [ ] Conflict resolution: most specific match wins (per-app > global).

### Phase 2.6 — Polishing
- [x] Easy installation — one-shot `deploy/install.sh` for Arch/CachyOS: pacman deps, release build of daemon + UI, binaries to `~/.local/bin`, systemd user unit staged (`--enable-service` opt-in).
- [x] Desktop integration — `.desktop` entry in `~/.local/share/applications/` plus icon at `~/.local/share/icons/loginext.png`; LogiNext is searchable from the application menu with its icon.
- [x] Persistent Daemon Toggle — status-bar button is a sticky kill-switch backed by `localStorage`; the user's last explicit ON/OFF intent survives UI restarts and overrides the auto-spawn path.
- [x] Advanced UI Status Feedback — custom-keyframed neumorphic status button: green breathing pulse (online), static red glow (offline), debossed press feedback. No CSS framework presets.
- [ ] Settings import/export (JSON over file + UI button).
- [ ] Tray indicator (optional — gated on whether it costs anything in the cold path).
- [ ] Bindings-schema v2 with backward-compatible read of the v1 flat keys.

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
