#pragma once

#include "config/profile.hpp"
#include "presets/preset.hpp"

namespace loginext::config {

struct Settings {
    SensitivityMode    mode          = SensitivityMode::Medium;
    bool               invert_hwheel = true;
    Profile            profile       = profile_medium;

    // Active binding for the thumb wheel. Phase 2.4 ships with one preset
    // (NBT); Phase 2.5 will swap this out per active window via an O(1)
    // lookup keyed on app id, mutating this field on the event-loop thread.
    presets::PresetId  active_preset = presets::PresetId::TabNav;
};

// Synchronize the profile field with the current mode.
// Call after mode is mutated (e.g., after CLI override or config reload).
inline void apply_mode(Settings& s) noexcept {
    s.profile = profile_for(s.mode);
}

} // namespace loginext::config
