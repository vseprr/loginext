#pragma once

#include <cstdint>

namespace loginext::config {

enum class SensitivityMode : uint8_t {
    Low,
    Medium,
    High,
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
};

inline constexpr Profile profile_low = {
    .idle_reset_ns      = 250'000'000,
    .emit_cooldown_ns   = 180'000'000,
    .fast_threshold     = 2,
    .slow_threshold     = 4,
    .fast_dt_ns         = 50'000'000,
    .slow_dt_ns         = 350'000'000,
    .pacing_interval_ns = 120'000'000,
    .damping_timeout_ns = 200'000'000,
    .leak_interval_ns   = 150'000'000,
};

inline constexpr Profile profile_medium = {
    .idle_reset_ns      = 200'000'000,
    .emit_cooldown_ns   = 100'000'000,
    .fast_threshold     = 1,
    .slow_threshold     = 3,
    .fast_dt_ns         = 40'000'000,
    .slow_dt_ns         = 250'000'000,
    .pacing_interval_ns = 80'000'000,
    .damping_timeout_ns = 150'000'000,
    .leak_interval_ns   = 200'000'000,
};

inline constexpr Profile profile_high = {
    .idle_reset_ns      = 150'000'000,
    .emit_cooldown_ns   = 50'000'000,
    .fast_threshold     = 1,
    .slow_threshold     = 2,
    .fast_dt_ns         = 30'000'000,
    .slow_dt_ns         = 200'000'000,
    .pacing_interval_ns = 60'000'000,
    .damping_timeout_ns = 120'000'000,
    .leak_interval_ns   = 200'000'000,
};

constexpr const Profile& profile_for(SensitivityMode m) noexcept {
    switch (m) {
        case SensitivityMode::Low:    return profile_low;
        case SensitivityMode::Medium: return profile_medium;
        case SensitivityMode::High:   return profile_high;
    }
    return profile_low;
}

constexpr const char* mode_name(SensitivityMode m) noexcept {
    switch (m) {
        case SensitivityMode::Low:    return "low";
        case SensitivityMode::Medium: return "medium";
        case SensitivityMode::High:   return "high";
    }
    return "low";
}

} // namespace loginext::config
