#pragma once

#include "config/profile.hpp"

namespace loginext::config {

struct Settings {
    SensitivityMode mode          = SensitivityMode::Medium;
    bool            invert_hwheel = true;
    Profile         profile       = profile_medium;
};

// Synchronize the profile field with the current mode.
// Call after mode is mutated (e.g., after CLI override or config reload).
inline void apply_mode(Settings& s) noexcept {
    s.profile = profile_for(s.mode);
}

} // namespace loginext::config
