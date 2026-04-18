# LogiNext — Optimization Audit

**Date:** 2026-04-19  
**Scope:** Full codebase (`src/`, `CMakeLists.txt`, `config/`)  
**Status:** ✅ All actionable findings (F1–F4, F7–F9, F11–F12, F14–F15) applied and verified. F5, F6, F10, F13 deferred (no action needed).  
**Code health:** Good overall — the hot path is lean, zero-heap-allocation discipline is well enforced, and the architecture is clean. The issues found are mostly reliability, signal-safety, and marginal I/O improvements rather than fundamental design flaws.

---

## 1) Optimization Summary

- **Current health:** The project is well-engineered for its stated goals. The hot path (`on_event` → `process_hwheel` → `enqueue_action`) is tight: no heap, no virtual dispatch, no blocking I/O. The main risks are subtle correctness/reliability issues, not gross inefficiency.
- **Top 3 highest-impact improvements:**
  1. **Signal safety of `g_stop`** — `volatile bool` is not async-signal-safe; must be `volatile sig_atomic_t` or `std::atomic<bool>` with relaxed ordering.
  2. **`timerfd` not drained in `process_timer()`** — ignoring the read result can leave the timer permanently armed, causing a busy-loop on repeated `EPOLLIN` wakeups.
  3. **Heap allocation during SIGHUP reload** — `load_settings()` uses `std::ifstream` + `std::stringstream` on the hot thread; while reload is infrequent, it can stall the event loop for the duration of the I/O + allocation.
- **Biggest risk if no changes are made:** The `volatile bool g_stop` and the un-drained timerfd are latent correctness bugs that can manifest as hangs or CPU spin under specific timing conditions.

---

## 2) Findings (Prioritized)

### F1 — `volatile bool g_stop` Is Not Async-Signal-Safe

- **Category:** Reliability / Concurrency
- **Severity:** Critical
- **Impact:** Correctness — undefined behavior per C/C++ standard; may fail to terminate on some compilers/optimizations.
- **Evidence:** [main.cpp:18](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/main.cpp#L18) — `volatile bool g_stop = false;`
- **Why it's inefficient:** The C++ standard does not guarantee that a `volatile bool` written in a signal handler is visible to the main thread without a data race. Only `volatile sig_atomic_t` or `std::atomic<T>` with relaxed ordering are safe here. On aggressive compilers (`-O2`+), the loop in `run_loop` (`while (!*stop)`) may be hoisted or optimized into an infinite loop if the compiler proves `stop` is never written on the visible code path.
- **Recommended fix:**
  ```cpp
  volatile sig_atomic_t g_stop = 0;
  // In signal handler: g_stop = 1;
  // In run_loop: while (!*stop) { ... }
  ```
  Alternatively `std::atomic<bool>` with `memory_order_relaxed`, but `sig_atomic_t` matches the existing `g_reload` pattern and avoids the atomic include.
- **Tradeoffs / Risks:** None — strictly safer.
- **Expected impact estimate:** Prevents a rare but real hang on certain compiler/arch combos.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`main.cpp`)

---

### F2 — `process_timer()` Ignores `timerfd` Read Result

