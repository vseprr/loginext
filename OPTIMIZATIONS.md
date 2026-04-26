# LogiNext — Active Performance Rules

The standing performance discipline for this codebase. Bug-fix archaeology lives in [CHANGELOG.md](./CHANGELOG.md); deferred audit findings + historical write-ups live in [KNOWN_ISSUES.md](./KNOWN_ISSUES.md).

If you are about to add code on the hot path or touch the daemon's signal / IPC surface, every rule below is non-negotiable.

---

## Hot-path rules

1. **Zero heap allocation in the event loop.** No `new`, no `malloc`, no `std::string` operations, no STL containers that may grow. All hot-path state is fixed-capacity, on the stack, owned by `AppContext` / `ScrollState` / `PacingQueue` / `PacingQueue::ring`.
2. **No virtual dispatch on the hot path.** Polymorphism, when needed, is `std::variant` or compile-time dispatch.
3. **Single-thread.** The event loop is the only consumer of the device fd, the timerfd, and the IPC sockets. Any background work (e.g. future Wayland active-window subscription) must arrive on a fd registered with the same `epoll`.
4. **Bounded I/O.** Every `read()` / `write()` checks the return value. Partial reads on the timerfd short-circuit. UDS writes are best-effort; SIGPIPE is ignored at startup so a dead UI client cannot kill the daemon mid-write.
5. **Async-signal-safety in handlers.** Signal handlers only flip `volatile sig_atomic_t` flags (`g_stop`, `g_reload`). Everything else happens on the next loop iteration in the main thread.
6. **No SA_RESTART.** `epoll_wait` is allowed to return `EINTR` so the loop re-checks `g_stop` / `g_reload` immediately on signal delivery.

## Reload path (the documented exception)

Reload is *not* a hot path in steady state — it fires once per user-initiated config change. The rule we accept here:

- File read uses `open()` + `read()` into a 4 KiB stack buffer. No `ifstream` / `stringstream`.
- Parse uses the in-tree flat parser (`config/loader.cpp`). No JSON library.
- Reload runs on the event-loop thread; the deferred-ack pattern in `dispatch.cpp::handle_reload` defers the IPC ack until *after* the new settings are live, so the UI can rely on success meaning success.
- Gesture state is reset (`app->scroll = {}`) — never patched field-by-field.

## IPC surface

- One UDS listener fd + up to `max_clients` short-lived client fds, all owned by `IpcServer`.
- Per-client fixed `client_recv_buf` — no resize, never grows. Lines longer than the buffer drop the client.
- Listener and client fds are level-triggered (simpler framing). The device fd is edge-triggered (correct for high-frequency input). Don't mix the two on the same fd type.
- Response buffer is a 1 KiB stack array per request. UDS writes to a local process never short-write at this size — confirmed in practice; do not add a write-loop for "robustness" without evidence.

## Build flags

- `-O2 -Wall -Wextra -Wpedantic -Werror` — clean build is enforced.
- `LOGINEXT_LTO` (default ON) — `INTERPROCEDURAL_OPTIMIZATION TRUE`. Allows the compiler to inline across TUs (e.g. `write_event` → `syn` → `emit_tab_next`).
- `LOGINEXT_NATIVE` (default ON) — `-march=native`. The binary is non-portable; this is intentional for a single-machine daemon. Disable for distribution builds.

## UI ↔ daemon contract

- The UI process is allowed any frameworks it wants — its hot path is webview rendering, not input.
- The UI may **never** reach into the daemon's internal state. Settings changes go: `write_config` (Tauri command writes the JSON) → `request("reload")` (line over UDS) → daemon answers when the new settings are live.
- Requests are one round-trip each over a per-request `UnixStream`. Connection pooling was deliberately rejected — the lifecycle bugs of long-lived sockets weren't worth the negligible reconnect cost over a UDS to a local peer.
- Heartbeat from the UI uses exponential backoff (2 s → 30 s on failure, 5 s on success). The first three failures also fire `daemon_respawn` so a crashed daemon recovers automatically without requiring the user to do anything.

## Logging

- Every daemon log line goes through `src/util/log.hpp`. Direct `fprintf(stderr, …)` is reserved for the brief boot banner only.
- `LX_TRACE` — file sink only. Per-event traces, every emit / every state transition. Cheap because the macro short-circuits before the format runs when the level is below threshold.
- `LX_DEBUG` — file sink, recoverable noise (IPC bring-up, accept failures the daemon recovered from).
- `LX_INFO` — both sinks. Lifecycle: config loaded, IPC listening, shutting down.
- `LX_WARN` / `LX_ERROR` — both sinks; the only thing that escapes to stderr by default.

## Verification cadence

Run before merging anything that touches the hot path or signal / IPC surface:

1. `cmake --build build` — must finish under `-Werror` with no warnings.
2. Manual scroll on Low / Medium / High — verify ghost filtering, cooldown, damping unchanged.
3. `strace -e write -c ./build/loginext` over a Low → High → Low cycle — exactly one write burst per `write_config` plus one per reload ack.
4. `valgrind --tool=massif` over 10 SIGHUP reload cycles — heap must not grow beyond the initial allocation.
5. `perf stat -e context-switches -p <daemon-pid>` during a scroll storm with the UI open — context switch rate must be indistinguishable from the UI-closed baseline.
