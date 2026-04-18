#include "config/loader.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

namespace loginext::config {

std::string default_config_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/loginext/config.json";
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
            std::string key;
            if (!parse_string(key)) return false;
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
    bool apply_value(const std::string& key, Settings& out) {
        if (key == "sensitivity") {
            std::string v;
            if (!parse_string(v)) return false;
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
        // Unknown key — consume the value silently for forward compat.
        return skip_value();
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return fail("expected string");
        out.clear();
        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\\') return fail("escape sequences not supported");
            out.push_back(src_[pos_++]);
        }
        if (!consume('"')) return fail("unterminated string");
        return true;
    }

    bool parse_bool(bool& out) {
        if (match("true"))  { out = true;  return true; }
        if (match("false")) { out = false; return true; }
        return fail("expected true/false");
    }

    bool skip_value() {
        if (peek() == '"')  { std::string tmp; return parse_string(tmp); }
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

    std::ifstream f(path);
    if (!f.is_open()) {
        // Missing file is not an error — silent return, caller keeps defaults.
        return false;
    }

    std::stringstream buf;
    buf << f.rdbuf();
    std::string content = buf.str();

    Settings tmp = s;  // parse into a scratch copy; only commit on full success
    Parser   p(content);
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
