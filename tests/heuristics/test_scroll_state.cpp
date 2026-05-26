// Unit tests for the scroll heuristic state machine.
//
// The engine has three distinct emit gates:
//   1. Gesture-start confirmation (pending → confirmed within
//      confirmation_window_ns by a same-direction follow-up).
//   2. Cooldown (emit_cooldown_ns since last emit).
//   3. Velocity-curve threshold (accumulator ≥ dynamic_threshold(dt)).
//
// We exercise each gate independently using the medium profile. Time is
// fed in monotonically as if from CLOCK_MONOTONIC — no real clock is used.

#include <gtest/gtest.h>

#include "config/profile.hpp"
#include "heuristics/scroll_state.hpp"

namespace lh = loginext::heuristics;
namespace lc = loginext::config;

namespace {

constexpr int64_t ns_per_ms = 1'000'000;

// Drive a confirmed gesture-start: emit the leading pending event, then a
// same-direction follow-up within the confirmation window. After this
// helper, `state.last_emit_ns` is non-zero and subsequent process_hwheel
// calls land in the post-warmup branches (cooldown + velocity threshold).
void warm_up(lh::ScrollState& s, int8_t dir, int64_t& t,
             const lc::Profile& p) {
    const int32_t v = static_cast<int32_t>(dir);
    // First event: pending, no emit.
    ASSERT_EQ(lh::process_hwheel(s, v, t, p), lh::Direction::None);
    // Second event well within the confirmation window: confirms + emits.
    t += p.confirmation_window_ns / 2;
    const lh::Direction d = lh::process_hwheel(s, v, t, p);
    ASSERT_NE(d, lh::Direction::None);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Gesture-start confirmation
// ─────────────────────────────────────────────────────────────────────

TEST(ScrollState, SingleEventDoesNotEmit) {
    // A single isolated event must NOT emit — the engine holds it as a
    // pending gesture-start candidate and waits for confirmation. This is
    // the "ghost rejection" property: a stray tick alone does nothing.
    lh::ScrollState s;
    EXPECT_EQ(lh::process_hwheel(s, +1, 1 * ns_per_ms, lc::profile_medium),
              lh::Direction::None);
}

TEST(ScrollState, ConfirmedGestureEmitsRight) {
    // Two same-direction events within confirmation_window_ns confirm
    // the gesture and emit Right.
    lh::ScrollState s;
    const auto& p = lc::profile_medium;
    EXPECT_EQ(lh::process_hwheel(s, +1, 10 * ns_per_ms, p),
              lh::Direction::None);
    EXPECT_EQ(lh::process_hwheel(s, +1, 10 * ns_per_ms
                                       + p.confirmation_window_ns / 2, p),
              lh::Direction::Right);
}

TEST(ScrollState, ConfirmedGestureEmitsLeft) {
    lh::ScrollState s;
    const auto& p = lc::profile_medium;
    EXPECT_EQ(lh::process_hwheel(s, -1, 10 * ns_per_ms, p),
              lh::Direction::None);
    EXPECT_EQ(lh::process_hwheel(s, -1, 10 * ns_per_ms
                                       + p.confirmation_window_ns / 2, p),
              lh::Direction::Left);
}

TEST(ScrollState, StaleFollowUpDoesNotEmit) {
    // Follow-up arrives well after confirmation_window_ns. The pending
    // candidate goes stale; the follow-up becomes a new pending candidate
    // rather than a confirmation, so still no emit.
    lh::ScrollState s;
    const auto& p = lc::profile_medium;
    EXPECT_EQ(lh::process_hwheel(s, +1, 10 * ns_per_ms, p),
              lh::Direction::None);
    EXPECT_EQ(lh::process_hwheel(s, +1, 10 * ns_per_ms
                                       + p.confirmation_window_ns + ns_per_ms,
                                 p),
              lh::Direction::None);
}

// ─────────────────────────────────────────────────────────────────────
// Idle reset
// ─────────────────────────────────────────────────────────────────────

TEST(ScrollState, IdleResetClearsAccumulator) {
    // After a confirmed gesture, walk forward past idle_reset_ns with no
    // events; tick_leak should bleed the accumulator to zero. (The hot
    // path also resets on its own when the next event arrives past the
    // idle window, but tick_leak is the explicit drain.)
    lh::ScrollState s;
    const auto& p = lc::profile_medium;
    int64_t t = 1 * ns_per_ms;
    warm_up(s, +1, t, p);

    // Drive accumulator above zero with a near-tick (won't emit because
    // we're still inside the cooldown), then leak it out.
    t += p.emit_cooldown_ns / 4;
    (void)lh::process_hwheel(s, +1, t, p);
    EXPECT_GT(s.accumulator, 0);

    // Many leak periods later, the accumulator decays to zero.
    lh::tick_leak(s, t + p.leak_interval_ns * 64, p);
    EXPECT_EQ(s.accumulator, 0);
}

TEST(ScrollState, IdleResetOnNextEvent) {
    // An event arriving more than idle_reset_ns after the last one
    // restarts the gesture state — accumulator, direction, reverse_count
    // all clear. Observable effect: the engine treats this as a fresh
    // gesture-start and does not emit.
    lh::ScrollState s;
    const auto& p = lc::profile_medium;
    int64_t t = 1 * ns_per_ms;
    warm_up(s, +1, t, p);

    // Long silence, then a single event in the opposite direction.
    t += p.idle_reset_ns * 4;
    EXPECT_EQ(lh::process_hwheel(s, -1, t, p), lh::Direction::None);
}

// ─────────────────────────────────────────────────────────────────────
// Direction-change jitter debounce (reverse_tolerance)
// ─────────────────────────────────────────────────────────────────────

TEST(ScrollState, JitterAbsorbedWithinTolerance) {
    // A single reverse tick mid-gesture (≤ reverse_tolerance) is silently
    // absorbed: direction state stays positive, no emit.
    lh::ScrollState s;
    const auto& p = lc::profile_medium;
    int64_t t = 1 * ns_per_ms;
    warm_up(s, +1, t, p);
    ASSERT_EQ(s.direction, +1);

    // Inject a lone reverse tick — should not flip direction.
    t += p.emit_cooldown_ns / 4;
    EXPECT_EQ(lh::process_hwheel(s, -1, t, p), lh::Direction::None);
    EXPECT_EQ(s.direction, +1);
}

TEST(ScrollState, GenuineDirectionChangeFlipsDirection) {
    // Enough consecutive reverse events to exceed reverse_tolerance
    // confirm a genuine direction change. Per scroll_state.cpp, the
    // confirming event itself also accumulates (|value| is added after
    // the reset), so accumulator ends at the magnitude of that event,
    // not 0. Direction must flip and reverse_count must clear.
    lh::ScrollState s;
    const auto& p = lc::profile_medium;
    int64_t t = 1 * ns_per_ms;
    warm_up(s, +1, t, p);

    // Fire reverse ticks until the tolerance is exceeded. reverse_count
    // increments once per reverse event; the (tolerance+1)-th flips state.
    for (int i = 0; i <= p.reverse_tolerance; ++i) {
        t += p.emit_cooldown_ns / 8;
        (void)lh::process_hwheel(s, -1, t, p);
    }
    EXPECT_EQ(s.direction,     -1);
    EXPECT_EQ(s.reverse_count,  0);
    // Confirming event contributes |value| (= 1) post-reset.
    EXPECT_EQ(s.accumulator,    1);
}

// ─────────────────────────────────────────────────────────────────────
// Zero-value short-circuit
// ─────────────────────────────────────────────────────────────────────

TEST(ScrollState, ZeroValueIsNoOp) {
    // value == 0 returns None immediately and must NOT advance any state.
    lh::ScrollState s;
    const lh::ScrollState before = s;
    EXPECT_EQ(lh::process_hwheel(s, 0, 123 * ns_per_ms, lc::profile_medium),
              lh::Direction::None);
    EXPECT_EQ(s.accumulator,   before.accumulator);
    EXPECT_EQ(s.direction,     before.direction);
    EXPECT_EQ(s.last_event_ns, before.last_event_ns);
}

// ─────────────────────────────────────────────────────────────────────
// tick_leak edge cases
// ─────────────────────────────────────────────────────────────────────

TEST(ScrollState, LeakIsNoOpWhenIdle) {
    // tick_leak on a freshly-constructed state must not touch anything —
    // there is nothing to drain and last_event_ns is 0.
    lh::ScrollState s;
    lh::tick_leak(s, 999 * ns_per_ms, lc::profile_medium);
    EXPECT_EQ(s.accumulator, 0);
    EXPECT_EQ(s.last_event_ns, 0);
}
