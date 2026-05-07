# LogiNext — Progress

A C++ daemon that gives Logitech mice on Linux the per-control customisation Logitech Options+ offers on Windows / macOS.

This file is the **roadmap**. For user-visible release history see [CHANGELOG.md](./CHANGELOG.md). For long-form architectural decisions, deferred audit findings, and "why is this written this way?" rationale see [KNOWN_ISSUES.md](./KNOWN_ISSUES.md). For implementation rules see [agents.md](./agents.md). For active performance discipline see [OPTIMIZATIONS.md](./OPTIMIZATIONS.md).

---

## Status

| Phase | Theme | State |
|---|---|---|
| 1 | Thumb-wheel engine — gesture-aware tab switching, sensitivity profiles, hot reload, `uinput` virtual mouse + keyboard | ✅ COMPLETED — v1.0.0 |
| 2 | Daemon IPC (`RuntimeDirectory`-managed UDS), Tauri UI, per-app rules with sensitivity/invert overrides, KWin DBus focus bridge with cold-boot bootstrap, udev unprivileged access, systemd-driven lifecycle (template v8 with cgroup ceilings + oomd hints), packaging (`install.sh` + Arch `PKGBUILD`) | ✅ COMPLETED — v1.0.0 |
| 3 | Other MX Master 3S controls (Back/Forward, Gesture, Mode-shift, vertical wheel) + new preset families (volume, custom keystroke, run command) | 🚧 Planned |

v1.0.0 ships the daemon and UI as production-ready for the thumb-wheel use case on Plasma 6 / CachyOS. Phase 3 expands the control surface; the lifecycle, scope, rule engine, listener-thread CPU posture, and cold-boot race fixes that land it are already in place from Phase 2.

### v1.0.0 closeout — what landed in the final cycle

