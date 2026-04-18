# LogiNext — Agent Instructions

This file specifies the rules that AI agents working on this repo must follow. Refer to [progress.md](./progress.md) for project scope and roadmap, and [README.md](./README.md) for user-facing description.

## Target

- **OS:** Arch / CachyOS (low-latency kernel — `linux-cachyos` or `linux-zen`). Likely to work on other distros, but this is the primary target.
- **Arch:** x86_64.
- **Display server:** Both X11 and Wayland (active window detection on both is targeted for Phase 2.5).

## Stack

- **Language:** C++20 (GCC 14+ / Clang 18+). Prefer `constexpr`, `std::chrono`, designated initializers, `std::span`.
- **Input:** `libevdev` — exclusive grab on physical device node (`/dev/input/eventX`).
- **Output:** `uinput` — virtual keyboard (Ctrl+Tab etc.) + virtual mouse (passthrough).
- **Build:** CMake 3.25+, Ninja. Single binary target (`loginext`).
- **UI (Phase 2):** Likely Tauri (Rust + web). Decision will be made in Phase 2.1; does not add a new language dependency to the main daemon — UI is a separate process.

## Core Rules

1. **Latency is law.** No heap allocation in the event loop. Everything is allocated at init time. All state on the hot path is carried on the stack (`AppContext`, `ScrollState`, `PacingQueue`).
2. **No OOP bloat.** Flat struct + free function. Polymorphism only via `std::variant` or compile-time dispatch.
3. **No frameworks.** No Boost, Qt, D-Bus, nlohmann/json. Direct syscalls (`epoll`, `timerfd`, `uinput`). ~100-line ad-hoc parser for JSON (see [src/config/loader.cpp](./src/config/loader.cpp)). A single-header parser will be considered if the schema grows; until then, no.
4. **Signal density.** Every line must justify its existence. Comments say *why*, not *what*.
5. **Config live-reload.** Every new parameter that determines runtime behavior must be added to `config::Profile` or `config::Settings` and must be reloadable via `SIGHUP`.

## Project Layout

```
src/
├── core/           # Event loop, device grab, uinput emitter, pacer
│   ├── device.{hpp,cpp}
│   ├── emitter.{hpp,cpp}
│   ├── event_loop.{hpp,cpp}
│   └── pacer.{hpp,cpp}
├── heuristics/     # Scroll engine state + transitions
│   └── scroll_state.{hpp,cpp}
├── config/         # Constants, profiles, CLI args, JSON loader
│   ├── constants.hpp
│   ├── profile.hpp
│   ├── settings.hpp
│   ├── args.{hpp,cpp}
│   └── loader.{hpp,cpp}
└── main.cpp        # Entry, init, teardown, signal & callback wiring

config/example.json # Example user config
```

Phase 2 adds new directories: `src/ipc/` (UDS server), `ui/` (frontend).

## Device

- **Logitech MX Master 3S** — Bolt receiver (`046d:b034`) or USB wired (`046d:c548`).
- Thumb wheel → `REL_HWHEEL` (horizontal relative axis).
- Event mapping for other controls (Phase 3) will be explored separately.

## Conventions

- Header + source file pairs (no header-only unless trivial).
- Namespace: `loginext::{core, heuristics, config, ipc}`.
- `[[nodiscard]]` — on all return values the caller should not ignore.
- `noexcept` — default on hot path functions.
- CLI/config flag names `snake_case`, JSON keys `snake_case`.
- Commit style: short imperative title + "why" in body if needed.
