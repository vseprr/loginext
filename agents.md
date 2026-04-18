# LogiNext — Agent Instructions

Bu dosya, repo üzerinde çalışacak AI ajanlarının uyması gereken kuralları belirtir. Proje kapsamı ve yol haritası için [progress.md](./progress.md), kullanıcıya dönük açıklama için [README.md](./README.md) referans alınmalıdır.

## Target

- **OS:** Arch / CachyOS (düşük gecikmeli çekirdek — `linux-cachyos` ya da `linux-zen`). Diğer dağıtımlarda çalışması muhtemel ama birincil hedef bu.
- **Arch:** x86_64.
- **Display server:** Hem X11 hem Wayland (Phase 2.5 için her ikisinde de aktif pencere tespiti hedeflenir).

## Stack

- **Dil:** C++20 (GCC 14+ / Clang 18+). `constexpr`, `std::chrono`, designated initializer'lar, `std::span` tercih edilir.
- **Input:** `libevdev` — fiziksel cihaz düğümünde (`/dev/input/eventX`) exclusive grab.
- **Output:** `uinput` — sanal klavye (Ctrl+Tab vb.) + sanal fare (passthrough).
- **Build:** CMake 3.25+, Ninja. Tek binary target (`loginext`).
- **UI (Phase 2):** muhtemelen Tauri (Rust + web). Karar Phase 2.1'de verilir; ana daemon'a yeni dil bağımlılığı eklemez — UI ayrı process.

## Core Rules

1. **Latency is law.** Event loop'unda heap allocation yasak. Her şey init sırasında ayrılır. Sıcak yoldaki tüm state stack üzerinde taşınır (`AppContext`, `ScrollState`, `PacingQueue`).
2. **No OOP bloat.** Flat struct + free function. Polimorfizm yalnızca `std::variant` veya compile-time dispatch ile.
3. **No frameworks.** Boost, Qt, D-Bus, nlohmann/json yok. Doğrudan syscall (`epoll`, `timerfd`, `uinput`). JSON için ~100 satırlık ad-hoc parser (bkz. [src/config/loader.cpp](./src/config/loader.cpp)). Şema büyürse tek-başlık bir parser değerlendirilir, o zamana kadar hayır.
4. **Signal density.** Her satır varlığını ispatlamalı. Yorumlar *why* söyler, *what* söylemez.
5. **Config live-reload.** Runtime davranışını belirleyen her yeni parametre `config::Profile` veya `config::Settings`'e eklenir ve `SIGHUP` ile yeniden yüklenebilir olmalıdır.

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

config/example.json # Örnek kullanıcı config'i
```

Phase 2 yeni dizinler ekler: `src/ipc/` (UDS sunucusu), `ui/` (frontend).

## Device

- **Logitech MX Master 3S** — Bolt receiver (`046d:b034`) veya USB kablolu (`046d:c548`).
- Thumb wheel → `REL_HWHEEL` (yatay relative axis).
- Diğer kontroller (Phase 3) için event mapping ayrıca keşfedilecek.

## Conventions

- Header + kaynak dosya ikilisi (no header-only unless trivial).
- Namespace: `loginext::{core, heuristics, config, ipc}`.
- `[[nodiscard]]` — çağıranın ignore etmemesi gereken tüm dönüşlere.
- `noexcept` — hot path fonksiyonlarında default.
- CLI/config flag isimleri `snake_case`, JSON key'leri `snake_case`.
- Commit stili: kısa imperative başlık + gerekiyorsa gövdede "why".
