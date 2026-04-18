# LogiNext — Agent Instructions

## Target
- **OS:** Arch/CachyOS (low-latency kernel, `linux-cachyos` or `linux-zen`)
- **Arch:** x86_64

## Stack
- **Language:** C++20 (GCC 14+ / Clang 18+). Use `constexpr`, `std::chrono`, `std::span` aggressively.
- **Input:** `libevdev` — exclusive grab on physical device node (`/dev/input/eventX`).
- **Output:** `uinput` — virtual keyboard for synthetic key injection.
- **Build:** CMake 3.25+, Ninja generator. Single binary target.

## Core Rules
1. **Latency is law.** Zero heap allocations in the event loop. Pre-allocate everything at init.
2. **No OOP bloat.** Flat structs + free functions. Polymorphism only via `std::variant` or compile-time dispatch.
3. **No frameworks.** No Boost, no Qt, no D-Bus polling. Direct syscalls where it matters (`epoll`, `timerfd`).
4. **Signal density.** Every line of code must justify its existence. Comments explain *why*, never *what*.

## Project Layout
```
src/
├── core/          # Event loop, device grab, uinput emitter
├── heuristics/    # Velocity engine, accumulator, damping logic
├── config/        # Compile-time constants, tuning knobs
└── main.cpp       # Entry point, init, teardown
```

## Device
- **Logitech MX Master 3S** (USB HID / Bolt receiver)
- Thumb wheel emits `REL_HWHEEL` (horizontal relative axis)
