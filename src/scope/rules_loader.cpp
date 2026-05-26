#include "scope/rules_loader.hpp"

#include "scope/app_hash.hpp"
#include "util/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

namespace loginext::scope {

std::string default_rules_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/loginext/app_rules.txt";
    }
    const char* sudo_uid = std::getenv("SUDO_UID");
    if (sudo_uid && *sudo_uid) {
        uid_t uid = static_cast<uid_t>(std::strtoul(sudo_uid, nullptr, 10));
        if (struct passwd* pw = getpwuid(uid)) {
            return std::string(pw->pw_dir) + "/.config/loginext/app_rules.txt";
        }
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/loginext/app_rules.txt";
    }
    return {};
}

namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

} // namespace

bool load_rules(const std::string& path, RuleTable& out) noexcept {
    clear(out);
    if (path.empty()) return true;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return true;  // missing file → all-global, not an error

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return true;
    buf[n] = '\0';

    std::string_view src(buf, static_cast<std::size_t>(n));
    std::size_t      pos = 0, line_no = 0;
    bool             ok = true;

    // Helper: split a value-side string on commas. The format is
    //   app=preset[,mode[,invert]]
    // where any of preset/mode/invert may be empty (e.g. `code=,high,`
    // overrides only the sensitivity). We allow up to three fields and
    // ignore anything past them — keeps the parser forward-compatible
    // with future per-app overrides without rev'ing the file format.
    auto split_csv = [](std::string_view v,
                        std::string_view (&out)[3]) -> std::size_t {
        std::size_t i = 0;
        std::size_t n = 0;
        while (n < 3) {
            std::size_t comma = v.find(',', i);
            std::size_t end   = (comma == std::string_view::npos) ? v.size() : comma;
            out[n++] = trim(v.substr(i, end - i));
            if (comma == std::string_view::npos) break;
            i = comma + 1;
        }
        return n;
    };

    while (pos < src.size()) {
        ++line_no;
        std::size_t end = src.find('\n', pos);
        if (end == std::string_view::npos) end = src.size();
        std::string_view line = trim(src.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line.front() == '#') continue;

        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            LX_WARN("app_rules: line %zu missing '=' — ignored", line_no);
            ok = false;
            continue;
        }
        std::string_view app   = trim(line.substr(0, eq));
        std::string_view value = trim(line.substr(eq + 1));
        if (app.empty()) {
            LX_WARN("app_rules: line %zu empty key — ignored", line_no);
            ok = false;
            continue;
        }

        std::string_view fields[3]{};
        std::size_t nfields = split_csv(value, fields);
        std::string_view preset_s = nfields > 0 ? fields[0] : std::string_view{};
        std::string_view mode_s   = nfields > 1 ? fields[1] : std::string_view{};
        std::string_view invert_s = nfields > 2 ? fields[2] : std::string_view{};

        // An empty preset means "tracked but not active" — the UI keeps
        // the chip in the context bar so the user can re-bind a preset
        // later, but the daemon has no rule to apply, so we skip the
        // table insert. The mode/invert overrides on the same line are
        // metadata for the UI; the daemon doesn't need them when the
        // preset is unset.
        if (preset_s.empty()) {
            LX_TRACE("app_rules: line %zu '%.*s' tracked-only (no preset) — skipping table insert",
                     line_no, static_cast<int>(app.size()), app.data());
            continue;
        }

        presets::PresetId pid;
        if (!presets::preset_id_from_str(preset_s.data(), preset_s.size(), pid)) {
            LX_WARN("app_rules: line %zu unknown preset '%.*s' — ignored",
                    line_no, static_cast<int>(preset_s.size()), preset_s.data());
            ok = false;
            continue;
        }

        config::SensitivityMode mode = config::SensitivityMode::Inherit;
        if (!config::mode_from_str(mode_s.data(), mode_s.size(), mode)) {
            LX_WARN("app_rules: line %zu unknown sensitivity '%.*s' — using inherit",
                    line_no, static_cast<int>(mode_s.size()), mode_s.data());
            ok = false;
            mode = config::SensitivityMode::Inherit;
        }

        int8_t invert = invert_inherit;
        if (invert_s.empty()) {
            invert = invert_inherit;
        } else if (invert_s == "true"  || invert_s == "1") {
            invert = invert_on;
        } else if (invert_s == "false" || invert_s == "0") {
            invert = invert_off;
        } else if (invert_s != "inherit") {
            LX_WARN("app_rules: line %zu unknown invert flag '%.*s' — using inherit",
                    line_no, static_cast<int>(invert_s.size()), invert_s.data());
            ok = false;
        }

        uint32_t h = hash_app(app.data(), app.size());
        if (!insert(out, h, pid, mode, invert)) {
            LX_WARN("app_rules: line %zu rule table full — '%.*s' dropped",
                    line_no, static_cast<int>(app.size()), app.data());
            ok = false;
            continue;
        }
    }

    LX_INFO_C(AppRules, "loaded %u rule(s) from %s", out.count, path.c_str());
    return ok;
}

} // namespace loginext::scope
