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

// Component tags appended to every log line in square brackets after the
// level tag, e.g. `2026-05-25T21:30:00.123 [INF] [ipc] client connected`.
// Tags are short (≤ 8 chars) so they stay aligned in grep output. Bumping
// this enum: add the new tag, append its 3-char string to component_tag()
// in log.cpp, do NOT reorder existing values (call sites pass them by
// name, but anything that ever serialised the enum int would break).
enum class LogComponent : std::uint8_t {
    General    = 0,  // fallback for existing call sites
    Core       = 1,  // event loop, device grab, uinput emitter, pacer
    Ipc        = 2,  // UDS server, dispatch, line-delimited JSON
    Scope      = 3,  // per-app focus listeners + rule lookup
    Heuristics = 4,  // scroll engine state
    Preset     = 5,  // (PresetId, Direction) → action mapping
    Config     = 6,  // CLI args + JSON config + reload
    Pacer      = 7,  // emit ring buffer + timerfd
    Kwin       = 8,  // KWin D-Bus focus bridge
    AppRules   = 9,  // app_rules.txt loader
    Daemon     = 10, // top-level daemon lifecycle (main.cpp)
    Perf       = 11, // --debug-perf counters
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
//
// Component-aware variant. Tag rendered as `[<comp>]` between the level
// tag and the message body.
void log_msg(LogLevel lvl, LogComponent comp, const char* fmt, ...) noexcept
    __attribute__((format(printf, 3, 4)));

// Backward-compatible overload — existing call sites stay unchanged.
// Internally dispatches to the component variant with LogComponent::General.
void log_msg(LogLevel lvl, const char* fmt, ...) noexcept
    __attribute__((format(printf, 2, 3)));

// Convenience wrappers — let the call site read at a glance.
#define LX_TRACE(...) ::loginext::util::log_msg(::loginext::util::LogLevel::Trace, __VA_ARGS__)
#define LX_DEBUG(...) ::loginext::util::log_msg(::loginext::util::LogLevel::Debug, __VA_ARGS__)
#define LX_INFO(...)  ::loginext::util::log_msg(::loginext::util::LogLevel::Info,  __VA_ARGS__)
#define LX_WARN(...)  ::loginext::util::log_msg(::loginext::util::LogLevel::Warn,  __VA_ARGS__)
#define LX_ERROR(...) ::loginext::util::log_msg(::loginext::util::LogLevel::Error, __VA_ARGS__)

// Component-aware variants — preferred for NEW call sites. The component
// suffix lives at the END of the macro name so call-site readability
// matches existing LX_INFO: `LX_INFO_C(Scope, "focus changed: %s", name)`.
#define LX_TRACE_C(comp, ...) ::loginext::util::log_msg(::loginext::util::LogLevel::Trace, ::loginext::util::LogComponent::comp, __VA_ARGS__)
#define LX_DEBUG_C(comp, ...) ::loginext::util::log_msg(::loginext::util::LogLevel::Debug, ::loginext::util::LogComponent::comp, __VA_ARGS__)
#define LX_INFO_C(comp, ...)  ::loginext::util::log_msg(::loginext::util::LogLevel::Info,  ::loginext::util::LogComponent::comp, __VA_ARGS__)
#define LX_WARN_C(comp, ...)  ::loginext::util::log_msg(::loginext::util::LogLevel::Warn,  ::loginext::util::LogComponent::comp, __VA_ARGS__)
#define LX_ERROR_C(comp, ...) ::loginext::util::log_msg(::loginext::util::LogLevel::Error, ::loginext::util::LogComponent::comp, __VA_ARGS__)

// Resolved log file path (zero-terminated, empty string if file sink is off).
// Stable for the lifetime of the daemon — exposed so main() can print it on
// startup so users know where to `tail -f`.
[[nodiscard]] const char* log_file_path() noexcept;

// Write a session-start banner to the log: timestamp, pid, version,
// optional active-preset, optional mode. Called once from main() after
// log_init succeeds and the config has been parsed. Stack-only, no
// allocation. Lines look like:
//   2026-05-25T21:30:00.123 [INF] [core] === loginext started v=1.1.0 pid=12345 preset=tab_nav mode=medium ===
//
// Any field passed as nullptr is omitted from the banner.
void log_session_marker(const char* version,
                        const char* active_preset,
                        const char* mode) noexcept;

} // namespace loginext::util
