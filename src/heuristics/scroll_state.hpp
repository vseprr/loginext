#pragma once

#include "config/profile.hpp"

#include <cstdint>

namespace loginext::heuristics {

enum class ActionResult : uint8_t {
    None,
    TabNext,    // Ctrl+Tab (scroll right / forward)
    TabPrev,    // Ctrl+Shift+Tab (scroll left / back)
};

// Entire scroll engine state — lives on the stack, zero heap
struct ScrollState {
    int32_t accumulator   = 0;    // leaky bucket counter
    int8_t  direction     = 0;    // -1, 0, +1
    int64_t last_event_ns = 0;    // monotonic timestamp of previous event
    int64_t last_emit_ns  = 0;    // monotonic timestamp of previous emit
};

// Process a single REL_HWHEEL event (axis already inverted by caller if needed).
// Returns the action to take (if any).
// timestamp_ns must be from CLOCK_MONOTONIC (or input_event timeval converted).
[[nodiscard]] ActionResult process_hwheel(ScrollState& state,
                                          int32_t value,
                                          int64_t timestamp_ns,
                                          const config::Profile& p) noexcept;

// Drain stale accumulation when no events arrive. Call periodically.
void tick_leak(ScrollState& state, int64_t now_ns, const config::Profile& p) noexcept;

} // namespace loginext::heuristics
