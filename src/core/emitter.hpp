#pragma once

#include "presets/preset.hpp"

#include <linux/input.h>

namespace loginext::core {

struct EmitterHandle {
    int kbd_fd   = -1;  // uinput virtual keyboard (preset-driven combos)
    int mouse_fd = -1;  // uinput virtual mouse (passthrough)
};

// Create uinput devices: keyboard for combos, mouse for passthrough.
[[nodiscard]] int init_emitter(EmitterHandle& em) noexcept;

// Two-phase keystroke emission. The combo is supplied by the active preset;
// the emitter has no preset-specific knowledge.
//   Phase 1: keys pressed in array order  + SYN_REPORT
//   Phase 2: keys released in reverse order + SYN_REPORT
void emit_combo_down(EmitterHandle& em, const presets::KeyCombo& combo) noexcept;
void emit_combo_up  (EmitterHandle& em, const presets::KeyCombo& combo) noexcept;

// Re-emit a raw input_event on the virtual mouse (passthrough).
void emit_passthrough(EmitterHandle& em, const struct input_event& ev) noexcept;

// Destroy uinput devices and close fds.
void destroy_emitter(EmitterHandle& em) noexcept;

} // namespace loginext::core
