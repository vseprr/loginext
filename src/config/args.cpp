#include "config/args.hpp"

#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <string>

namespace loginext::config {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --mode=MODE       low | medium | high  (overrides config file)\n"
        "  --invert=BOOL     true | false         (overrides config file)\n"
        "  --config=PATH     path to JSON config  (default: $XDG_CONFIG_HOME/loginext/config.json)\n"
        "  --quiet           suppress stderr (file log keeps running)\n"
        "  --verbose         file log includes per-event traces\n"
        "  --debug-events    dump raw libevdev events to stderr (hardware discovery; use with UI offline)\n"
        "  --help            show this message\n"
        "\n"
        "SIGHUP reloads the config file without restarting.\n"
        "Detailed logs: tail -f $XDG_STATE_HOME/loginext/daemon.log\n",
        prog);
}

namespace {

bool parse_mode(const char* arg, SensitivityMode& out) {
    if (std::strcmp(arg, "low")    == 0) { out = SensitivityMode::Low;    return true; }
    if (std::strcmp(arg, "medium") == 0) { out = SensitivityMode::Medium; return true; }
    if (std::strcmp(arg, "high")   == 0) { out = SensitivityMode::High;   return true; }
    return false;
}

bool parse_bool(const char* arg, bool& out) {
    if (std::strcmp(arg, "true")  == 0 || std::strcmp(arg, "1") == 0) { out = true;  return true; }
    if (std::strcmp(arg, "false") == 0 || std::strcmp(arg, "0") == 0) { out = false; return true; }
    return false;
}

} // namespace

int parse_args(int argc, char* argv[], CliOptions& out) {
    static const option long_opts[] = {
        {"mode",    required_argument, nullptr, 'm'},
        {"invert",  required_argument, nullptr, 'i'},
        {"config",  required_argument, nullptr, 'c'},
        {"quiet",   no_argument,       nullptr, 'q'},
        {"verbose", no_argument,       nullptr, 'v'},
        {"debug-events", no_argument,  nullptr, 'd'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    int idx = 0;
    while ((c = getopt_long(argc, argv, "m:i:c:qvdh", long_opts, &idx)) != -1) {
        switch (c) {
            case 'm':
                if (!parse_mode(optarg, out.mode)) {
                    std::fprintf(stderr, "[loginext] invalid --mode: %s\n", optarg);
                    return 1;
                }
                out.cli_mode_set = true;
                break;
            case 'i':
                if (!parse_bool(optarg, out.invert_hwheel)) {
                    std::fprintf(stderr, "[loginext] invalid --invert: %s\n", optarg);
                    return 1;
                }
                out.cli_invert_set = true;
                break;
            case 'c':
                out.config_path = optarg;
                break;
            case 'q':
                out.quiet = true;
                break;
            case 'v':
                out.verbose = true;
                break;
            case 'd':
                out.debug_events = true;
                break;
            case 'h':
                out.help = true;
                return 0;
            case '?':
            default:
                return 1;
        }
    }
    return 0;
}

} // namespace loginext::config
