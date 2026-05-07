# LogiNext — Agent Rulebook

Concise rules for AI agents touching this repo. For project scope see [progress.md](./progress.md); for the user-facing description see [README.md](./README.md); for architectural performance rules see [OPTIMIZATIONS.md](./OPTIMIZATIONS.md).

## Target

- **OS:** Arch / CachyOS, low-latency kernel (`linux-cachyos` / `linux-zen`). Other distros likely work but are not the target.
- **Arch:** x86_64. **Display server:** X11 + Wayland.

## Stack

- **Daemon:** C++20 (GCC 14+ / Clang 18+), `libevdev`, raw `epoll` + `timerfd` + `uinput`. CMake 3.25 + Ninja. Single binary `loginext`.
- **UI:** Tauri 2.x (Rust shell + vanilla TypeScript + Vite). Talks to the daemon over a Unix domain socket; never touches the daemon's hot path.

## Hard rules

1. **Latency is law.** Zero heap allocation in the event loop. All hot-path state lives on the stack inside `AppContext` / `ScrollState` / `PacingQueue`.
2. **No frameworks.** No Boost, Qt, D-Bus, nlohmann/json. Direct syscalls. The ad-hoc parser at [src/config/loader.cpp](./src/config/loader.cpp) (~100 lines) covers config; do not replace until the schema demands it.
3. **No OOP bloat.** Flat struct + free function. Polymorphism via `std::variant` or compile-time dispatch only.
4. **Signal density.** Every line earns its keep. Comments answer *why*, never *what*.
5. **Config live-reload.** Every new runtime parameter goes into `config::Profile` or `config::Settings` and must reload via `SIGHUP`.
6. **UI ↔ daemon contract.** UI writes the config file then sends `reload` over the socket. UI never reaches into the hot path directly.
7. **Logging discipline.** Daemon log goes through [src/util/log.hpp](./src/util/log.hpp). `LX_TRACE` for per-event chatter (file only), `LX_INFO` for lifecycle, `LX_WARN`/`LX_ERROR` for the rare failures that should escape to stderr.
8. **Feature isolation & modularity.** The Heuristics Engine ([src/heuristics/](./src/heuristics/)) is strictly decoupled from Output Actions ([src/presets/](./src/presets/), [src/core/emitter.cpp](./src/core/emitter.cpp)). The engine's only output is `heuristics::Direction { None, Left, Right }`; it must never reference key codes, mouse buttons, command strings, or any preset id. All future features follow this same split: heuristic state goes under `heuristics/`, the `(PresetId, Direction) → Action` mapping goes under `presets/` (constexpr table, O(1) dispatch), the emitter under `core/` consumes only resolved actions. Adding a new preset must be strictly additive — a new arm in the table, never a touch on the existing NBT entry, the heuristic, the pacer, or the profile constants. **The Low / Medium / High profile constants in [src/config/profile.hpp](./src/config/profile.hpp) are the "Golden Feel" of NBT and are non-negotiable; do not flatten, merge, or retune them when adding a new preset family.**

## Development workflow

- **Hardware discovery is a CLI concern.** Use the `--debug-events` flag (`sudo ./build/loginext --debug-events`) to discover raw `(type, code, value)` signals from new buttons or controls. Run with the UI in SYSTEM OFFLINE so the auto-spawned daemon doesn't fight for the device grab; flip back to SYSTEM ONLINE when done. Never add discovery / event-dumping logic to the UI, the IPC dispatch, or the production hot path — the cost has to stay paid only when an operator explicitly asks for it.

## Active focus — UI/UX agent

Intent-based daemon state management. The Tauri shell still auto-spawns on launch (lifecycle stays unchanged), but the front-end now owns the sticky **user intent**: a `daemon_forced_off` flag in `localStorage` is set by the DAEMON OFFLINE click and consumed on every boot to immediately countermand the auto-spawn via `kill_daemon`. The heartbeat respects intent — no auto-respawn while OFF, only a gentle 10 s probe so an externally-started daemon (systemd, CLI) still flips the button back. When adding new lifecycle controls, route them through this same intent → IPC → button-class pipeline; never sneak an auto-spawn around the flag.

