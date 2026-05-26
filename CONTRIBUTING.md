# Contributing to LogiNext

Thanks for your interest in LogiNext. This document covers the practical
bits — coding rules, build expectations, PR checklist. The full design
philosophy lives in [agents.md](./agents.md); architectural decisions
worth not re-litigating live in [KNOWN_ISSUES.md](./KNOWN_ISSUES.md).

## Quick start

```bash
git clone https://github.com/vseprr/loginext.git
cd loginext

# C++ daemon
cmake -S . -B build -G Ninja
cmake --build build

# UI (separate from the daemon — talks to it over a Unix socket)
cd ui
npm install
npm run tauri dev
```

For a one-shot install (Arch / CachyOS): `./deploy/install.sh`.

## Project structure

```
src/
├── core/         # event loop, device grab, uinput emitter, pacer
├── heuristics/   # scroll state + velocity engine
├── presets/      # constexpr (PresetId, Direction) → KeyCombo table
├── scope/        # per-app rule table + active-window listeners
├── config/       # CLI args, JSON config, sensitivity profiles
├── ipc/          # UDS server + dispatch
├── util/         # logger
└── main.cpp
ui/
├── src/          # vanilla TypeScript — components, views, IPC client
└── src-tauri/    # Rust shell — daemon spawn, Tauri commands
tests/            # GoogleTest unit tests (opt-in: -DBUILD_TESTS=ON)
```

## Hard coding rules

These are non-negotiable. PRs that violate them get refactor requests.

1. **Zero heap allocation in the event loop.** All hot-path state lives
   on the stack inside `AppContext` / `ScrollState` / `PacingQueue`. If
   you need to add a new field, put it there — don't introduce a heap
   allocation per event.
2. **No frameworks.** No Boost, Qt, D-Bus client libraries, nlohmann/json,
   spdlog, fmt. Direct syscalls + printf. The ad-hoc parser at
   `src/config/loader.cpp` (~100 lines) covers config; do not replace
   until the schema genuinely demands it.
3. **No OOP bloat.** Flat struct + free function. Polymorphism via
   `std::variant` or compile-time dispatch only.
4. **Builds clean under `-Werror -Wall -Wextra -Wpedantic`.** No warnings
   silenced with pragmas without an inline comment explaining why.
5. **Signal density in comments.** Comments answer *why*, never *what*.
   Code that's self-explanatory gets no comment.
6. **Logging discipline.** Every new log line uses the structured macros
   in `src/util/log.hpp`. Prefer the `_C` variant with a component tag:
   `LX_INFO_C(Scope, "focus changed: %s", name)`. Levels: `LX_TRACE` for
   per-event chatter (file only), `LX_INFO` for lifecycle, `LX_WARN` /
   `LX_ERROR` for the rare failures that should escape to stderr.
7. **UI ↔ daemon contract.** UI writes the config file, then sends
   `reload` over the socket. UI never reaches into the daemon's hot path
   directly.
8. **Feature isolation.** Heuristics ([`src/heuristics/`](src/heuristics/))
   is strictly decoupled from output actions
   ([`src/presets/`](src/presets/), [`src/core/emitter.cpp`](src/core/emitter.cpp)).
   The engine's only output is `heuristics::Direction { None, Left, Right }`.
   New presets are strictly additive — a new arm in the constexpr table.

## Daemon lifecycle (read before touching `ui/src-tauri/`)

The UI is a systemd-only client. The daemon is managed exclusively by
`systemctl --user enable/disable/start/stop loginext.service` — spawn-detach
is a deprecated legacy path that should not be reintroduced. The UI's
`run()` branches on `service::query_state()`:

- Unit not installed → show install wizard, no daemon launch
- Unit active → `wait_for_running` (poll the socket, don't spawn)
- Unit enabled but inactive → `service::start_only()` then `wait_for_running`
- Unit disabled → don't start anything; toggle reads OFF

If you find yourself adding a code path that spawn-detaches the daemon,
stop. Re-read [KNOWN_ISSUES.md § "Two-daemon grab race"](KNOWN_ISSUES.md).

## Per-app rule data model

`<app_id>=<preset>[,<mode>[,<invert>]]`. App ids are case-insensitive;
fields after `preset` are independent. Empty `preset` = tracked-only chip
(UI-visible, daemon ignores). Empty `mode` / `invert` = inherit from
global at lookup time. See `config/app_rules.example.txt`.

## Tests

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

The test target is opt-in (`-DBUILD_TESTS=ON`) so default `cmake -B build`
stays light and doesn't download GoogleTest. New pure-logic modules
should ship with tests under `tests/<module>/`. The heuristics engine
and the per-app rule table are the canonical examples in
`tests/heuristics/` and `tests/scope/`.

## Lint targets

```bash
# C++ daemon
cmake --build build                     # -Werror -Wall -Wextra -Wpedantic
clang-format --dry-run -Werror src/**/*.{hpp,cpp}

# Rust UI shell
cd ui/src-tauri
cargo clippy -- -D warnings

# TypeScript UI frontend
cd ui
npx tsc --noEmit
```

CI runs all four on every PR. Local pre-commit checks are encouraged.

## Pull request checklist

Before opening a PR, please verify:

- [ ] No heap allocation introduced on the daemon hot path
- [ ] `cmake --build build` finishes clean under `-Werror`
- [ ] `cargo clippy -- -D warnings` passes for `ui/src-tauri`
- [ ] `npx tsc --noEmit` passes for `ui/`
- [ ] New pure-logic code has accompanying tests under `tests/`
- [ ] Comments explain *why*, not *what* (signal-density rule)
- [ ] No new frameworks pulled in (see rule 2 above)
- [ ] Log lines use structured macros with appropriate component tags
- [ ] If a new lifecycle code path was added, it routes through
      `service::query_state()` and does NOT spawn-detach the daemon

The PR template at `.github/PULL_REQUEST_TEMPLATE.md` will prompt you
through these.

## Bug reports

The UI ships a bug-report button (🐛 icon in the status bar) that
auto-collects system info + the recent daemon log and formats them as
a copy-pasteable GitHub Issue. Please use it when filing reports — it
saves a triage round-trip.

If filing manually, please include:

- OS / kernel version
- Compositor (KWin / Sway / Hyprland / GNOME / X11 WM name)
- LogiNext version (`Cargo.toml` / `package.json`)
- Service state (`systemctl --user status loginext.service`)
- Device VID/PID (paste from `lsusb`)
- Recent daemon log (`~/.local/state/loginext/daemon.log`, last ~100 lines)

The bug report template at `.github/ISSUE_TEMPLATE/bug_report.yml`
prompts for these.

## License

LogiNext is MIT-licensed. By contributing you agree your contributions
are licensed under the same.
