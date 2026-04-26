# LogiNext — Changelog

User-visible changes and resolved issues, newest first. Internal performance discipline lives in [OPTIMIZATIONS.md](./OPTIMIZATIONS.md). Roadmap lives in [progress.md](./progress.md).

---

## 2026-04-26 — Production lifecycle, real neumorphism, structured logging

### Added
- **Detached daemon spawn from the UI.** Launching the Tauri shell probes `$XDG_RUNTIME_DIR/loginext.sock`; if the socket is dead it spawns the C++ daemon as a fully detached background process (`setsid`, stdio → `/dev/null`). Closing the UI no longer kills the daemon; reopening reconnects to the existing socket.
- **Daemon binary discovery.** Honours `$LOGINEXT_DAEMON`, then probes `../../build/loginext` relative to the UI exe (dev workflow), then `$PATH`, then `/usr/local/bin/loginext` and `/usr/bin/loginext`.
- **`daemon_status` / `daemon_respawn` Tauri commands.** UI surfaces the spawn outcome on first paint (`already_running`, `spawned`, `binary_not_found`, `spawn_failed`, `timeout`) and re-runs the spawn check from the heartbeat when it loses the daemon mid-session.
- **File logger (`src/util/log.{hpp,cpp}`).** Detailed daemon log at `$XDG_STATE_HOME/loginext/daemon.log` (`$HOME/.local/state/loginext/daemon.log` fallback). Levels: Trace / Debug / Info / Warn / Error. Per-event traces moved off stderr — file-only by default.
- **`--quiet` and `--verbose` flags** on the daemon. `--quiet` suppresses stderr (the file sink keeps recording). `--verbose` lowers the file threshold to Trace.
- **Boot/sock paths printed to stderr.** Daemon prints config + socket + log path at start; UI prints socket path + spawn outcome.
- **`deploy/systemd/loginext.service`** — `systemctl --user` template with hardening (`NoNewPrivileges`, `ProtectSystem=strict`, etc.). Optional; the spawn-on-launch path is the default.
- **`deploy/scripts/loginext-logs`** — one-shot tail helper. Subcommands: default → `tail -F`, `--path` → print resolved log path, `--clear` → confirm-then-truncate.

### Changed
- **CSS rebuilt as true soft neumorphism.** Surfaces all share the page background; depth is purely shadow-based. Each segmented option is its own raised pill (was previously a single dark-track block) and depresses into the surface when active. Cards and pills got softer corners (`--radius-card: 32px`, `--radius-pill: 999px`) and the dual-shadow pair was retuned for `#1e1f24`.
- **Daemon default mode → `Medium`** (was `Low`). The UI defaults match. Eliminates the "low-flashes-then-rerenders" flicker on first paint when no config file exists.
- **Heartbeat is `setTimeout`-chained with exponential backoff.** Success → 5 s next tick; failure ramps 2 s → 4 s → 8 s → 16 s → 30 s. Re-runs the spawn check on the first three failures so a crashed daemon recovers automatically.
- **Initial paint of the segmented + toggle is unselected.** `value` deliberately omitted; `fetchInitialState()` highlights the right entry once the daemon answers.
- **IPC errors via the structured logger.** All `[loginext] ipc: …` `fprintf` lines in `src/ipc/server.cpp` route through `LX_*` so they obey the same Quiet / Verbose contract as the rest of the daemon.

### Documentation
- `agents.md` collapsed into a tight rulebook.
- `progress.md` summarised completed phases into a single paragraph; only active and future work is now itemised.
- `OPTIMIZATIONS.md` trimmed to active performance rules; F1–F15 audit entries archived to [KNOWN_ISSUES.md](./KNOWN_ISSUES.md).
- `README.md` rewritten in English; new architecture diagram, troubleshooting / diagnostics section.

---

## 2026-04-25 — UI state-sync correctness pass

### Fixed
- **Segmented closure-state bug.** `Segmented` was holding a stale closure copy of the active option. `fetchInitialState()` updated `aria-selected` on the DOM but never touched the closure, so the first click on the already-highlighted segment short-circuited and did nothing. The component now treats the DOM (`aria-selected`) as the single source of truth — same pattern as `toggle.ts`.
- **Redundant write+reload after initial sync.** `applyCurrentSettings` short-circuits when `(mode, invert)` matches the last applied tuple. `fetchInitialState` seeds `lastAppliedMode` / `lastAppliedInvert` from the daemon so the first click only fires an IPC when the value actually changes.

### Changed
- **Card corners softened** (`--radius-card: 28px`); radius scale bumped across the tree.
- **Hover / press / active states** on every interactive widget now use a `box-shadow` accent glow + small `transform` press. No `transition: all`, no layout-thrashing properties — only `color`, `background`, `box-shadow`, `transform`.

---

## 2026-04-19 — Optimisation audit (F1–F15)

Findings and fixes applied. Detailed write-ups archived in [KNOWN_ISSUES.md](./KNOWN_ISSUES.md). Headlines:

### Fixed
- **F1 (critical):** `volatile bool g_stop` → `volatile sig_atomic_t`. Async-signal-safety compliance; prevents a hang under aggressive optimisation.
- **F2 (high):** `process_timer()` now drains the `timerfd` read fully and returns on partial / failed reads. Prevents a 100% CPU spin on a spurious wake.
- **F3 (high):** `emit_passthrough()` and `write_event()` log `write()` failures at Warn level instead of silently dropping events.
- **F4 (medium):** Config reload reads through `open()` + a 4 KiB stack buffer; `std::ifstream` / `stringstream` removed from the reload path.
- **F7:** `<cmath>` → `<cstdlib>` in `scroll_state.cpp` (single `std::abs(int)` call).
- **F8:** `on_reload()` resets `app->scroll = {}` (aggregate init) so new fields can't be silently missed.
- **F9:** Ring-buffer modulo replaced with bitmask + `static_assert` that `max_queued_actions` is a power of 2.
- **F11:** Optional CMake flags `LOGINEXT_LTO` and `LOGINEXT_NATIVE` (both default ON).
- **F12 (medium):** `tap_key_combo()` batches the full key-down + SYN + key-up + SYN sequence into a single `write()` syscall.
- **F14 (medium):** Virtual mouse now registers `EV_MSC` + `MSC_SCAN` so passthrough events for those types reach consumers.
- **F15:** `SYN_DROPPED` filtered out of the passthrough path.

### Deferred (no action — see KNOWN_ISSUES.md)
- F5 (`default_config_path` allocation — only called once at startup, fine).
- F6 (sequential `/dev/input/event*` scan — ~1 ms total, negligible).
- F10 (`check_damping` on every event — ~5 ns, negligible).
- F13 (`Parser` class shape — fine until JSON schema grows).