**Cross-uid lifecycle.** Stopping the daemon goes through IPC `quit` *before* `kill(2)`. The reason: when the daemon is launched as a different uid than the UI (e.g. `sudo loginext`), kill(2) returns EPERM, but the listener UDS was already chowned to the invoking user during init — the IPC round-trip works regardless of process ownership. If you add another lifecycle command (suspend, force-reload, etc.) prefer the same IPC-first / signal-fallback pattern; kill(2) should be reserved for genuinely wedged daemons that ignored their cooperative-shutdown ack.

**Context-aware preset management.** The UI tracks a `currentContext: { type: "global" } | { type: "app", app: string }`. The context selector bar (below the Thumb Wheel) lets the user switch between the global scope and per-app scopes. The "Available Presets" list dynamically highlights whichever preset is active for the selected context. Click-to-bind sets a preset; click-to-deselect removes the binding. For the global context, deselection sets `active_preset` to `"none"` in `config.json` — the daemon interprets this as `PresetId::None` and forwards HWHEEL events as raw passthrough (no heuristics, no key combos). For per-app contexts, deselection deletes the rule from `app_rules.txt` and the app icon disappears from the context bar. The old "Defined rules" list is removed — all rule management flows through the preset list.

## Layout

```
src/
├── core/        # event loop, device grab, uinput emitter, pacer
├── heuristics/  # scroll engine state + transitions
├── config/      # constants, profile, args, JSON loader
├── ipc/         # UDS server, dispatch, line-delimited JSON
├── util/        # logger
└── main.cpp
ui/
├── src/         # vanilla TS — components, views, ipc client
└── src-tauri/   # Rust shell — daemon spawn (daemon.rs), IPC bridge
deploy/
├── systemd/     # user-unit template
└── scripts/     # loginext-logs (tail helper)
```

## Conventions

- Header + source pairs (no header-only except trivial).
- Namespace: `loginext::{core, heuristics, config, ipc, util}`.
- `[[nodiscard]]` on return values the caller must not drop.
- `noexcept` default on hot-path functions.
- CLI / config / JSON keys: `snake_case`.
- Commit style: short imperative title; "why" goes in the body when non-obvious.

## Device

- **Logitech MX Master 3S** — Bolt receiver `046d:b034` or USB `046d:c548`.
- Thumb wheel → `REL_HWHEEL`. Other controls land in Phase 3.

## Runtime privilege model

The daemon **runs as the seat user, never as root**. Both the PKGBUILD (`deploy/loginext-git.install` post-hook) and `deploy/install.sh` install [deploy/udev/99-loginext.rules](./deploy/udev/99-loginext.rules) which grants `TAG+="uaccess"` (logind ACL) + `GROUP="input"` access to `/dev/uinput` and the MX Master 3S event nodes. Trying to run via `sudo` is a non-supported path: the user session bus broker rejects EXTERNAL auth from uid 0 with EPIPE, so the KWin focus bridge cannot bind and per-app rules silently degrade. When adding a new device-id to the supported set, extend the udev rules in lockstep — a daemon that requires sudo for new hardware is a regression.

## Process model (read before touching lifecycle code)

- The user launches the UI. The Tauri shell probes `$XDG_RUNTIME_DIR/loginext/loginext.sock` (under the systemd-managed `RuntimeDirectory=loginext` subdir, mode 0700). If the socket isn't alive it spawns the C++ daemon **detached** (`setsid`, stdio → `/dev/null`).
- v1.0.0 default: `loginext.service` is enabled by `install.sh` and brought up by systemd at every login. The UI's spawn-detached path is the fallback for hosts that opted out via `--no-enable`.
- Closing the UI does NOT stop the daemon. Reopening reconnects.
- Detailed daemon logs always go to `$XDG_STATE_HOME/loginext/daemon.log`. Stderr is reserved for boot/lifecycle/critical lines so a terminal-launched UI stays readable.

