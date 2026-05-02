# LogiNext — Known issues & deferred audit findings

Long-form context for performance / reliability audit findings that are either already shipped (see [CHANGELOG.md](./CHANGELOG.md)) or deliberately deferred. Active rules sit in [OPTIMIZATIONS.md](./OPTIMIZATIONS.md). Architectural roadmap lives in [progress.md](./progress.md).

The 2026-04-19 audit catalogued 15 findings (F1–F15). F1–F4, F7–F9, F11–F12, F14–F15 shipped. F5, F6, F10, F13 stayed deferred — the rationale is below so future agents don't re-litigate them.

---

## Per-app scope listener quirks (2026-05-03)

- **Hash 0 is reserved as the global sentinel.** `scope::hash_app()` re-rolls into `fnv_prime` if the FNV-1a result happens to land on 0. A different rule store implementation (e.g. cuckoo, robin-hood) must preserve this invariant: the hot-path `lookup()` short-circuits on 0 and an in-table 0 would mark the slot empty. Don't change without coordinated edits to both `app_hash.hpp` and `rules.hpp`.
- **Listener publishes via `memory_order_relaxed`.** The atomic carries an integer, not a pointer; the only happens-before requirement is "eventually visible". A focus change racing with a thumb-wheel emit can produce one event resolved against the previous app's preset — that's an accepted behaviour, not a bug. Do not add stronger orderings unless you can demonstrate a correctness violation.
- **Hyprland backend uses `socket2.sock` (event stream), not `socket.sock` (request channel).** The two sockets have similar names and very different semantics; using the request channel would require active polling, which defeats the whole point of an async listener.
- **X11 backend prefers `WM_CLASS.instance_name` over `class_name`.** Firefox reports `Navigator` as the class but `navigator` as the instance — and Chrome / Chromium follow the same pattern. Instance name is more specific and matches what a user typing `firefox` in `app_rules.txt` actually expects (after lower-casing). Some niche apps publish only the class name; the loop falls back to it automatically.
- **Wayland-other compositors (Sway, KWin, …) are not yet wired.** The thread idles in those sessions and the daemon resolves every event against the global preset. Adding a backend is a new arm in `listener.cpp::thread_main()`; the surrounding infrastructure is already correct.
- **Listener thread holds a libxcb connection for the lifetime of the daemon.** That's intentional — reconnect cost is non-trivial (atom interns, root attribute change-mask) and the X server is the long-lived peer. If `xcb_connection_has_error()` fires the loop exits and the daemon falls back to global-only resolution; restart the daemon to recover.

## Quirks of the `--debug-events` flag (2026-05-02)

- **Dump goes to stderr, not stdout.** Stdout is reserved for the daemon's line-delimited JSON IPC stream; mixing the raw event dump in would corrupt any consumer reading from a pipe. Stderr also matches the rest of the daemon's lifecycle output, so `sudo ./build/loginext --debug-events 2>&1 | grep KEY` works as expected during discovery.
- **Bypasses `LX_*` log levels on purpose.** The dump uses `std::fprintf(stderr, …)` directly so it appears even with `--quiet`, and so it doesn't pollute the structured file log at `$XDG_STATE_HOME/loginext/daemon.log` with Trace-volume entries.
- **Requires SYSTEM OFFLINE in the UI before launch.** The auto-spawned daemon already holds the exclusive `libevdev` grab; a second instance will fail to grab. Click SYSTEM OFFLINE first (the `daemon_forced_off` flag in `localStorage` keeps it dead through any UI restart), run the discovery binary in a terminal, then click SYSTEM ONLINE to hand the device back.
- **The hot-path branch is `__builtin_expect(debug_events, 0)`.** Do not refactor it into a `std::function` callback or a virtual hook — the whole point is that production runs pay one predicted-not-taken byte test and nothing else.

## Deferred findings — *do not "fix" these without reading the rationale*

### F5 — `default_config_path()` allocates `std::string` on every call

- **Severity:** Low.
- **Why deferred:** It is only ever called once, at startup, when the result is stashed into `AppContext::config_path`. The reload path reads the cached field, never re-resolves the path. The two or three transient `std::string` allocations are paid once on a cold startup; rewriting them as a static buffer would buy nothing measurable and complicate the API.
- **What to revisit:** If we ever support multi-profile configs (path resolved per request) or per-user reloading inside an event handler, switch to a fixed-capacity char buffer at that point.

### F6 — `device.cpp` opens every `/dev/input/event*` node sequentially

- **Severity:** Low.
- **Why deferred:** ~1 ms total on a typical desktop (10–20 event nodes). Hot-pluggability isn't a Phase-2 goal; the daemon enumerates once at startup. `libudev` would violate the "no frameworks" rule and pre-filtering via `/proc/bus/input/devices` is the right shape but only worth it once we add live attach/detach.
- **What to revisit:** When Phase 3 introduces hot-plug for additional Logitech devices, switch enumeration to `/proc/bus/input/devices` parsing + per-event-node open only on candidates whose vendor:product matches.

### F10 — `check_damping()` runs on every `REL_HWHEEL` event

- **Severity:** Low.
- **Why deferred:** Cost is two comparisons + one subtraction per event (≈5 ns). When called from the event handler the silence delta is always near zero, so the function returns at the first guard — no work. Moving it to a separate timer would cost more in scheduling complexity than it saves.
- **What to revisit:** Only if profiling ever shows `on_event` itself becoming a hotspot. It currently isn't — the `process_hwheel` heuristics dominate.

### F13 — `Parser` class in `loader.cpp` is the only OOP construct

