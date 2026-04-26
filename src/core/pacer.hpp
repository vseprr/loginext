#pragma once

#include "config/constants.hpp"
#include "config/profile.hpp"
#include "core/emitter.hpp"
#include "presets/preset.hpp"

#include <array>
#include <cstdint>

namespace loginext::core {

struct PacingQueue {
    // Queue holds resolved KeyCombos rather than logical directions. The
    // resolution happens at enqueue time so that switching the active preset
    // mid-burst affects only future ticks, never in-flight ones.
    std::array<presets::KeyCombo, config::max_queued_actions> buffer{};
    uint8_t head  = 0;
    uint8_t tail  = 0;
    uint8_t count = 0;
    int     timer_fd = -1;
    int64_t last_input_ns = 0;                      // for damping detection
    const config::Profile* profile = nullptr;       // non-owning, set by main

    // Two-phase keystroke state: when true, keys are held down and the
    // next timer fire must emit KEY_UP before processing the next combo.
    bool             release_pending = false;
    presets::KeyCombo held_combo{};
};

// Create timerfd for pacing.
[[nodiscard]] int init_pacer(PacingQueue& q) noexcept;

// Push a combo into the ring buffer. Arms the timer if this is the first item.
// Zero-count combos are dropped silently.
void enqueue_combo(PacingQueue& q, const presets::KeyCombo& combo,
                   int64_t now_ns) noexcept;

// Called when timerfd fires: two-phase state machine.
// Phase 1: dequeue combo → emit KEY_DOWN → re-arm for key_release_delay_ns.
// Phase 2: emit KEY_UP → re-arm for pacing_interval_ns (or disarm if queue empty).
void process_timer(PacingQueue& q, EmitterHandle& em) noexcept;

// Check if input has been silent long enough to flush the queue.
// Safety: if keys are held down during damping, releases them immediately.
void check_damping(PacingQueue& q, int64_t now_ns, EmitterHandle& em) noexcept;

// Close timerfd.
void destroy_pacer(PacingQueue& q) noexcept;

} // namespace loginext::core
