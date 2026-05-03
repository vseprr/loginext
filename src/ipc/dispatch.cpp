#include "ipc/dispatch.hpp"

#include "config/profile.hpp"
#include "presets/preset.hpp"
#include "scope/listener.hpp"
#include "scope/rules.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unistd.h>

namespace loginext::ipc {

namespace {

// Return a string_view onto the value of the first string-valued "<key>"
// pair found in `line` (no escapes — matches the config/loader.cpp grammar).
// Empty on miss.
std::string_view extract_string(std::string_view line, std::string_view key) noexcept {
    // Compose the pattern inline: "<key>" — scan for it byte-wise.
    // For the schemas we care about the whole line is short (<1 KiB), so a
    // linear memmem-style scan is fine.
    char needle[64];
    int  n = std::snprintf(needle, sizeof(needle), "\"%.*s\"",
                           static_cast<int>(key.size()), key.data());
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return {};

    size_t pos = line.find(std::string_view(needle, static_cast<size_t>(n)));
    if (pos == std::string_view::npos) return {};
    pos += static_cast<size_t>(n);

    // Skip whitespace and ':' between key and value.
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos >= line.size() || line[pos] != ':') return {};
    ++pos;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos >= line.size() || line[pos] != '"') return {};
    ++pos;

    size_t start = pos;
    while (pos < line.size() && line[pos] != '"') ++pos;
    if (pos >= line.size()) return {};
    return line.substr(start, pos - start);
}

int write_err(char* resp, size_t cap, const char* code) noexcept {
    int n = std::snprintf(resp, cap, "{\"ok\":false,\"err\":\"%s\"}", code);
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : -1;
}

int handle_ping(char* resp, size_t cap) noexcept {
    int n = std::snprintf(resp, cap, "{\"ok\":true,\"v\":1}");
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : -1;
}

int handle_get_settings(char* resp, size_t cap,
                        const loginext::config::Settings& s) noexcept {
    int n = std::snprintf(resp, cap,
        "{\"ok\":true,\"mode\":\"%s\",\"invert_hwheel\":%s,"
        "\"active_preset\":\"%s\"}",
        loginext::config::mode_name(s.mode),
        s.invert_hwheel ? "true" : "false",
        loginext::presets::preset_id_str(s.active_preset));
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : -1;
}

int handle_list_devices(char* resp, size_t cap) noexcept {
    // Phase 2 scope: a single hard-coded device. Phase 3 will populate this
    // dynamically from core::find_device() + libevdev introspection.
    int n = std::snprintf(resp, cap,
        "{\"ok\":true,\"devices\":["
          "{\"id\":\"mx_master_3s\",\"name\":\"MX Master 3S\"}"
        "]}");
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : -1;
}

int handle_list_controls(char* resp, size_t cap) noexcept {
    int n = std::snprintf(resp, cap,
        "{\"ok\":true,\"controls\":["
          "{\"id\":\"thumb_wheel\",\"name\":\"Thumb Wheel\",\"kind\":\"wheel_h\"}"
        "]}");
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : -1;
}

int handle_list_presets(char* resp, size_t cap,
                        const loginext::config::Settings& s) noexcept {
    // The body is built from the preset table so adding a new PresetId
    // automatically widens this listing — no string surgery here.
    char body[512];
    int  bn = 0;
    for (uint8_t i = 0; i < loginext::presets::preset_count; ++i) {
        auto id = static_cast<loginext::presets::PresetId>(i);
        int w = std::snprintf(body + bn, sizeof(body) - static_cast<size_t>(bn),
                              "%s{\"id\":\"%s\",\"name\":\"%s\"}",
                              i == 0 ? "" : ",",
                              loginext::presets::preset_id_str(id),
                              loginext::presets::preset_name(id));
        if (w <= 0 || static_cast<size_t>(bn + w) >= sizeof(body)) return -1;
        bn += w;
    }
    int n = std::snprintf(resp, cap,
        "{\"ok\":true,\"presets\":[%.*s],\"active\":\"%s\"}",
        bn, body,
        loginext::presets::preset_id_str(s.active_preset));
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : -1;
}

// Reload is deferred: set the flag, stash the client fd for a post-reload
// ack, and return 0 ("no response now"). The event loop will call
// send_reload_ack() after the config is actually live.
int handle_reload(DispatchCtx* ctx, int client_fd) noexcept {
    if (ctx->reload_flag) *ctx->reload_flag = 1;
    ctx->reload_pending_fd = client_fd;
    return 0;
}

