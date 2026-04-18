# LogiNext — Progress

A C++ daemon running on Arch/CachyOS that manages Logitech devices in userspace with an Options+-like control surface. First goal: smooth tab switching via the MX Master 3S thumb wheel. Next goal: a UI that makes every button and wheel configurable on a per-application basis.

---

## Phase 1 — Thumb Wheel → Tab Navigation Engine

Completed. Calibrated on real hardware.

### Device I/O
- [x] `/dev/input/*` scanning, automatic MX Master 3S detection by VID/PID
- [x] **Exclusive grab** on device via `libevdev`
- [x] Two virtual devices via `uinput`: virtual keyboard for tabs, virtual mouse for passthrough
- [x] Single thread, `epoll` (edge-triggered) + `timerfd` event loop
- [x] Signals: graceful shutdown via `SIGINT` / `SIGTERM`, config reload via `SIGHUP`

### Heuristic Engine
- [x] **Leaky-bucket accumulator** for `REL_HWHEEL` (`ScrollState`)
- [x] **Velocity-aware dynamic threshold**: lerp between fast/slow threshold based on Δt
- [x] **Idle reset**: silence longer than `idle_reset_ns` → accumulator zeroed, new gesture
- [x] **Leading-edge confirmation window**: the first event of a gesture is not emitted immediately; if a second event in the same direction arrives within `confirmation_window_ns`, it is confirmed → filters out single-event ghosts caused by a resting finger on the wheel
- [x] **Emit cooldown**: minimum emit interval within the same gesture
- [x] **Axis invert** (from config; default `true` for MX Master 3S)

### Output & Pacing
- [x] `Ctrl+Tab` and `Ctrl+Shift+Tab` via `uinput`
- [x] Ring buffer pacing queue + `timerfd` (`pacing_interval_ns`)
- [x] Damping: when input stops, brake events accumulated in the queue (`damping_timeout_ns`)
- [x] All events other than tab-switching are passed through via virtual mouse

### Config Layer
- [x] 3 presets: `low` / `medium` / `high` (`Profile` struct, `constexpr`)
- [x] Flat JSON parser (dependency-free, ~100 lines) → `~/.config/loginext/config.json`
- [x] CLI override: `--mode=low|medium|high`, `--config=<path>`, `--help`
- [x] `SIGHUP` → hot reload, gesture state is reset

### Build & Tooling
- [x] CMake 3.25+ / Ninja, single binary target
- [x] `-O2 -Wall -Wextra -Wpedantic -Werror` clean compilation
- [x] `compile_commands.json` export (IDE integration)

---

## Phase 2 — Configuration UI (next)

Goal: a control panel similar to Logitech Options+. The user:

1. Sees connected Logitech devices (only MX Master 3S for now).
2. Selects a physical control on the device (for Phase 2: **thumb wheel**; other buttons/wheels later).
3. Assigns an **action preset** to that control. First preset: **"Navigate between tabs"** — the engine built in Phase 1 powers this preset.
4. Once a preset is selected, its specific parameters appear below (for tab navigation: sensitivity `low`/`medium`/`high` or continuous slider, invert axis).
5. Applies this rule **globally** or **per-application** (e.g. Firefox only).

### Theme
- **Neumorphism — dark variant**. Adapt the soft emboss/rounding language to a dark palette: `#1e1f24`/`#262830` surface, `rgba(0,0,0,0.55)` inner/outer shadow + `rgba(255,255,255,0.04)` highlight, `#6c7cff` accent. Corners 16–24px, active element "pressed" (inset shadow), passive element "raised" (outer shadow).

### Architectural decision — daemon ↔ UI IPC
- UI is a separate process; does not harm daemon hot path.
- Transport: **Unix domain socket** (`$XDG_RUNTIME_DIR/loginext.sock`). No framework like D-Bus (agents.md rule).
- Protocol: line-delimited JSON. Command set: `list_devices`, `list_controls`, `list_presets`, `get_bindings`, `set_binding`, `get_profile`, `set_profile`, `reload`.
- "Bindings" are added to the daemon's existing `Settings` struct; config JSON schema is extended. After the UI writes to the config file, it tells the daemon to `reload` (SIGHUP infrastructure is ready).

