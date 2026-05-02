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

**Persistent daemon toggle + advanced UI status feedback (2026-04-26).** Status bar now hosts a single SYSTEM ONLINE / SYSTEM OFFLINE neumorphic button that doubles as the indicator and the kill-switch. Click flips a `daemon_forced_off` flag in `localStorage`; intent survives UI restarts so a user-stopped daemon stays stopped after a relaunch (the Tauri shell's auto-spawn is countermanded by an immediate `kill_daemon` IPC on first paint). Online state breathes a green `box-shadow` pulse via a custom `@keyframes`; offline state holds a static red glow; both keep the dual-shadow neumorphic depth, with `transform: scale(0.98)` + debossed inset on press. New `kill_daemon` Tauri command walks `/proc` to locate `loginext`, sends SIGTERM, polls the socket for clean shutdown, and escalates to SIGKILL only on a wedge.

---

## In progress — Phase 2.4 / 2.5

### Phase 2.4 — Tab-nav preset polish
- [x] Second preset entry — `Zoom` (`Ctrl+=` / `Ctrl+-`) added alongside NBT in `presets/preset.hpp`. `list_presets` IPC now returns both; the UI dropdown auto-populates.
- [ ] Preset selector dropdown UI work — daemon contract (`list_presets`, `set_preset`) is already in place; this is now purely a vanilla-TS task in `ui/`.
- [ ] Live preview overlay: stream emitted Tab events back to the UI while the window is open (multi-message IPC channel — currently every request is one round-trip).

### Phase 2.5 — Device profiles for new MX Master 3S buttons (next up)
- [ ] Use `--debug-events` to capture raw signatures for: Gesture button (under thumb), Back, Forward, Mode-shift, vertical wheel click. Record the (`type`, `code`, `value`) sequences and pin them to a doc note for reproducibility.
- [ ] Codify the mapping as a constexpr "MX Master 3S" device profile (`presets/mx_master_3s.hpp` or extend `presets/preset.hpp`) — additive only, must not retune NBT or touch `config::Profile`.
- [ ] Extend `heuristics/` with per-button state where needed (e.g. Gesture-button + REL_X/REL_Y → directional gesture); the engine still emits only abstract directions, the preset table does the action lookup.
- [ ] Wire each new control through `ipc/list_controls` so the UI can populate them without further daemon changes.

### Phase 2.5 — Scope: global vs per-app
- [ ] Rule-scope selector: "All applications" | "Only in…" (UI work — daemon side already wired; the sidecar `app_rules.txt` is the contract until the UI editor lands).
- [x] Active window detection — Hyprland (`HYPRLAND_INSTANCE_SIGNATURE` → `/tmp/hypr/$HIS/.socket2.sock`) and X11 (`_NET_ACTIVE_WINDOW` via libxcb + xcb-icccm). Wayland-other compositors fall through to the no-op idle backend (global preset only); add per-compositor backends as needed.
- [x] Daemon-side binding lookup keyed on active window. Push-based: a dedicated `pthread` listener publishes the focused app's FNV-1a hash into `std::atomic<uint32_t>` on every focus change; the hot path does one relaxed load + one masked open-addressing probe.
- [x] Conflict resolution: most specific match wins. `scope::lookup()` returns false on hash==0 or on miss, and `on_event` falls back to `settings.active_preset` in that case.

### Phase 2.6 — Polishing
- [x] Easy installation — one-shot `deploy/install.sh` for Arch/CachyOS: pacman deps, release build of daemon + UI, binaries to `~/.local/bin`, systemd user unit staged (`--enable-service` opt-in).
- [x] Desktop integration — `.desktop` entry in `~/.local/share/applications/` plus icon at `~/.local/share/icons/loginext.png`; LogiNext is searchable from the application menu with its icon.
- [x] Persistent Daemon Toggle — status-bar button is a sticky kill-switch backed by `localStorage`; the user's last explicit ON/OFF intent survives UI restarts and overrides the auto-spawn path.
- [x] Advanced UI Status Feedback — custom-keyframed neumorphic status button: green breathing pulse (online), static red glow (offline), debossed press feedback. No CSS framework presets.
- [x] Native Arch PKGBUILD — `deploy/PKGBUILD` is a `-git` recipe; `pkgver()` resolves at build time from `git describe`. `cd deploy && makepkg -si` builds + installs (or upgrades) over the previous version after a `git pull`. Distribution-portable (`-DLOGINEXT_NATIVE=OFF`), pulls in `libxcb` + `xcb-util-wm` for the active-window listener.
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
