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

// Emit Ctrl+Tab keystroke sequence.
void emit_tab_next(EmitterHandle& em) noexcept;

// Emit Ctrl+Shift+Tab keystroke sequence.
void emit_tab_prev(EmitterHandle& em) noexcept;

// Emit an action result (dispatches to tab_next/tab_prev).
void emit_action(EmitterHandle& em, loginext::heuristics::ActionResult action) noexcept;

// Re-emit a raw input_event on the virtual mouse (passthrough).
void emit_passthrough(EmitterHandle& em, const struct input_event& ev) noexcept;

// Destroy uinput devices and close fds.
void destroy_emitter(EmitterHandle& em) noexcept;

} // namespace loginext::core
