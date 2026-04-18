#pragma once

#include "config/settings.hpp"

#include <string>

namespace loginext::config {

// Returns $XDG_CONFIG_HOME/loginext/config.json or
// $HOME/.config/loginext/config.json (empty string if neither is set).
std::string default_config_path();

// Load settings from a flat JSON file.
// Recognized keys: "sensitivity" ("low"|"medium"|"high"), "invert_hwheel" (bool).
// On missing file: returns false silently, leaves s untouched.
// On parse error: logs to stderr and returns false, leaves s untouched.
// On success: updates s (and calls apply_mode) and returns true.
bool load_settings(const std::string& path, Settings& s) noexcept;

} // namespace loginext::config
