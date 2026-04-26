#include "util/log.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace loginext::util {

namespace {

constexpr size_t kPathCap = 256;
constexpr size_t kLineCap = 1024;

int      g_fd               = -1;
char     g_path[kPathCap]   = {};
LogConfig g_cfg{};

// Resolve the state directory (XDG_STATE_HOME → $HOME/.local/state). Under
// sudo we trust SUDO_UID's passwd entry so the log lands in the invoking
// user's home, not /root. Returns true on success and writes a zero-terminated
// path of length < kPathCap into `out`.
bool resolve_state_dir(char* out, size_t cap) noexcept {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        int n = std::snprintf(out, cap, "%s/loginext", xdg);
        return n > 0 && static_cast<size_t>(n) < cap;
    }
    const char* home = nullptr;

    if (const char* sudo_uid = std::getenv("SUDO_UID"); sudo_uid && *sudo_uid) {
        uid_t uid = static_cast<uid_t>(std::strtoul(sudo_uid, nullptr, 10));
        if (struct passwd* pw = getpwuid(uid)) home = pw->pw_dir;
    }
    if (!home) home = std::getenv("HOME");
    if (!home || !*home) return false;

    int n = std::snprintf(out, cap, "%s/.local/state/loginext", home);
    return n > 0 && static_cast<size_t>(n) < cap;
}

// mkdir -p for a single subdir under an existing parent. Best-effort — failure
// only matters if the eventual open() fails, so we don't surface intermediate
// errors.
void ensure_dir(const char* path) noexcept {
    // Walk component-by-component, skipping the leading "/".
    char buf[kPathCap];
    size_t len = std::strlen(path);
    if (len >= sizeof(buf)) return;
    std::memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            mkdir(buf, 0700);
            buf[i] = '/';
        }
    }
    mkdir(buf, 0700);
}

// Under sudo, chown the log path tree to the invoking user so they can read
// it without root. Best-effort — silent on failure.
void chown_to_invoking_user(const char* path) noexcept {
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");
    if (!sudo_uid || !*sudo_uid) return;

    uid_t uid = static_cast<uid_t>(std::strtoul(sudo_uid, nullptr, 10));
    gid_t gid = sudo_gid && *sudo_gid
              ? static_cast<gid_t>(std::strtoul(sudo_gid, nullptr, 10))
              : uid;
    [[maybe_unused]] int r = chown(path, uid, gid);
}

const char* level_tag(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRC";
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info:  return "INF";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Error: return "ERR";
    }
    return "???";
}

// Format YYYY-MM-DDTHH:MM:SS.mmm into `out` (capacity ≥ 24).
void format_timestamp(char* out, size_t cap) noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tmv{};
    localtime_r(&ts.tv_sec, &tmv);
    std::snprintf(out, cap,
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03ld",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  ts.tv_nsec / 1'000'000);
}

} // namespace

int log_init(const LogConfig& cfg) noexcept {
    g_cfg = cfg;

    if (!cfg.file_enabled) {
        g_path[0] = '\0';
        return 0;
    }

    char dir[kPathCap];
    if (!resolve_state_dir(dir, sizeof(dir))) {
        std::fprintf(stderr, "[loginext] log: cannot resolve state dir (no XDG_STATE_HOME, no HOME)\n");
        g_path[0] = '\0';
        return -1;
    }

    ensure_dir(dir);
    chown_to_invoking_user(dir);

    int n = std::snprintf(g_path, sizeof(g_path), "%s/daemon.log", dir);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(g_path)) {
        g_path[0] = '\0';
        return -1;
    }

    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (g_fd < 0) {
        std::fprintf(stderr, "[loginext] log: open(%s) failed: %s\n",
                     g_path, std::strerror(errno));
        g_path[0] = '\0';
        return -1;
    }
    chown_to_invoking_user(g_path);

    // Boot marker — easy to grep when the user reports "what happened at
    // 14:32?". Goes to file only; the stderr boot banner lives in main.cpp.
    char ts[32];
    format_timestamp(ts, sizeof(ts));
    char banner[256];
    int bn = std::snprintf(banner, sizeof(banner),
                           "%s [INF] === loginext daemon started, pid=%d ===\n",
                           ts, static_cast<int>(getpid()));
    if (bn > 0) {
        [[maybe_unused]] ssize_t w = write(g_fd, banner, static_cast<size_t>(bn));
    }
    return 0;
}

void log_shutdown() noexcept {
    if (g_fd >= 0) {
        char ts[32];
        format_timestamp(ts, sizeof(ts));
        char banner[128];
        int bn = std::snprintf(banner, sizeof(banner),
                               "%s [INF] === loginext daemon stopped ===\n", ts);
        if (bn > 0) {
            [[maybe_unused]] ssize_t w = write(g_fd, banner, static_cast<size_t>(bn));
        }
        close(g_fd);
        g_fd = -1;
    }
    g_path[0] = '\0';
}

void log_msg(LogLevel lvl, const char* fmt, ...) noexcept {
    const bool to_file   = g_cfg.file_enabled  && g_fd >= 0
                        && static_cast<int>(lvl) >= static_cast<int>(g_cfg.file_level);
    const bool to_stderr = g_cfg.stderr_enabled
                        && static_cast<int>(lvl) >= static_cast<int>(g_cfg.stderr_level);
    if (!to_file && !to_stderr) return;

    char ts[32];
    format_timestamp(ts, sizeof(ts));

    // Single render — write the same buffer to both sinks.
    char body[kLineCap];
    va_list ap;
    va_start(ap, fmt);
    int blen = std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (blen < 0) return;
    if (static_cast<size_t>(blen) >= sizeof(body)) blen = static_cast<int>(sizeof(body)) - 1;

    char line[kLineCap + 64];
    int n = std::snprintf(line, sizeof(line), "%s [%s] %s\n",
                          ts, level_tag(lvl), body);
    if (n <= 0) return;
    if (static_cast<size_t>(n) >= sizeof(line)) n = static_cast<int>(sizeof(line)) - 1;

    if (to_file) {
        [[maybe_unused]] ssize_t w = write(g_fd, line, static_cast<size_t>(n));
    }
    if (to_stderr) {
        [[maybe_unused]] ssize_t w = write(STDERR_FILENO, line, static_cast<size_t>(n));
    }
}

const char* log_file_path() noexcept {
    return g_path;
}

} // namespace loginext::util