## v1.0.0 retrospective — hurdles worth not re-introducing

Phase 2's stabilisation cycle (the fixes that closed v1.0.0) hit four classes of bug repeatedly. Future agents touching the listener / unit / IPC paths should keep these in mind.

1. **systemd-user / Plasma session-env race.** `systemctl --user import-environment XDG_CURRENT_DESKTOP WAYLAND_DISPLAY DISPLAY …` runs *during* Plasma startup, after `default.target` has already activated `loginext.service`. `getenv()` reads a snapshot of `environ` at exec time and never refreshes — so any code path that gates a backend on those vars permanently misses every backend on cold boot. **Rule:** for compositor / session detection at daemon start, use direct probes that depend only on `XDG_RUNTIME_DIR` (which PAM exports before systemd-user). The current implementation is `probe_kwin_on_bus` (sd-bus `NameHasOwner`), `probe_wayland_socket` (`stat()` of `$XDG_RUNTIME_DIR/wayland-{0..3}`), and `probe_x11_display` (`xcb_connect` retry across `:0` / `:1`); see [src/scope/listener.cpp](./src/scope/listener.cpp) `thread_main`.

2. **systemd unit-file mount-namespace ordering.** `ProtectHome=read-only` covers `/run/user/<uid>`; without `ReadWritePaths=` or `RuntimeDirectory=`, the daemon's namespace-builder bails on the missing socket file BEFORE exec runs (`status=226/NAMESPACE`). v1.0.0 uses `RuntimeDirectory=loginext` which creates `$XDG_RUNTIME_DIR/loginext/` with mode 0700 before the daemon starts and tears it down on stop. **Rule:** when adding a new file the daemon needs to write under `/run/user/<uid>` or `$HOME`, prefer `RuntimeDirectory=` / `StateDirectory=` over `ReadWritePaths=` — the former survives crashes cleanly, the latter requires the `-` optional-path prefix or you risk reintroducing 226/NAMESPACE.

3. **Diagnostic deadlines must terminate.** `kwin_dbus_loop`'s 30-second "no events received" warning was ALSO used as the `select()` deadline. When events arrived inside the window (the healthy case), the warning condition `!l->kwin_received_any` never fired → `warned_no_kwin_events` stayed `false` → the deadline kept being computed even after it was in the past → `select(timeout=0)` spun at ~720 000 iterations/sec. **Rule:** any timer that's also a select() deadline must have a terminator on every exit branch, not just the one that fires the timer. The fix was to also set the flag when `l->kwin_received_any` becomes true.

4. **IPC socket bring-up vs. device-grab ordering.** UI clients have a 5-second timeout on the socket-existence probe. If the daemon's blocking init (find_device → grab → emitter → pacer → epoll) runs first and the IPC server initialises last, a slow udev enumeration leaves the socket missing past the UI's deadline. **Rule:** UDS bind happens before any blocking init that could exceed a few hundred ms. Epoll registration of the listener fd can be deferred to after the loop exists — the kernel queues client connects on the listen backlog and they're accepted as soon as the event loop starts.

5. **Tauri bundle targets.** `"targets": "all"` invokes the AppImage bundler, which validates icons as 512 × 512 square. We ship a single non-square icon and don't actually distribute via AppImage (deb is what `install.sh` and `PKGBUILD` consume). **Rule:** keep `bundle.targets` to the formats actually shipped — adding a new bundler is opt-in, not the default.

### Active diagnostic surface

- `--debug-events` — raw `(type, code, value)` libevdev dump for hardware discovery.
- `--debug-perf` — per-second wakeup / event / sd_bus_process counters from both threads. Run for 60 s and read the `perf[main]` / `perf[listener]` lines; a CPU spinner is whichever loop reports thousands of iterations per second.
- `journalctl --user -u loginext.service -b 0` — the systemd-managed lifecycle.
- `~/.local/state/loginext/daemon.log` — the detailed file log (LX_INFO + LX_DEBUG that `--quiet` suppresses from journald).
