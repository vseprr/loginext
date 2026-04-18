#pragma once

#include "heuristics/scroll_state.hpp"

#include <linux/input.h>

namespace loginext::core {

struct EmitterHandle {
    int kbd_fd   = -1;  // uinput virtual keyboard (Ctrl+Tab)
    int mouse_fd = -1;  // uinput virtual mouse (passthrough)
};

// Create uinput devices: keyboard for tab switching, mouse for passthrough.
[[nodiscard]] int init_emitter(EmitterHandle& em) noexcept;

// Two-phase keystroke emission: press modifiers+key down, then release later.
// Phase 1: KEY_DOWN + SYN
void emit_action_down(EmitterHandle& em, loginext::heuristics::ActionResult action) noexcept;

// Phase 2: KEY_UP + SYN
void emit_action_up(EmitterHandle& em, loginext::heuristics::ActionResult action) noexcept;

// Re-emit a raw input_event on the virtual mouse (passthrough).
void emit_passthrough(EmitterHandle& em, const struct input_event& ev) noexcept;

// Destroy uinput devices and close fds.
void destroy_emitter(EmitterHandle& em) noexcept;

} // namespace loginext::core
