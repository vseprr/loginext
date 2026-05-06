#pragma once

#include <cstdint>

namespace loginext::config {

enum class SensitivityMode : uint8_t {
    Low     = 0,
    Medium  = 1,
    High    = 2,
    // Sentinel used by AppRule to mean "no per-app override; resolve to
    // settings.mode at lookup time". Never reaches profile_for() — the
    // hot path checks for Inherit and substitutes the global mode before
    // resolving the Profile reference.
    Inherit = 255,
};

struct Profile {
    int64_t idle_reset_ns;
    int64_t emit_cooldown_ns;
    int32_t fast_threshold;
    int32_t slow_threshold;
    int64_t fast_dt_ns;
    int64_t slow_dt_ns;
    int64_t pacing_interval_ns;
    int64_t damping_timeout_ns;
    int64_t leak_interval_ns;
    int64_t confirmation_window_ns;
    int8_t  reverse_tolerance;      // consecutive reverse ticks to tolerate (jitter debounce)
    int32_t leak_decay_shift;       // right-shift for exponential decay per leak_interval_ns
    int64_t key_release_delay_ns;   // delay between KEY_DOWN and KEY_UP emission
};

inline constexpr Profile profile_low = {
    .idle_reset_ns          = 250'000'000,
    .emit_cooldown_ns       = 180'000'000,
    .fast_threshold         = 2,
    .slow_threshold         = 4,
    .fast_dt_ns             = 50'000'000,
    .slow_dt_ns             = 350'000'000,
    .pacing_interval_ns     = 120'000'000,
    .damping_timeout_ns     = 200'000'000,
    .leak_interval_ns       = 150'000'000,
    .confirmation_window_ns = 80'000'000,
    .reverse_tolerance      = 2,
    .leak_decay_shift       = 1,
    .key_release_delay_ns   = 2'000'000,
};

inline constexpr Profile profile_medium = {
    .idle_reset_ns          = 200'000'000,
    .emit_cooldown_ns       = 100'000'000,
    .fast_threshold         = 1,
    .slow_threshold         = 3,
    .fast_dt_ns             = 40'000'000,
    .slow_dt_ns             = 250'000'000,
    .pacing_interval_ns     = 80'000'000,
    .damping_timeout_ns     = 150'000'000,
    .leak_interval_ns       = 200'000'000,
    .confirmation_window_ns = 70'000'000,
    .reverse_tolerance      = 1,
    .leak_decay_shift       = 1,
    .key_release_delay_ns   = 2'000'000,
};

inline constexpr Profile profile_high = {
    .idle_reset_ns          = 150'000'000,
    .emit_cooldown_ns       = 50'000'000,
    .fast_threshold         = 1,
    .slow_threshold         = 2,
    .fast_dt_ns             = 30'000'000,
    .slow_dt_ns             = 200'000'000,
    .pacing_interval_ns     = 60'000'000,
    .damping_timeout_ns     = 120'000'000,
    .leak_interval_ns       = 200'000'000,
    .confirmation_window_ns = 60'000'000,
    .reverse_tolerance      = 1,
    .leak_decay_shift       = 1,
    .key_release_delay_ns   = 1'500'000,
};

constexpr const Profile& profile_for(SensitivityMode m) noexcept {
    switch (m) {
        case SensitivityMode::Low:    return profile_low;
        case SensitivityMode::Medium: return profile_medium;
        case SensitivityMode::High:   return profile_high;
        case SensitivityMode::Inherit: break;  // caller must resolve before this point
    }
    return profile_medium;
}

constexpr const char* mode_name(SensitivityMode m) noexcept {
    switch (m) {
        case SensitivityMode::Low:    return "low";
        case SensitivityMode::Medium: return "medium";
        case SensitivityMode::High:   return "high";
        case SensitivityMode::Inherit: return "";
    }
    return "medium";
}

// Parse a sensitivity mode from its string form. Empty input maps to
// Inherit so per-app rules with no mode override round-trip cleanly.
// Used by the app_rules.txt loader and IPC command parser.
constexpr bool mode_from_str(const char* s, std::size_t n,
                             SensitivityMode& out) noexcept {
    auto eq = [&](const char* lit) noexcept {
        std::size_t i = 0;
        while (i < n && lit[i] != '\0' && lit[i] == s[i]) ++i;
        return i == n && lit[i] == '\0';
    };
    if (n == 0)         { out = SensitivityMode::Inherit; return true; }
    if (eq("low"))      { out = SensitivityMode::Low;     return true; }
    if (eq("medium"))   { out = SensitivityMode::Medium;  return true; }
    if (eq("high"))     { out = SensitivityMode::High;    return true; }
    if (eq("inherit"))  { out = SensitivityMode::Inherit; return true; }
    return false;
}

} // namespace loginext::config
