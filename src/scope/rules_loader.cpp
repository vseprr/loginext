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
        std::string_view app    = trim(line.substr(0, eq));
        std::string_view preset = trim(line.substr(eq + 1));
        if (app.empty() || preset.empty()) {
            LX_WARN("app_rules: line %zu empty key or value — ignored", line_no);
            ok = false;
            continue;
        }

        presets::PresetId pid;
        if (!presets::preset_id_from_str(preset.data(), preset.size(), pid)) {
            LX_WARN("app_rules: line %zu unknown preset '%.*s' — ignored",
                    line_no, static_cast<int>(preset.size()), preset.data());
            ok = false;
            continue;
        }

        uint32_t h = hash_app(app.data(), app.size());
        if (!insert(out, h, pid)) {
            LX_WARN("app_rules: line %zu rule table full — '%.*s' dropped",
                    line_no, static_cast<int>(app.size()), app.data());
            ok = false;
            continue;
        }
    }

    LX_INFO("app_rules: loaded %u rule(s) from %s", out.count, path.c_str());
    return ok;
}

} // namespace loginext::scope
