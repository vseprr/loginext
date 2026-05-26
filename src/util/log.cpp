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

// Single-level log rotation. If the existing log file is larger than
// kRotateThreshold, rename it to `<path>.1` (overwriting any existing
// .1 file) before re-opening. We deliberately stay at one rotation
// level — bug reports use only the last session anyway, and adding
// more would just clutter $XDG_STATE_HOME without diagnostic value.
//
// Best-effort: any error here is logged to stderr and ignored — the
// daemon continues with the original (now-large) log. Better to have
// a bloated log than no log.
constexpr off_t kRotateThreshold = 2 * 1024 * 1024;  // 2 MiB

void maybe_rotate_log(const char* path) noexcept {
    struct stat st{};
    if (::stat(path, &st) != 0) return;          // missing — nothing to rotate
    if (st.st_size < kRotateThreshold) return;   // within budget

    char rotated[kPathCap];
    int n = std::snprintf(rotated, sizeof(rotated), "%s.1", path);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(rotated)) return;

    // Remove an older .1 if it exists. unlink is idempotent — ignore
    // ENOENT. Other errors fall through to rename which will then
    // overwrite via its own semantics.
    (void)::unlink(rotated);

    if (::rename(path, rotated) != 0) {
        std::fprintf(stderr, "[loginext] log: rotate %s → %s failed: %s\n",
                     path, rotated, std::strerror(errno));
        return;
    }
    chown_to_invoking_user(rotated);
    std::fprintf(stderr, "[loginext] log: rotated %s → %s (was %lld bytes)\n",
                 path, rotated, static_cast<long long>(st.st_size));
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

const char* component_tag(LogComponent c) noexcept {
    switch (c) {
        case LogComponent::General:    return "gen";
        case LogComponent::Core:       return "core";
        case LogComponent::Ipc:        return "ipc";
        case LogComponent::Scope:      return "scope";
        case LogComponent::Heuristics: return "heur";
        case LogComponent::Preset:     return "preset";
        case LogComponent::Config:     return "config";
        case LogComponent::Pacer:      return "pacer";
        case LogComponent::Kwin:       return "kwin";
        case LogComponent::AppRules:   return "rules";
        case LogComponent::Daemon:     return "daemon";
        case LogComponent::Perf:       return "perf";
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

    // Rotate before opening — keeps the new session's log starting fresh
    // when the previous one ballooned past the budget. Best-effort; errors
    // get logged to stderr and we proceed with the original file.
    maybe_rotate_log(g_path);

    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (g_fd < 0) {
        std::fprintf(stderr, "[loginext] log: open(%s) failed: %s\n",
                     g_path, std::strerror(errno));
        g_path[0] = '\0';
        return -1;
    }
    chown_to_invoking_user(g_path);

    // The session-start banner is emitted from main() via log_session_marker()
    // so it can include version + active preset + mode — fields not visible
    // here at log_init time.
    return 0;
}

void log_shutdown() noexcept {
    if (g_fd >= 0) {
        char ts[32];
        format_timestamp(ts, sizeof(ts));
        char banner[128];
        int bn = std::snprintf(banner, sizeof(banner),
                               "%s [INF] [core] === loginext daemon stopped ===\n", ts);
        if (bn > 0) {
            [[maybe_unused]] ssize_t w = write(g_fd, banner, static_cast<size_t>(bn));
        }
        close(g_fd);
        g_fd = -1;
    }
    g_path[0] = '\0';
}

// Internal core of log_msg. Component-aware. va_list-based so both
// public overloads can dispatch into it after their own va_start.
static void log_msg_v(LogLevel lvl, LogComponent comp,
                      const char* fmt, va_list ap) noexcept {
    const bool to_file   = g_cfg.file_enabled  && g_fd >= 0
                        && static_cast<int>(lvl) >= static_cast<int>(g_cfg.file_level);
    const bool to_stderr = g_cfg.stderr_enabled
                        && static_cast<int>(lvl) >= static_cast<int>(g_cfg.stderr_level);
    if (!to_file && !to_stderr) return;

    char ts[32];
    format_timestamp(ts, sizeof(ts));

    char body[kLineCap];
    int blen = std::vsnprintf(body, sizeof(body), fmt, ap);
    if (blen < 0) return;
    if (static_cast<size_t>(blen) >= sizeof(body)) blen = static_cast<int>(sizeof(body)) - 1;

    char line[kLineCap + 80];
    int n = std::snprintf(line, sizeof(line), "%s [%s] [%s] %s\n",
                          ts, level_tag(lvl), component_tag(comp), body);
    if (n <= 0) return;
    if (static_cast<size_t>(n) >= sizeof(line)) n = static_cast<int>(sizeof(line)) - 1;

    if (to_file) {
        [[maybe_unused]] ssize_t w = write(g_fd, line, static_cast<size_t>(n));
    }
    if (to_stderr) {
        [[maybe_unused]] ssize_t w = write(STDERR_FILENO, line, static_cast<size_t>(n));
    }
}

void log_msg(LogLevel lvl, LogComponent comp, const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    log_msg_v(lvl, comp, fmt, ap);
    va_end(ap);
}

// Backward-compatible overload — defaults to General component.
void log_msg(LogLevel lvl, const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    log_msg_v(lvl, LogComponent::General, fmt, ap);
    va_end(ap);
}

void log_session_marker(const char* version,
                        const char* active_preset,
                        const char* mode) noexcept {
    if (g_fd < 0 && !g_cfg.stderr_enabled) return;

    char ts[32];
    format_timestamp(ts, sizeof(ts));

    // Compose conditionally — nullptr fields are omitted. Stack-only.
    char banner[kLineCap];
    int n;
    const char* v  = version       ? version       : "?";
    if (active_preset && mode) {
        n = std::snprintf(banner, sizeof(banner),
            "%s [INF] [core] === loginext started v=%s pid=%d preset=%s mode=%s ===\n",
            ts, v, static_cast<int>(getpid()), active_preset, mode);
    } else if (active_preset) {
        n = std::snprintf(banner, sizeof(banner),
            "%s [INF] [core] === loginext started v=%s pid=%d preset=%s ===\n",
            ts, v, static_cast<int>(getpid()), active_preset);
    } else {
        n = std::snprintf(banner, sizeof(banner),
            "%s [INF] [core] === loginext started v=%s pid=%d ===\n",
            ts, v, static_cast<int>(getpid()));
    }
    if (n <= 0) return;
    if (static_cast<size_t>(n) >= sizeof(banner)) n = static_cast<int>(sizeof(banner)) - 1;

    if (g_fd >= 0) {
        [[maybe_unused]] ssize_t w = write(g_fd, banner, static_cast<size_t>(n));
    }
    if (g_cfg.stderr_enabled) {
        [[maybe_unused]] ssize_t w = write(STDERR_FILENO, banner, static_cast<size_t>(n));
    }
}

const char* log_file_path() noexcept {
    return g_path;
}

} // namespace loginext::util