// Minimal JSON-string escape: only " and \ need encoding for our payload
// shape, control chars are dropped (they should never appear in a window
// class / app id, and silently dropping them is preferable to producing
// invalid JSON that crashes the UI's parser). `dst_cap` includes the
// trailing NUL.
size_t json_escape_into(char* dst, size_t dst_cap, const char* src) noexcept {
    if (dst_cap == 0) return 0;
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 2 < dst_cap; ++i) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
            dst[out++] = static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x7f) {
            dst[out++] = static_cast<char>(c);
        }
    }
    dst[out] = '\0';
    return out;
}

// Reports the currently focused window as the listener thread last saw it,
// plus which rule (if any) the hot path would resolve against. Useful for
// the future per-app rule editor in the UI ("currently focused: Firefox →
// tab_nav") and for the live debug HUD.
//
// If the scope listener never came up, returns hash=0 / rule_matched=false
// / source="none" — the UI can render that as "active-window detection
// disabled" without special-casing an error.
int handle_get_active_app(char* resp, size_t cap, DispatchCtx* dc) noexcept {
    uint32_t hash    = 0;
    char     name[128] = {0};
    char     source[16] = {0};

    if (dc->scope) {
        hash = dc->scope->active_app_hash.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(dc->scope->intro_mutex);
        std::strncpy(name,   dc->scope->intro_name,   sizeof(name)   - 1);
        std::strncpy(source, dc->scope->intro_source, sizeof(source) - 1);
    } else {
        std::strncpy(source, "none", sizeof(source) - 1);
    }

    loginext::presets::PresetId effective = dc->settings->active_preset;
    bool matched = false;
    if (dc->rules) {
        loginext::presets::PresetId override_id;
        if (loginext::scope::lookup(*dc->rules, hash, override_id)) {
            effective = override_id;
            matched   = true;
        }
    }

    char name_esc[256];
    char src_esc[32];
    json_escape_into(name_esc, sizeof(name_esc), name);
    json_escape_into(src_esc,  sizeof(src_esc),  source);

    int n = std::snprintf(resp, cap,
        "{\"ok\":true,\"hash\":\"0x%08x\",\"name\":\"%s\",\"source\":\"%s\","
        "\"preset\":\"%s\",\"global_preset\":\"%s\",\"rule_matched\":%s}",
        hash,
        name_esc,
        src_esc,
        loginext::presets::preset_id_str(effective),
        loginext::presets::preset_id_str(dc->settings->active_preset),
        matched ? "true" : "false");
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : -1;
}

} // namespace

int dispatch(const char* line, size_t len,
             char* resp, size_t resp_cap,
             void* ctx) noexcept {
    return dispatch_with_fd(line, len, resp, resp_cap, ctx, -1);
}

int dispatch_with_fd(const char* line, size_t len,
                     char* resp, size_t resp_cap,
                     void* ctx, int client_fd) noexcept {
    auto* dc = static_cast<DispatchCtx*>(ctx);
    std::string_view sv(line, len);

    // Strip a trailing '\r' so the protocol tolerates CRLF clients.
    if (!sv.empty() && sv.back() == '\r') sv.remove_suffix(1);
    if (sv.empty()) return 0;  // keep-alive blank line — no response

    std::string_view cmd = extract_string(sv, "cmd");
    if (cmd.empty())           return write_err(resp, resp_cap, "bad_request");

    if (cmd == "ping")           return handle_ping(resp, resp_cap);
    if (cmd == "get_settings")   return handle_get_settings(resp, resp_cap, *dc->settings);
    if (cmd == "list_devices")   return handle_list_devices(resp, resp_cap);
    if (cmd == "list_controls")  return handle_list_controls(resp, resp_cap);
    if (cmd == "list_presets")   return handle_list_presets(resp, resp_cap, *dc->settings);
    if (cmd == "reload")         return handle_reload(dc, client_fd);
    if (cmd == "get_active_app") return handle_get_active_app(resp, resp_cap, dc);

    return write_err(resp, resp_cap, "unknown_command");
}

void send_reload_ack(DispatchCtx& ctx, bool success) noexcept {
    if (ctx.reload_pending_fd < 0) return;

    char buf[128];
    int n;
    if (success) {
        n = std::snprintf(buf, sizeof(buf), "{\"ok\":true}\n");
    } else {
        n = std::snprintf(buf, sizeof(buf),
                         "{\"ok\":false,\"err\":\"reload_failed\"}\n");
    }
    if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
        [[maybe_unused]] ssize_t w = write(ctx.reload_pending_fd, buf,
                                           static_cast<size_t>(n));
    }
    ctx.reload_pending_fd = -1;
}

} // namespace loginext::ipc
