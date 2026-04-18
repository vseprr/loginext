# LogiNext — Progress

## Phase 1: MX Master 3S — Intelligent Tab Navigator (Thumb Wheel)

### Milestone 1: Device I/O
- [x] Enumerate `/dev/input/` and auto-detect MX Master 3S by vendor/product ID
- [x] Exclusive `libevdev` grab on the device node
- [x] `uinput` virtual keyboard creation + teardown
- [x] `epoll`-based event loop (single-thread, non-blocking)

### Milestone 2: Heuristic Engine
- [ ] Invert scroll axis direction
- [ ] Leaky-bucket accumulator for `REL_HWHEEL` events
- [ ] Velocity-based thresholding (Δt → dynamic action threshold)
- [ ] Burst mode detection (fast swipe = low friction)

### Milestone 3: Output & Pacing
- [ ] `Ctrl+Tab` / `Ctrl+Shift+Tab` emission via `uinput`
- [ ] Event pacing queue (60–80ms inter-event delay via `timerfd`)
- [ ] Synthetic damping (brake queued events when input stops)

### Milestone 4: Polish
- [ ] Graceful signal handling (`SIGINT`, `SIGTERM`)
- [ ] CLI arg for manual device path override
- [ ] Tuning constants in `src/config/`

---

## Phase 2: (Future) Additional Gestures & Buttons
*Scope TBD after Phase 1 is stable.*