### Tech stack candidates
- **Tauri (Rust shell + web frontend)**: small binary, single runtime, natural neumorphism via CSS. Preferred.
- Alternative: **Qt 6 / QML** (native but heavy), **GTK4 + libadwaita** (GNOME-centric).
- Decision finalized in Phase 2.1; `ui/` subdirectory stays inside the main repo (monorepo).

### Phase 2.1 — Daemon IPC layer
- [ ] `src/ipc/` module: UDS listener, register with epoll, non-blocking accept
- [ ] JSON command dispatch (mini parser from loader is extended)
- [ ] `bindings` concept: `(device_id, control_id) → (preset_id, preset_params, scope)` mapping
- [ ] Config schema v2: backward-compatible, old flat keys are read as fallback
- [ ] Integration test: send commands to socket via CLI client (`loginext-ctl`), parse response

### Phase 2.2 — UI skeleton
- [ ] Tech stack decision and `ui/` bootstrap (Tauri assumed)
- [ ] Neumorphism dark design tokens (CSS variables): surface, shadow, accent, radius, typography
- [ ] Common components: `Card`, `RaisedButton`, `PressedButton`, `Toggle`, `Slider`, `ListItem`
- [ ] UDS client: connect to daemon, heartbeat, reconnection

### Phase 2.3 — Device & Control browser
- [ ] Left column: list of connected devices (MX Master 3S)
- [ ] Middle column: controls of the selected device (thumb wheel is the only one initially; buttons and wheels later)
- [ ] Right column: assigned preset + parameters for the selected control
- [ ] Simple SVG placeholders for device/control visuals

### Phase 2.4 — "Navigate between tabs" preset panel
- [ ] Preset selector dropdown (first entry: Navigate between tabs; others later)
- [ ] Sensitivity selection: 3-segment toggle (Low / Medium / High) + optional continuous slider
- [ ] Invert axis toggle
- [ ] "Live preview": show generated events while UI is open (debug overlay)

### Phase 2.5 — Scope: global vs per-app
- [ ] Rule scope selector: "All applications" | "Only in…"
- [ ] Active window detection: `wlr-foreign-toplevel` or `ext-foreign-toplevel-list` for Wayland, `_NET_ACTIVE_WINDOW` for X11. Compositor support varies — fallback: user selects application manually (executable name / WM_CLASS).
- [ ] Daemon side: binding lookup by active window (cache + invalidation required when adding to hot path)
- [ ] Rule conflict: most specific match wins (per-app > global).

### Phase 2.6 — Polishing
- [ ] Import/export: transfer settings file to another machine
- [ ] Autostart: `~/.config/systemd/user/loginext.service` template (user unit; udev rule may also be needed for uinput permissions)
- [ ] Tray / indicator (optional)

---

## Phase 3 — Other controls (future)

As soon as Phase 2 is complete, the following controls will be integrated into the `bindings` system. Each requires a separate preset family:

- Back / Forward buttons
- Gesture button (under thumb)
- Vertical scroll wheel (SmartShift toggle integration to be investigated)
- Mode-shift button (if device-side change is possible)

Preset candidates: "Window switcher", "Workspace switch", "Volume", "Zoom", "Custom keystroke", "Run command".

---

## Design notes — permanent rules

- No heap allocation on the hot path; still in effect.
- UI only writes to the config file + tells the daemon to `reload`. No direct access to the hot path from the UI.
- Per-app lookup will be on the daemon hot path, so O(1) hash map + subscribe to window changes is required.
- For each new preset: a state under `heuristics/` + an output via `core/emitter`. Everything else comes from `bindings` and `ipc`.
