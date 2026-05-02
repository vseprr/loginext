#pragma once

#include "scope/rules.hpp"

#include <string>

namespace loginext::scope {

// Resolve $XDG_CONFIG_HOME/loginext/app_rules.txt (with sudo + HOME fallbacks
// matching config::default_config_path()). Empty string if it cannot resolve.
std::string default_rules_path();

// Parse the sidecar text-format rule file into `out`.
//
// Format: line-based, one rule per line, `app=preset_id`. `#` starts a
// comment, blank lines ignored. App names are lower-cased and FNV-1a hashed
// at load time so the hot path only ever sees integer compares. Missing
// file is not an error — `out` is cleared and the call returns true (the
// daemon then falls back to the global preset for everything).
bool load_rules(const std::string& path, RuleTable& out) noexcept;

} // namespace loginext::scope
