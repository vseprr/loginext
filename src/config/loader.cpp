#include "config/loader.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <string_view>
#include <unistd.h>

namespace loginext::config {

std::string default_config_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/loginext/config.json";
    }

    // Under sudo, $HOME points to /root. Resolve the invoking user's
    // home from the password database so the daemon and UI agree.
    const char* sudo_uid = std::getenv("SUDO_UID");
    if (sudo_uid && *sudo_uid) {
        uid_t uid = static_cast<uid_t>(std::strtoul(sudo_uid, nullptr, 10));
        if (struct passwd* pw = getpwuid(uid)) {
            return std::string(pw->pw_dir) + "/.config/loginext/config.json";
        }
    }

    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/loginext/config.json";
    }
    return {};
}

namespace {

// Minimal JSON parser — supports a flat object with string and bool values.
// No escapes, no numbers, no nesting. Whitespace tolerant.
class Parser {
public:
    explicit Parser(std::string_view s) : src_(s) {}

    bool parse(Settings& out) {
        skip_ws();
        if (!consume('{')) return fail("expected '{'");

        skip_ws();
        if (peek() == '}') { ++pos_; return true; }  // empty object

        while (true) {
            skip_ws();
            std::string_view key;
            if (!parse_string_view(key)) return false;
            skip_ws();
            if (!consume(':')) return fail("expected ':'");
            skip_ws();
            if (!apply_value(key, out)) return false;
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return fail("expected ',' or '}'");
        }
    }

private:
    bool apply_value(std::string_view key, Settings& out) {
        if (key == "sensitivity") {
            std::string_view v;
            if (!parse_string_view(v)) return false;
            if      (v == "low")    out.mode = SensitivityMode::Low;
            else if (v == "medium") out.mode = SensitivityMode::Medium;
            else if (v == "high")   out.mode = SensitivityMode::High;
            else return fail("sensitivity must be 'low', 'medium', or 'high'");
            return true;
        }
        if (key == "invert_hwheel") {
            bool b;
            if (!parse_bool(b)) return false;
            out.invert_hwheel = b;
            return true;
        }
        if (key == "active_preset") {
            std::string_view v;
            if (!parse_string_view(v)) return false;
            presets::PresetId id;
            if (!presets::preset_id_from_str(v.data(), v.size(), id)) {
                return fail("unknown preset id");
            }
            out.active_preset = id;
            return true;
        }
        // Unknown key — consume the value silently for forward compat.
        return skip_value();
    }

    bool parse_string_view(std::string_view& out) {
        if (!consume('"')) return fail("expected string");
        size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\\') return fail("escape sequences not supported");
            ++pos_;
        }
        out = src_.substr(start, pos_ - start);
        if (!consume('"')) return fail("unterminated string");
        return true;
    }

    bool parse_bool(bool& out) {
        if (match("true"))  { out = true;  return true; }
        if (match("false")) { out = false; return true; }
        return fail("expected true/false");
    }

    bool skip_value() {
        if (peek() == '"')  { std::string_view tmp; return parse_string_view(tmp); }
        bool dummy;
        if (peek() == 't' || peek() == 'f') return parse_bool(dummy);
        return fail("unsupported value type");
    }

    bool match(std::string_view lit) {
        if (src_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    bool consume(char c) {
        if (peek() == c) { ++pos_; return true; }
        return false;
    }

    char peek() const {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }

    void skip_ws() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }

    bool fail(const char* msg) {
        std::fprintf(stderr, "[loginext] config parse error at byte %zu: %s\n", pos_, msg);
        return false;
    }

    std::string_view src_;
    size_t           pos_ = 0;
};

} // namespace

bool load_settings(const std::string& path, Settings& s) noexcept {
    if (path.empty()) return false;

    // Stack-based I/O — no heap allocation on the reload path.
    // Config file is a small flat JSON; 4 KiB is more than sufficient.
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        // Missing file is not an error — silent return, caller keeps defaults.
        return false;
    }

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) return false;
    buf[n] = '\0';

    Settings tmp = s;  // parse into a scratch copy; only commit on full success
    Parser   p(std::string_view(buf, static_cast<size_t>(n)));
    if (!p.parse(tmp)) {
        std::fprintf(stderr, "[loginext] config %s: parse failed, keeping previous settings\n",
                     path.c_str());
        return false;
    }

    s = tmp;
    apply_mode(s);
    return true;
}

} // namespace loginext::config

