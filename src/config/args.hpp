#pragma once

#include "config/profile.hpp"

#include <string>

namespace loginext::config {

struct CliOptions {
    std::string     config_path;      // empty → default_config_path()
    SensitivityMode mode = SensitivityMode::Medium;
    bool            invert_hwheel = true;
    bool            cli_mode_set = false;
    bool            cli_invert_set = false;
    bool            help = false;
    bool            quiet = false;    // suppress stderr (file log still active)
    bool            verbose = false;  // lower file-log threshold to Trace
    bool            debug_events = false;  // dump raw libevdev events to stderr (hardware discovery)
};

// Returns 0 on success, nonzero on error. Prints usage on --help or error.
int parse_args(int argc, char* argv[], CliOptions& out);

void print_usage(const char* prog);

} // namespace loginext::config
