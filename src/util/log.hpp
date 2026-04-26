#pragma once

// Tiny stderr/file logger. Two sinks:
//   - stderr: lean by default — boot banner, fatal errors, lifecycle. The UI
//     parses these from the daemon process when it owns the spawn.
//   - file:   detailed, ring-overwritten log at $XDG_STATE_HOME/loginext/daemon.log
//     (created lazily). Runtime-only — no startup cost if the daemon is
//     launched without the file sink.
//
// Design:
//   - All routines are noexcept and async-signal-unsafe (use plain printf
//     buffers). Never call from a signal handler.
//   - No heap allocation after init (the log fd + path live for the daemon's
//     lifetime). Format buffer is stack-resident (1 KiB).
//   - Levels gate the file sink only — stderr always sees ≥ Info.

#include <cstddef>
#include <cstdint>

namespace loginext::util {

enum class LogLevel : std::uint8_t {
    Trace = 0,  // every event tick, every emit — file sink only
    Debug = 1,  // state transitions, IPC traffic — file sink only
    Info  = 2,  // boot, reload, device attach/detach — file + stderr (if not muted)
    Warn  = 3,  // recoverable errors — file + stderr always
    Error = 4,  // fatal — file + stderr always
};

struct LogConfig {
    bool        file_enabled    = true;   // open daemon.log on init
    bool        stderr_enabled  = true;   // write Info+ to stderr
    LogLevel    file_level      = LogLevel::Debug;
    LogLevel    stderr_level    = LogLevel::Info;
};

// Initialise the logger. Resolves $XDG_STATE_HOME/loginext/daemon.log (or
// $HOME/.local/state/loginext/daemon.log fallback), creates the directory if
// missing, and opens the file in O_APPEND | O_CREAT mode. Idempotent — calling
// twice keeps the existing fd.
//
// Returns 0 on success (file sink open), -1 if the file sink failed (stderr
// sink still functional).
int log_init(const LogConfig& cfg) noexcept;

// Close the file fd. Safe to call from teardown; subsequent log calls fall
// back to stderr only.
void log_shutdown() noexcept;

// printf-style log entry. The first argument is the level — anything below
// the configured threshold for a sink is dropped before the format runs.
void log_msg(LogLevel lvl, const char* fmt, ...) noexcept
    __attribute__((format(printf, 2, 3)));

// Convenience wrappers — let the call site read at a glance.
#define LX_TRACE(...) ::loginext::util::log_msg(::loginext::util::LogLevel::Trace, __VA_ARGS__)
#define LX_DEBUG(...) ::loginext::util::log_msg(::loginext::util::LogLevel::Debug, __VA_ARGS__)
#define LX_INFO(...)  ::loginext::util::log_msg(::loginext::util::LogLevel::Info,  __VA_ARGS__)
#define LX_WARN(...)  ::loginext::util::log_msg(::loginext::util::LogLevel::Warn,  __VA_ARGS__)
#define LX_ERROR(...) ::loginext::util::log_msg(::loginext::util::LogLevel::Error, __VA_ARGS__)

// Resolved log file path (zero-terminated, empty string if file sink is off).
// Stable for the lifetime of the daemon — exposed so main() can print it on
// startup so users know where to `tail -f`.
[[nodiscard]] const char* log_file_path() noexcept;

} // namespace loginext::util