- **Severity:** Low (style only).
- **Why deferred:** The class is contained in an anonymous namespace, doesn't leak, and is genuinely the right shape for a streaming parser — splitting it into free functions would just spread the cursor `pos_` across a dozen signatures without making anything clearer. The agents.md rule is "no OOP *bloat*"; this is OOP *fit*.
- **What to revisit:** When the IPC schema or config schema gains nesting / numbers / arrays, replace the whole parser with a single-header library (per the agents.md note). At that point delete the class entirely; do not extend it.

---

## Shipped findings — historical detail

Full descriptions retained for posterity. The fix is already in the codebase; the entry is here as breadcrumbs for "why is this written this way?" archaeology.

### F1 — `volatile bool g_stop` was not async-signal-safe (shipped)

The C++ standard does not guarantee that a `volatile bool` written in a signal handler is visible to the main thread without a data race. Only `volatile sig_atomic_t` or `std::atomic<T>` are safe. Under `-O2`+ the loop in `run_loop` (`while (!*stop)`) could theoretically be hoisted into an infinite loop. Fixed by switching to `volatile sig_atomic_t`, matching the existing `g_reload` pattern.

### F2 — `process_timer()` ignored the `timerfd` read result (shipped)

`[[maybe_unused]] auto r = read(...)` silenced the warning but did not handle failure. If `read()` returned `-1` with `EAGAIN` (possible after races) or `0`, the timerfd stayed readable because the expiration counter was never consumed. The level-triggered epoll registration would fire again immediately → busy loop → 100% CPU. Fixed by checking the return value and short-circuiting on partial reads.

### F3 — Silent `write()` failure on the passthrough path (shipped)

`emit_passthrough()` and `write_event()` dropped events on `write()` failure with no diagnostic. uinput buffer-full is rare but the failure mode was a total mouse freeze with nothing in the log. Fixed by routing the error through the structured logger at Warn level (so it lands in the file log without spamming stderr in normal operation).

### F4 — Config reload allocated on the event-loop thread (shipped)

`std::ifstream` + `std::stringstream` + repeated `std::string` operations inside `load_settings()`, called from `on_reload()` on the same thread as the event loop. Reload is rare but the rule is "no heap allocation in the event loop." Replaced with a single `open()` + `read()` into a 4 KiB stack buffer. Config files larger than 4 KiB are now rejected — fine for the flat schema, will be revisited if v2 nests.

### F7 — `<cmath>` for a single `std::abs(int)` (shipped)

`<cmath>` pulls in floating-point math symbols never used in `scroll_state.cpp`. Switched to `<cstdlib>` (where `std::abs(int)` actually lives).

### F8 — `on_reload()` reset `ScrollState` field-by-field (shipped)

Six manual assignments, easy to forget when adding a new field. Replaced with `app->scroll = {}` aggregate init. Future-proof and one line.

### F9 — Ring-buffer used `% max_queued_actions` (shipped)

The compiler *should* optimise `% 8` to `& 7` since `max_queued_actions` is `constexpr int = 8`, but the cast to `uint8_t` may interfere on some toolchains. Replaced with explicit `& (max_queued_actions - 1)` plus `static_assert((max_queued_actions & (max_queued_actions - 1)) == 0)` so the constraint is documented in code.

### F11 — Missing LTO and `-march=native` (shipped)

Added optional CMake flags `LOGINEXT_LTO` (sets `INTERPROCEDURAL_OPTIMIZATION TRUE`) and `LOGINEXT_NATIVE` (`-march=native`), both default ON. Appropriate for a single-machine daemon; non-portable binary is the explicit tradeoff.

### F12 — 4–6 `write()` syscalls per tab switch (shipped)

`tap_key_combo()` was emitting Ctrl-down, Tab-down, SYN, Tab-up, Ctrl-up, SYN as six separate `write()`s. uinput accepts batched writes, so the whole sequence now packs into a single `write()` of an `input_event[8]` array. Saves ~10–20 µs per emission and keeps the kernel from re-entering on each event.

### F14 — Virtual mouse missing `EV_MSC` capability (shipped)

The passthrough path re-emitted `EV_MSC` events but `setup_mouse()` never registered `EV_MSC` + `MSC_SCAN` capabilities, so uinput silently dropped them. The MX Master 3S emits `MSC_SCAN` for button presses; consumers that care (some apps under X11) lost them. Fixed by registering the capability.

### F15 — `SYN_DROPPED` re-emitted on the virtual device (shipped)

`SYN_DROPPED` is libevdev's signal that the kernel buffer overflowed. Re-emitting it on the virtual device was semantically wrong (the virtual device has its own buffer). Now filtered out before passthrough.

---

## Validation strategy (still applicable)

These are the verification recipes for the fixes above. Run them after any change to the hot path or signal handlers.

1. **Signal safety (F1).** Send `SIGINT` to the daemon under a sustained event load (`evemu-record` + `evemu-play`). Verify clean exit within 100 ms, ×100.
2. **Timerfd drain (F2).** Stress the pacer (`enqueue_action` × 100 in a tight loop) and watch CPU usage. CPU must stay < 5 %.
3. **Write batching (F12).** `strace -e write -c loginext` before and after a Low → High → Low cycle. Compare syscall counts per tab switch.
4. **Memory profile (F4).** `valgrind --tool=massif` across 10 reload cycles. Heap must not grow beyond initial allocation.
5. **MSC passthrough (F14).** `evtest` on the virtual mouse device while clicking the MX Master 3S buttons; verify `MSC_SCAN` events appear.
6. **Regression sweep.** Manual scroll on each sensitivity mode after every change to `heuristics/` or `core/`. Verify ghost filtering, cooldown, damping still behave.