- **Category:** Reliability / I/O
- **Severity:** High
- **Impact:** CPU spin — if `read()` fails or returns partial data, the timerfd stays readable, causing `epoll_wait` to wake up immediately and repeatedly.
- **Evidence:** [pacer.cpp:59](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/pacer.cpp#L59)
  ```cpp
  [[maybe_unused]] auto r = read(q.timer_fd, &expirations, sizeof(expirations));
  ```
- **Why it's inefficient:** `[[maybe_unused]]` silences the warning but doesn't handle the failure. If `read()` returns `-1` with `EAGAIN` (shouldn't happen since epoll woke us, but possible after races) or `0`, the timerfd remains readable because the expiration counter was not consumed. The level-triggered epoll registration ([event_loop.cpp:37](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/event_loop.cpp#L37)) will fire again immediately → busy loop → 100% CPU.
- **Recommended fix:**
  ```cpp
  uint64_t expirations = 0;
  ssize_t r = read(q.timer_fd, &expirations, sizeof(expirations));
  if (r != sizeof(expirations)) return; // spurious wake or error — do nothing
  ```
- **Tradeoffs / Risks:** None — this is purely defensive.
- **Expected impact estimate:** Prevents a theoretical 100% CPU spin.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`pacer.cpp`)

---

### F3 — `emit_passthrough()` Ignores `write()` Failure

- **Category:** Reliability / I/O
- **Severity:** High
- **Impact:** Silent data loss — if the uinput fd buffer is full or fd becomes invalid, mouse events are silently dropped with no recovery or logging.
- **Evidence:** [emitter.cpp:163](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/emitter.cpp#L163)
  ```cpp
  [[maybe_unused]] auto r = write(em.mouse_fd, &ev, sizeof(ev));
  ```
  Same pattern at [emitter.cpp:22](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/emitter.cpp#L22) in `write_event()`.
- **Why it's inefficient:** On the hot path, a failed `write()` means the user's mouse input is dropped. No retry, no error log, no recovery. While uinput buffer full is rare, the failure mode is total mouse freeze with no diagnostic.
- **Recommended fix:** At minimum, add a debug-level `fprintf` on failure. For production robustness, consider a retry-once on `EAGAIN`:
  ```cpp
  void emit_passthrough(EmitterHandle& em, const struct input_event& ev) noexcept {
      ssize_t r = write(em.mouse_fd, &ev, sizeof(ev));
      if (r < 0 && errno != EAGAIN) {
          std::fprintf(stderr, "[loginext] passthrough write failed: %s\n", std::strerror(errno));
      }
  }
  ```
- **Tradeoffs / Risks:** Adds a branch on the hot path, but the branch is almost never taken. The `fprintf` would only fire on actual errors.
- **Expected impact estimate:** Turns a silent freeze into a diagnosable failure.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`emitter.cpp`)

---

### F4 — Config Reload Performs Heap Allocation on the Event-Loop Thread

- **Category:** Memory / Latency
- **Severity:** Medium
- **Impact:** Latency spike during SIGHUP — `std::ifstream`, `std::stringstream`, `std::string::operator+` all allocate.
- **Evidence:** [loader.cpp:130-154](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/config/loader.cpp#L130-L154) — `load_settings()` is called from `on_reload()` on the event-loop thread.
- **Why it's inefficient:** `agents.md` rule 1 says "no heap allocation in the event loop." While reload is rare (user-initiated SIGHUP), it runs synchronously on the same thread as the event loop. During the file I/O + parsing, input events queue up in the kernel buffer. For a small config file this latency is negligible (~100μs), but it violates the stated design rule.
- **Recommended fix (option A — pragmatic):** Document the exception: "reload is not on the hot path; heap is acceptable here." This is already the reality.
- **Recommended fix (option B — strict):** Read the file using POSIX `open()`/`read()` into a stack-allocated `char[4096]` buffer (config is tiny). Eliminates `ifstream` and `stringstream`:
  ```cpp
  bool load_settings(const std::string& path, Settings& s) noexcept {
      int fd = open(path.c_str(), O_RDONLY);
      if (fd < 0) return false;
      char buf[4096];
      ssize_t n = read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if (n <= 0) return false;
      buf[n] = '\0';
      // parse from buf...
  }
  ```
- **Tradeoffs / Risks:** Option B limits config file size to 4 KiB, which is more than enough for the flat schema. Slightly less portable but this is already linux-only code.
- **Expected impact estimate:** Eliminates ~3-5 heap allocations per reload. Low practical impact since reload is rare.
- **Removal Safety:** Needs Verification (if switching to stack buffer, edge case: config > 4 KiB)
- **Reuse Scope:** local file (`loader.cpp`)

---

### F5 — `default_config_path()` Allocates on Every Call

- **Category:** Memory
- **Severity:** Low
- **Impact:** 2-3 small `std::string` allocations each time the function is called.
- **Evidence:** [loader.cpp:13-21](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/config/loader.cpp#L13-L21) — `std::string(xdg) + "/loginext/config.json"`.
- **Why it's inefficient:** Called at startup and on each SIGHUP. The path doesn't change at runtime. Could be computed once and cached.
- **Recommended fix:** Compute at startup, store in `AppContext::config_path` (which is already done in `main.cpp`). The SIGHUP path in `on_reload()` already reads from `app->config_path`. So `default_config_path()` is only called once at startup — this is already fine in practice.
- **Tradeoffs / Risks:** None.
- **Expected impact estimate:** Negligible — only called once at startup.
- **Removal Safety:** Safe (no change needed)
- **Reuse Scope:** N/A

---

### F6 — `device.cpp` Opens Every `/dev/input/event*` Node Sequentially

- **Category:** I/O
- **Severity:** Low
- **Impact:** Startup latency — on systems with many input devices, the linear scan opens/closes each device node sequentially.
- **Evidence:** [device.cpp:34-81](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/device.cpp#L34-L81) — `find_device()` iterates all `eventN` entries.
- **Why it's inefficient:** Each `open()` + `libevdev_new_from_fd()` is a syscall pair. On a typical desktop with 10-20 event nodes, this costs ~1-2ms total. Not a bottleneck, but could be improved.
- **Recommended fix:** Not urgent. If needed later (Phase 2+ with dynamic device add/remove), consider using `libudev` or reading `/proc/bus/input/devices` to pre-filter candidates by vendor/product before opening.
- **Tradeoffs / Risks:** Adding `libudev` violates the "no frameworks" rule. Pre-filtering from `/proc` is acceptable.
- **Expected impact estimate:** ~1ms reduction in startup. Negligible.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`device.cpp`)

---

### F7 — `scroll_state.cpp` Includes `<cmath>` for a Single `std::abs()` Call

- **Category:** Build / Code Reuse
- **Severity:** Low
- **Impact:** Unnecessary header include — `std::abs(int)` is in `<cstdlib>`, not `<cmath>`. The `<cmath>` header pulls in floating-point math symbols that are never used.
- **Evidence:** [scroll_state.cpp:5](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/heuristics/scroll_state.cpp#L5) — `#include <cmath>`.  
  [scroll_state.cpp:52](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/heuristics/scroll_state.cpp#L52) — `state.accumulator += std::abs(value);`
- **Why it's inefficient:** `<cmath>` is a heavier header than `<cstdlib>`. Minimal build-time impact for a single TU, but violates the "signal density" principle.
- **Recommended fix:** Replace `#include <cmath>` with `#include <cstdlib>`, or inline the abs: `value < 0 ? -value : value`.
- **Tradeoffs / Risks:** None.
- **Expected impact estimate:** Marginal build-time improvement. Code cleanliness.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`scroll_state.cpp`)

---

### F8 — `on_reload()` Manual Field Reset Instead of Aggregate Init

- **Category:** Maintainability / Reliability
- **Severity:** Medium
- **Impact:** Bug risk — if `ScrollState` gains new fields (Phase 2/3), the manual reset in `on_reload()` will silently miss them.
- **Evidence:** [main.cpp:84-89](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/main.cpp#L84-L89)
  ```cpp
  app->scroll.accumulator   = 0;
  app->scroll.direction     = 0;
  app->scroll.pending_dir   = 0;
  app->scroll.last_event_ns = 0;
  app->scroll.last_emit_ns  = 0;
  app->scroll.pending_ts    = 0;
  ```
- **Why it's inefficient:** Six manual assignments that must be kept in sync with the struct definition. A single `app->scroll = {};` achieves the same result (aggregate initialization) and is future-proof.
- **Recommended fix:**
  ```cpp
  app->scroll = {};
  ```
- **Tradeoffs / Risks:** None — `ScrollState` uses default member initializers that are all zero.
- **Expected impact estimate:** Eliminates a maintenance hazard. No runtime change.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`main.cpp`)

---

### F9 — `PacingQueue` Ring Buffer Uses Modulo on Every Operation

- **Category:** CPU / Algorithm
- **Severity:** Low
- **Impact:** Minor — modulo is compiled to a bitwise AND when `max_queued_actions` is a power of 2, but only if the compiler proves it's constant.
- **Evidence:** [pacer.cpp:44,49,67](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/pacer.cpp#L44) — `(q.head + 1) % config::max_queued_actions`.
- **Why it's inefficient:** `config::max_queued_actions` is `constexpr int = 8`, so the compiler *should* optimize `% 8` to `& 7`. However, the cast to `uint8_t` may interfere with the optimization on some compilers. An explicit `& (max_queued_actions - 1)` with a `static_assert` is more self-documenting and guaranteed.
- **Recommended fix:**
  ```cpp
  static_assert((config::max_queued_actions & (config::max_queued_actions - 1)) == 0,
                "max_queued_actions must be a power of 2");
  // Then use:
  q.head = static_cast<uint8_t>((q.head + 1) & (config::max_queued_actions - 1));
  ```
- **Tradeoffs / Risks:** Adds a constraint (`max_queued_actions` must be power of 2). Already true.
- **Expected impact estimate:** Negligible runtime; better self-documentation and guaranteed codegen.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`pacer.cpp`, `constants.hpp`)

---

### F10 — `check_damping()` Is Called on Every `REL_HWHEEL` Event

- **Category:** Algorithm
- **Severity:** Low
- **Impact:** Redundant work — damping check runs on every thumb wheel tick, but damping is only meaningful after input *stops*.
- **Evidence:** [main.cpp:58](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/main.cpp#L58) — `loginext::core::check_damping(app->pacer, ts);`
- **Why it's inefficient:** Every `REL_HWHEEL` event triggers `check_damping()`, which compares timestamp differences. By definition, if we just received an event, `now_ns - last_input_ns` ≈ 0, which is always less than `damping_timeout_ns`. The function always returns early at the `silence >= damping_timeout_ns` check. The cost is trivial (two comparisons + a subtraction), but the call is semantically a no-op when called from the event handler.
- **Recommended fix:** Move damping detection to a separate timer or check it on timer expiry. Alternatively, document that the cost is negligible and keep it for simplicity.
- **Tradeoffs / Risks:** Removing it from the event handler means damping only triggers on the next timer tick, which might be slightly delayed. If the pacing timer is already armed, this is fine. If no timer is armed, the queue would persist until the next event.
- **Expected impact estimate:** ~5ns saved per event. Negligible.
- **Removal Safety:** Needs Verification
- **Reuse Scope:** local file (`main.cpp`)

---

### F11 — No Build Configuration for LTO or `-march=native`

- **Category:** Build
- **Severity:** Low
- **Impact:** Missing ~5-15% performance gain from link-time optimization and architecture-specific instructions.
- **Evidence:** [CMakeLists.txt:31-37](file:///home/vseprr/.gemini/antigravity/scratch/loginext/CMakeLists.txt#L31-L37) — only `-O2 -Wall -Wextra -Wpedantic -Werror`.
- **Why it's inefficient:** LTO allows the compiler to inline across translation units (e.g., `write_event()` → `syn()` → `emit_tab_next()`), eliminating function call overhead in the emit chain. `-march=native` enables hardware-specific instructions. For a latency-critical daemon, both are free wins.
- **Recommended fix:** Add as optional CMake options:
  ```cmake
  option(LOGINEXT_LTO "Enable link-time optimization" ON)
  if(LOGINEXT_LTO)
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()

  option(LOGINEXT_NATIVE "Use -march=native" ON)
  if(LOGINEXT_NATIVE)
      target_compile_options(loginext PRIVATE -march=native)
  endif()
  ```
- **Tradeoffs / Risks:** LTO increases link time. `-march=native` makes the binary non-portable. Both are appropriate for a single-machine daemon.
- **Expected impact estimate:** ~5-15% codegen improvement in emit/passthrough paths.
- **Removal Safety:** Safe
- **Reuse Scope:** service-wide (`CMakeLists.txt`)

---

### F12 — `emitter.cpp` Makes 4-6 `write()` Syscalls Per Tab Switch

- **Category:** I/O / Latency
- **Severity:** Medium
- **Impact:** Each `emit_tab_next()` produces 5 `write()` syscalls (Ctrl-down, Tab-down, SYN, Tab-up, Ctrl-up, SYN → 4 events + 2 SYNs = 6 writes). Each is a context switch.
- **Evidence:** [emitter.cpp:87-99](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/emitter.cpp#L87-L99) — `tap_key_combo()` calls `write_event()` individually for each key press/release, plus `syn()` calls.
- **Why it's inefficient:** `uinput` accepts batched writes. Writing all events in a single `write()` (or `writev()`) reduces syscall count from 6 to 1.
- **Recommended fix:**
  ```cpp
  void tap_key_combo(int fd, const int* keys, int count) noexcept {
      input_event batch[8]; // max: 3 keys down + SYN + 3 keys up + SYN = 8
      int n = 0;
      for (int i = 0; i < count; ++i) {
          batch[n++] = {.type = EV_KEY, .code = static_cast<uint16_t>(keys[i]), .value = 1};
      }
      batch[n++] = {.type = EV_SYN, .code = SYN_REPORT, .value = 0};
      for (int i = count - 1; i >= 0; --i) {
          batch[n++] = {.type = EV_KEY, .code = static_cast<uint16_t>(keys[i]), .value = 0};
      }
      batch[n++] = {.type = EV_SYN, .code = SYN_REPORT, .value = 0};
      [[maybe_unused]] auto r = write(fd, batch, static_cast<size_t>(n) * sizeof(input_event));
  }
  ```
- **Tradeoffs / Risks:** Batched writes are well-supported by `uinput`. If partial writes occur (extremely unlikely for small batches), some keys could get stuck. Add a check in production.
- **Expected impact estimate:** 5→1 syscalls per tab switch. ~10-20μs saved per emission. Medium impact on per-event latency.
- **Removal Safety:** Likely Safe
- **Reuse Scope:** local file (`emitter.cpp`)

---

### F13 — `Parser` Class in `loader.cpp` — Over-Abstraction for Current Scope

- **Category:** Over-Abstracted Code / Maintainability
- **Severity:** Low
- **Impact:** The class has 8 private methods for parsing 2 keys. The OOP style contradicts the project's "flat struct + free function" rule.
- **Evidence:** [loader.cpp:27-126](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/config/loader.cpp#L27-L126) — `Parser` class with `parse()`, `apply_value()`, `parse_string()`, `parse_bool()`, `skip_value()`, `match()`, `consume()`, `peek()`, `skip_ws()`, `fail()`.
- **Why it's inefficient:** The class is fine for correctness and will scale if config grows (Phase 2 schema extension). However, as a class with member state it's the only OOP construct in the codebase. The `agents.md` notes "a single-header parser will be considered if the schema grows."
- **Recommended fix:** No change now — the parser is contained in an anonymous namespace and doesn't leak. Reassess when Phase 2.1 extends the JSON schema. If a full JSON parser is added then, this class should be removed entirely.
- **Tradeoffs / Risks:** Leaving as-is is pragmatic. Converting to free functions would be noisier without benefit.
- **Expected impact estimate:** None — this is a style observation for future planning.
- **Removal Safety:** N/A (no change recommended)
- **Reuse Scope:** local file (`loader.cpp`)

---

### F14 — Missing `EV_MSC` Registration on Virtual Mouse

- **Category:** Reliability
- **Severity:** Medium
- **Impact:** The passthrough path re-emits `EV_MSC` events ([main.cpp:64](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/main.cpp#L64)), but `setup_mouse()` does not register `EV_MSC` capability on the virtual mouse device.
- **Evidence:**
  - [main.cpp:63-66](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/main.cpp#L63-L66) — passthrough includes `ev.type == EV_MSC`.
  - [emitter.cpp:59-84](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/core/emitter.cpp#L59-L84) — `setup_mouse()` registers `EV_KEY` and `EV_REL` but not `EV_MSC`.
- **Why it's inefficient:** uinput silently drops events for unregistered event types. `EV_MSC` (typically `MSC_SCAN`) is passed through but never reaches consumers. This is either dead work (wasted `write()` syscalls) or a functional bug (scan codes lost). MX Master 3S emits `MSC_SCAN` for button presses.
- **Recommended fix:** Either register `EV_MSC` + `MSC_SCAN` in `setup_mouse()`, or filter `EV_MSC` out in the passthrough path to avoid useless writes.
- **Tradeoffs / Risks:** Registering `MSC_SCAN` is the correct fix.
- **Expected impact estimate:** Fixes silent data loss + saves ~1 `write()` per button event if filtering.
- **Removal Safety:** Safe
- **Reuse Scope:** `emitter.cpp` + `main.cpp`

---

### F15 — No `EV_SYN` Capability Check Before Passthrough

- **Category:** Algorithm / Reliability
- **Severity:** Low
- **Impact:** All `EV_SYN` events are passed through unconditionally. `SYN_DROPPED` events from the physical device are re-emitted on the virtual device, which could confuse consumers.
- **Evidence:** [main.cpp:63](file:///home/vseprr/.gemini/antigravity/scratch/loginext/src/main.cpp#L63) — `ev.type == EV_SYN` is always passed through.
- **Why it's inefficient:** `SYN_DROPPED` is a signal from libevdev that events were lost in the kernel buffer. Re-emitting it on the virtual device is misleading since the virtual device has its own buffer. It won't cause functional issues in most consumers, but it's semantically wrong.
- **Recommended fix:** Filter: `if (ev.type == EV_SYN && ev.code == SYN_DROPPED) return;`
- **Tradeoffs / Risks:** Very low risk.
- **Expected impact estimate:** Marginal correctness improvement.
- **Removal Safety:** Safe
- **Reuse Scope:** local file (`main.cpp`)

---

## 3) Quick Wins (Do First)

| # | Finding | Time to Implement | Impact |
|---|---------|-------------------|--------|
| 1 | **F1**: `volatile bool g_stop` → `volatile sig_atomic_t` | 1 min | Critical correctness |
| 2 | **F8**: `on_reload()` reset → `app->scroll = {};` | 1 min | Maintenance safety |
| 3 | **F2**: Drain timerfd read properly in `process_timer()` | 2 min | Prevent CPU spin |
| 4 | **F7**: `<cmath>` → `<cstdlib>` in `scroll_state.cpp` | 1 min | Build cleanliness |
| 5 | **F14**: Register `EV_MSC` on virtual mouse or filter it | 3 min | Correctness |
| 6 | **F3**: Log `write()` failures in `emit_passthrough()`/`write_event()` | 3 min | Diagnosability |

---

## 4) Deeper Optimizations (Do Next)

| # | Finding | Effort | Impact |
|---|---------|--------|--------|
| 1 | **F12**: Batch `write()` calls in `tap_key_combo()` | 15 min | ~10-20μs latency reduction per emit |
| 2 | **F11**: CMake LTO + `-march=native` | 10 min | ~5-15% codegen improvement |
| 3 | **F4**: Replace `ifstream`/`stringstream` with stack-based POSIX read | 20 min | Eliminates heap allocation on reload path |
| 4 | **F9**: Explicit power-of-2 bitmask in ring buffer | 5 min | Guaranteed optimal codegen |
| 5 | **F15**: Filter `SYN_DROPPED` from passthrough | 2 min | Correctness |

---

## 5) Validation Plan

### Automated Tests

1. **Signal safety (F1):** Send `SIGINT` to the daemon under high event load (`evemu-record` replay → `evemu-play`). Verify clean exit within 100ms. Repeat 100 times.
2. **Timerfd drain (F2):** Stress test: enqueue 100 rapid actions, monitor CPU usage. Before fix: may show 100% CPU on a pathological schedule. After fix: CPU stays < 5%.
3. **Write batching (F12):**
   ```bash
   # Before: strace -e write -c loginext
   # After:  strace -e write -c loginext
   # Compare syscall counts per tab switch cycle
   ```

### Profiling Strategy

1. **Event-to-emission latency:** Instrument with `clock_gettime(CLOCK_MONOTONIC)` at event ingress (`on_event`) and emission (`emit_tab_next`). Log delta. Target: < 50μs at p99.
2. **Syscall overhead:** Use `perf stat -e syscalls:sys_enter_write` to count writes per second under sustained scrolling.
3. **Memory profile:** `valgrind --tool=massif` during a SIGHUP reload cycle. Verify zero heap growth after F4 fix.

### Correctness Verification

1. **MSC passthrough (F14):** Use `evtest` on the virtual mouse device. Verify `MSC_SCAN` events appear after registering the capability.
2. **Regression test:** Manual scroll test on all three sensitivity modes after each change. Verify ghost filtering, cooldown, and damping still behave correctly.

---

## 6) Optimized Code / Patches

### Patch 1: Signal Safety (`g_stop`)

```diff
 // main.cpp:18
-volatile bool          g_stop   = false;
+volatile sig_atomic_t  g_stop   = 0;
```

```diff
 // main.cpp:22
-    g_stop = true;
+    g_stop = 1;
```

Run-loop signature in `event_loop.hpp` already uses `volatile bool*` — change to `volatile sig_atomic_t*` or add a cast.

---

### Patch 2: Drain timerfd

```diff
 // pacer.cpp:57-59
 void process_timer(PacingQueue& q, EmitterHandle& em) noexcept {
     uint64_t expirations = 0;
-    [[maybe_unused]] auto r = read(q.timer_fd, &expirations, sizeof(expirations));
+    if (read(q.timer_fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
+        return;  // spurious wake — do not process
+    }
```

---

### Patch 3: Aggregate reset

```diff
 // main.cpp:84-89
-    app->scroll.accumulator   = 0;
-    app->scroll.direction     = 0;
-    app->scroll.pending_dir   = 0;
-    app->scroll.last_event_ns = 0;
-    app->scroll.last_emit_ns  = 0;
-    app->scroll.pending_ts    = 0;
+    app->scroll = {};
```

---

### Patch 4: Batched uinput writes

```diff
 // emitter.cpp — replace tap_key_combo
 void tap_key_combo(int fd, const int* keys, int count) noexcept {
-    for (int i = 0; i < count; ++i) {
-        write_event(fd, EV_KEY, static_cast<uint16_t>(keys[i]), 1);
-    }
-    syn(fd);
-    for (int i = count - 1; i >= 0; --i) {
-        write_event(fd, EV_KEY, static_cast<uint16_t>(keys[i]), 0);
-    }
-    syn(fd);
+    input_event batch[8];
+    int n = 0;
+    for (int i = 0; i < count; ++i) {
+        batch[n] = {};
+        batch[n].type = EV_KEY;
+        batch[n].code = static_cast<uint16_t>(keys[i]);
+        batch[n].value = 1;
+        ++n;
+    }
+    batch[n] = {};
+    batch[n].type = EV_SYN;
+    batch[n].code = SYN_REPORT;
+    ++n;
+    for (int i = count - 1; i >= 0; --i) {
+        batch[n] = {};
+        batch[n].type = EV_KEY;
+        batch[n].code = static_cast<uint16_t>(keys[i]);
+        batch[n].value = 0;
+        ++n;
+    }
+    batch[n] = {};
+    batch[n].type = EV_SYN;
+    batch[n].code = SYN_REPORT;
+    ++n;
+    [[maybe_unused]] auto r = write(fd, batch, static_cast<size_t>(n) * sizeof(input_event));
 }
```

---

### Patch 5: Register `EV_MSC` on virtual mouse

```diff
 // emitter.cpp — in setup_mouse(), after EV_REL block
+    // Enable EV_MSC for scan codes (passthrough)
+    if (ioctl(fd, UI_SET_EVBIT, EV_MSC) < 0) return -1;
+    if (ioctl(fd, UI_SET_MSCBIT, MSC_SCAN) < 0) return -1;
```
