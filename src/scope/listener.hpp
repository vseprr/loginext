#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <pthread.h>

namespace loginext::scope {

// Active-window listener — runs on its own pthread so the main epoll loop
// is never blocked by a compositor round-trip.
//
// Communication with the hot path is one-way: the thread only ever writes
// to `active_app_hash`, and only with `memory_order_relaxed` (the value is
// just an integer, not a pointer or a synchronisation handle, and the hot
// path tolerates last-tick staleness — at worst the user sees the previous
// app's preset for one event after a focus change).
//
// 0 means "no specific app focused / unknown / detector unavailable" — the
// hot-path lookup treats 0 as the global fallback.
//
// `intro_*` carry the latest *string* form of the focused window (compositor
// class / app id, plus which backend produced it) so the IPC `get_active_app`
// command can report something human-readable to the UI. They are NEVER read
// on the hot path — only on user-initiated IPC requests, which run on the
// event-loop thread but with very low frequency. The mutex contention is
// effectively zero (≤10 Hz writer, on-demand reader).
struct Listener {
    std::atomic<uint32_t> active_app_hash{0};
    std::atomic<bool>     stop{false};
    pthread_t             thread{};
    int                   wake_pipe[2]{-1, -1};   // self-pipe to break out of select()
    bool                  started = false;

    std::mutex            intro_mutex;
    char                  intro_name[128]{};      // raw class / app id (rule-key candidate)
    char                  intro_source[16]{};     // "kde-wayland" / "x11" / "hyprland" / ""
};

// Start the background detector. Returns 0 on success, -1 if the thread
// could not be created (in which case the daemon continues with all events
// resolved against the global preset). Never blocks the caller for compositor
// I/O — initial probing happens inside the thread.
[[nodiscard]] int start(Listener& l) noexcept;

// Signal the thread to exit and join it. Idempotent; safe even if start()
// failed. Called once at daemon shutdown.
void stop(Listener& l) noexcept;

} // namespace loginext::scope