- [x] Listener-thread CPU spinner fixed (the 30-second `select(timeout=0)` busy-loop in `kwin_dbus_loop`'s diagnostic-deadline math). Idle daemon: ~40 ms CPU per minute, down from ~30 s of CPU per minute.
- [x] Cold-boot rule activation works without the UI being launched. Direct `org.kde.KWin.NameHasOwner` / `wayland-N` socket / `xcb_connect` probes replaced the env-var gates that the systemd-user / Plasma session-env import race made unusable. 30 × 2 s polling loop catches up the moment a compositor surfaces.
- [x] Cold-boot KWin bootstrap. After the listener binds `org.loginext.WindowFocus`, an inline one-shot KWin script is loaded via `org.kde.kwin.Scripting.loadScript` + `Script.run` to push the active window directly to our `Activated` handler — bypassing the persistent `loginext-focus` script if it isn't enabled in `kwinrc`. Falls back to `org.kde.KWin.reconfigure` if `loadScript` is rejected.
- [x] Bounded udev grace window inside `find_device()` (10 × 2 s) so a slow-enumerating `/dev/input/event*` at cold boot doesn't burn through `StartLimitBurst`. Unit's `RestartSec` bumped to 15 s and `StartLimitIntervalSec` to 120 s in lockstep.
- [x] Cgroup resource ceilings in the unit (`MemoryHigh=32M`, `MemoryMax=64M`, `TasksMax=32`, `CPUQuota=50%`) plus `ManagedOOMPreference=avoid` / `ManagedOOMMemoryPressure=kill` / `ManagedOOMSwap=kill`. Caps damage if the pacer state machine ever wedges and tells systemd-oomd to kill other cgroups first under PSI pressure.
- [x] Secure IPC socket via `RuntimeDirectory=loginext` + `RuntimeDirectoryMode=0700`. Socket lives at `$XDG_RUNTIME_DIR/loginext/loginext.sock`; systemd creates the directory before exec and tears it down on stop. Stale sockets across crashes are no longer possible.
- [x] IPC bring-up moved BEFORE the device retry loop. The UI's 5-second socket-existence probe now passes immediately, even when the daemon is mid-retry on a slow-enumerating receiver.
- [x] `--debug-perf` CLI flag for per-second wakeup / event / sd_bus_process counters. Was the diagnostic that exposed the listener spinner; left in for future regressions.
- [x] Tauri `bundle.targets` set to `["deb"]` so `npm run tauri build` no longer fails on the AppImage bundler's 512 × 512 square-icon validation.
- [x] `install.sh` enables the systemd unit by default; pass `--no-enable` for headless / debugging setups.
- [x] Versions bumped to 1.0.0 across `tauri.conf.json` and `ui/package.json`.

---

## Phase 3 — Other controls (active roadmap)

Each control listed here joins the same `(PresetId, Direction) → Action` table that NBT and Zoom already use. Adding a control is strictly additive: a new heuristic state under `heuristics/`, a new preset arm in `presets/preset.hpp`, a new `controls` entry in the IPC `list_controls` handler, and the UI populates the new card with no surface changes.

### Hardware discovery

- [ ] Capture raw `(type, code, value)` signatures for every MX Master 3S control via `--debug-events`. Cover: Gesture button (under thumb), Back, Forward, Mode-shift, vertical scroll click, vertical scroll axis. Document each in [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) under "MX Master 3S device profile" so the mapping is reproducible without re-running the dump.
- [ ] Codify the captured mapping as a constexpr device profile (`presets/mx_master_3s.hpp` or extend `presets/preset.hpp`). Additive only — the Phase-1 NBT timings and the `config::Profile` constants are non-negotiable.

### New controls

- [ ] **Back / Forward buttons.** Single-shot key events; reuse the Heuristics::Direction taxonomy by treating Back as `Left` and Forward as `Right`. Likely preset family: history navigation (`Alt+Left` / `Alt+Right`), tab close/reopen (`Ctrl+W` / `Ctrl+Shift+T`), undo/redo (`Ctrl+Z` / `Ctrl+Shift+Z`).
- [ ] **Gesture button (under thumb).** Press-and-hold + REL_X / REL_Y → directional gesture. Heuristic state lives in a new `heuristics/gesture_state` so the existing scroll engine stays untouched. Preset families: workspace switch, window switcher, application launcher.
- [ ] **Vertical scroll wheel.** Already passed through; the question is whether to add a "remap to volume" preset family that gates on per-app rules. Investigate SmartShift toggle integration via the device's HID++ feature page.
- [ ] **Mode-shift button.** If the device exposes a usable side-channel for "alternate layer", remap it to a Caps-Lock-style modifier so a single button doubles a control's preset family.

### New preset families (cross-cutting)

- [ ] Volume up/down (`KEY_VOLUMEUP` / `KEY_VOLUMEDOWN`), with a third "mute" arm for the Gesture button's press direction.
- [ ] Custom keystroke (user-supplied keycombo). New IPC command `set_custom_combo`; UI gains a "record combo" capture state.
- [ ] Run command (user-supplied shell exec). Gated behind a confirmation in the UI because it crosses a privilege boundary the rest of the bindings don't.

### IPC surface

- [ ] Multi-message IPC channel for live preview overlays (currently every request is a single round-trip). Needed for the "stream emitted Tab events back to the UI while the panel is open" feature kicked down the road from Phase 2.4.

---

## Permanent design notes

- **No heap allocation on the hot path.** Reload path is a documented exception (4 KiB stack buffer + flat parser; see [OPTIMIZATIONS.md](./OPTIMIZATIONS.md)).
- **UI writes the config file + sends `reload`.** Never touches the hot path directly.
- **New presets follow the same shape:** state under `heuristics/`, output via `core/emitter`, mapping in `presets/`. Wire-up belongs in the IPC `list_controls` / `list_presets` handlers.
- **UI components that own state treat the DOM attribute (`aria-selected`, `aria-checked`) as the source of truth.** Closure-captured state is a known trap (see [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) — "UI state-sync correctness").
- **Daemon runs as the seat user.** Hardware access is granted via `deploy/udev/99-loginext.rules` (`uaccess` ACL + `input` group fallback). Running under sudo is unsupported — Plasma 6's session-bus broker rejects EXTERNAL auth from uid 0. See [KNOWN_ISSUES.md](./KNOWN_ISSUES.md).
