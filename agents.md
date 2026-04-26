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

## Active focus — UI/UX agent

Intent-based daemon state management. The Tauri shell still auto-spawns on launch (lifecycle stays unchanged), but the front-end now owns the sticky **user intent**: a `daemon_forced_off` flag in `localStorage` is set by the SYSTEM OFFLINE click and consumed on every boot to immediately countermand the auto-spawn via `kill_daemon`. The heartbeat respects intent — no auto-respawn while OFF, only a gentle 10 s probe so an externally-started daemon (systemd, CLI) still flips the button back. When adding new lifecycle controls, route them through this same intent → IPC → button-class pipeline; never sneak an auto-spawn around the flag.

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

## Process model (read before touching lifecycle code)

- The user launches the UI. The Tauri shell probes `$XDG_RUNTIME_DIR/loginext.sock`; if dead, it spawns the C++ daemon **detached** (`setsid`, stdio → `/dev/null`).
- Closing the UI does NOT stop the daemon. Reopening reconnects.
- Detailed daemon logs always go to `$XDG_STATE_HOME/loginext/daemon.log`. Stderr is reserved for boot/lifecycle/critical lines so a terminal-launched UI stays readable.
