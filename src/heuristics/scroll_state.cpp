#include "heuristics/scroll_state.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace loginext::heuristics {

namespace {

int8_t sign(int32_t v) noexcept {
    return static_cast<int8_t>((v > 0) - (v < 0));
}

ActionResult action_for_direction(int8_t dir) noexcept {
    return dir > 0 ? ActionResult::TabNext : ActionResult::TabPrev;
}

// Continuous velocity curve: lerp between fast_threshold and slow_threshold
// based on where Δt falls in [fast_dt_ns, slow_dt_ns].
int32_t dynamic_threshold(int64_t dt_ns, const config::Profile& p) noexcept {
    int64_t clamped = std::clamp(dt_ns, p.fast_dt_ns, p.slow_dt_ns);
    int64_t numerator   = static_cast<int64_t>(p.slow_threshold - p.fast_threshold)
                        * (clamped - p.fast_dt_ns);
    int64_t denominator = p.slow_dt_ns - p.fast_dt_ns;
    return p.fast_threshold + static_cast<int32_t>(numerator / denominator);
}

} // namespace

ActionResult process_hwheel(ScrollState& state, int32_t value,
                            int64_t timestamp_ns,
                            const config::Profile& p) noexcept {
    if (value == 0) return ActionResult::None;

    const int64_t dt = (state.last_event_ns > 0)
                       ? (timestamp_ns - state.last_event_ns)
                       : INT64_MAX;

    // Idle reset: silence longer than idle_reset_ns starts a fresh gesture
    if (dt > p.idle_reset_ns) {
        state.accumulator = 0;
        state.direction   = 0;
    }

    const int8_t event_dir = sign(value);
    if (event_dir != state.direction) {
        state.accumulator = 0;
        state.direction   = event_dir;
    }

    state.accumulator  += std::abs(value);
    state.last_event_ns = timestamp_ns;

    // Leading-edge: first event of a gesture fires immediately
    const bool gesture_start = (state.last_emit_ns == 0)
                            || (timestamp_ns - state.last_emit_ns > p.idle_reset_ns);
    if (gesture_start) {
        state.accumulator  = 0;
        state.last_emit_ns = timestamp_ns;
        return action_for_direction(event_dir);
    }

    // Cooldown: same-gesture emissions must be spaced by emit_cooldown_ns
    if (timestamp_ns - state.last_emit_ns < p.emit_cooldown_ns) {
        return ActionResult::None;
    }

    // Velocity-curve threshold for sustained swipes
    const int32_t threshold = dynamic_threshold(dt, p);
    if (state.accumulator >= threshold) {
        state.accumulator  = 0;
        state.last_emit_ns = timestamp_ns;
        return action_for_direction(event_dir);
    }

    return ActionResult::None;
}

void tick_leak(ScrollState& state, int64_t now_ns, const config::Profile& p) noexcept {
    if (state.accumulator == 0) return;
    if (state.last_event_ns == 0) return;

    int64_t elapsed = now_ns - state.last_event_ns;
    if (elapsed >= p.leak_interval_ns) {
        int32_t drain = static_cast<int32_t>(elapsed / p.leak_interval_ns);
        state.accumulator = std::max(0, state.accumulator - drain);
    }
}

} // namespace loginext::heuristics
