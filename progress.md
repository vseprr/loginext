# LogiNext — Progress

A C++ daemon that gives Logitech mice on Linux the per-control customisation Logitech Options+ offers on Windows / macOS.

This file is the **roadmap**. For user-visible release history see [CHANGELOG.md](./CHANGELOG.md). For long-form architectural decisions, deferred audit findings, and "why is this written this way?" rationale see [KNOWN_ISSUES.md](./KNOWN_ISSUES.md). For implementation rules see [agents.md](./agents.md). For active performance discipline see [OPTIMIZATIONS.md](./OPTIMIZATIONS.md).

---

## Status

| Phase | Theme | State |
|---|---|---|
| 1 | Thumb-wheel engine — gesture-aware tab switching, sensitivity profiles, hot reload, `uinput` virtual mouse + keyboard | ✅ Shipped |
| 2 | Daemon IPC, Tauri UI, per-app rules with sensitivity/invert overrides, KWin DBus focus bridge, udev unprivileged access, systemd-driven lifecycle, packaging | ✅ Stable |
| 3 | Other MX Master 3S controls (Back/Forward, Gesture, Mode-shift, vertical wheel) + new preset families (volume, custom keystroke, run command) | 🚧 Planned |

The daemon and UI are production-ready for the thumb-wheel use case. Phase 3 expands the control surface; the lifecycle, scope, and rule engine that land it are already in place from Phase 2.

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
