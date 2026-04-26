#pragma once

#include "config/profile.hpp"

#include <cstdint>

namespace loginext::heuristics {

// Logical tick produced by the heuristic engine. The engine has zero
// knowledge of presets, key combos, or output devices — it answers a
// single question: did the user just gesture left or right?
//
// Mapping a Direction onto a concrete action lives entirely in
// loginext::presets — the heuristic and the action layer are deliberately
// decoupled so that adding a new preset cannot perturb the NBT "feel".
enum class Direction : uint8_t {
    None,
    Left,    // negative HWHEEL travel (scroll left / back)
    Right,   // positive HWHEEL travel (scroll right / forward)
};

// Entire scroll engine state — lives on the stack, zero heap
struct ScrollState {
    int32_t accumulator   = 0;    // leaky bucket counter
    int8_t  direction     = 0;    // -1, 0, +1
    int8_t  pending_dir   = 0;    // unconfirmed gesture-start direction
    int8_t  reverse_count = 0;    // consecutive reverse-direction events (jitter debounce)
    int64_t last_event_ns = 0;    // monotonic timestamp of previous event
    int64_t last_emit_ns  = 0;    // monotonic timestamp of previous emit
    int64_t pending_ts    = 0;    // timestamp of unconfirmed gesture-start
};

// Process a single REL_HWHEEL event (axis already inverted by caller if needed).
// Returns the logical tick produced by the heuristic, or Direction::None.
// timestamp_ns must be from CLOCK_MONOTONIC (or input_event timeval converted).
[[nodiscard]] Direction process_hwheel(ScrollState& state,
                                       int32_t value,
                                       int64_t timestamp_ns,
                                       const config::Profile& p) noexcept;

// Drain stale accumulation when no events arrive. Call periodically.
void tick_leak(ScrollState& state, int64_t now_ns, const config::Profile& p) noexcept;

} // namespace loginext::heuristics
