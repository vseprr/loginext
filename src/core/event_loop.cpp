#include "core/event_loop.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <libevdev/libevdev.h>
#include <unistd.h>

namespace loginext::core {

int init_loop(EventLoop& loop, int device_fd) noexcept {
    loop.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop.epoll_fd < 0) {
        std::fprintf(stderr, "[loginext] epoll_create1 failed: %s\n", std::strerror(errno));
        return -1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = device_fd;

    if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, device_fd, &ev) < 0) {
        std::fprintf(stderr, "[loginext] epoll_ctl failed: %s\n", std::strerror(errno));
        close(loop.epoll_fd);
        loop.epoll_fd = -1;
        return -1;
    }

    std::fprintf(stderr, "[loginext] epoll loop initialized (edge-triggered)\n");
    return 0;
}

int register_timer(EventLoop& loop, int timer_fd) noexcept {
    loop.timer_fd = timer_fd;

    epoll_event ev{};
    ev.events = EPOLLIN;  // level-triggered for timerfd (simpler, always drains)
    ev.data.fd = timer_fd;

    if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) < 0) {
        std::fprintf(stderr, "[loginext] epoll_ctl timerfd failed: %s\n", std::strerror(errno));
        return -1;
    }
    return 0;
}

int run_loop(EventLoop& loop, int device_fd, void* evdev_raw,
              volatile sig_atomic_t* stop,
              volatile sig_atomic_t* reload,
              EventCallback  event_cb,  void* event_ctx,
              TimerCallback  timer_cb,  void* timer_ctx,
              ReloadCallback reload_cb, void* reload_ctx,
              IoCallback     io_cb,     void* io_ctx,
              bool           debug_events,
              bool           debug_perf) noexcept {

    auto* evdev = static_cast<libevdev*>(evdev_raw);
    constexpr int timeout_ms = -1;
    int exit_code = 0;  // 1 if loop exits due to fatal device error

    // --debug-perf counters. epoll_wakeups counts the outer-loop iterations,
    // dev_events / timer_wakeups / io_wakeups attribute work to each fd
    // class, dev_event_drained counts individual input_events the inner
    // libevdev loop pulled out (this is where a runaway device-fd spinner
    // would show up, since edge-triggered epoll can deliver arbitrary
    // event counts per wake).
    timespec perf_last_ts{};
    if (debug_perf) clock_gettime(CLOCK_MONOTONIC, &perf_last_ts);
    uint64_t perf_epoll_wakeups   = 0;
    uint64_t perf_dev_wakeups     = 0;
    uint64_t perf_dev_events      = 0;
    uint64_t perf_timer_wakeups   = 0;
    uint64_t perf_io_wakeups      = 0;

    while (!*stop) {
        if (*reload) {
            *reload = 0;
            reload_cb(reload_ctx);
        }

        int nfds = epoll_wait(loop.epoll_fd, loop.events.data(),
                              static_cast<int>(loop.events.size()), timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR) continue;  // signal delivered — loop re-checks stop & reload
            std::fprintf(stderr, "[loginext] epoll_wait error: %s\n", std::strerror(errno));
            break;
        }

        ++perf_epoll_wakeups;

        for (int i = 0; i < nfds; ++i) {
            int fd = loop.events[static_cast<size_t>(i)].data.fd;

            if (fd == device_fd) {
                ++perf_dev_wakeups;
                // Drain all pending input events
                input_event ev{};
                unsigned flags = LIBEVDEV_READ_FLAG_NORMAL;
                int rc = 0;
                while ((rc = libevdev_next_event(evdev, flags, &ev)) >= 0) {
                    if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                        flags = LIBEVDEV_READ_FLAG_SYNC;
                        continue;
                    }
                    flags = LIBEVDEV_READ_FLAG_NORMAL;
                    ++perf_dev_events;
                    // Hardware-discovery dump. `__builtin_expect` keeps the
                    // production fall-through (debug_events==false) on the
                    // straight-line path; the fprintf is reached only when an
                    // operator explicitly asked for it.
                    if (__builtin_expect(debug_events, 0)) {
                        const char* type_name = libevdev_event_type_get_name(ev.type);
                        const char* code_name = libevdev_event_code_get_name(ev.type, ev.code);
                        std::fprintf(stderr,
                            "[debug-events] type=%s(0x%02x) code=%s(0x%03x) value=%d\n",
                            type_name ? type_name : "?", ev.type,
                            code_name ? code_name : "?", ev.code,
                            ev.value);
                    }
                    event_cb(ev, event_ctx);
                }
                if (rc != -EAGAIN && rc != -EINTR) {
                    // Fatal device error (most commonly -ENODEV on USB
                    // disconnect / suspend race). Without exiting, the
                    // edge-triggered epoll keeps firing on the dead fd
                    // POLLERR/POLLHUP forever — the libevdev_next_event
                    // call returns the same error instantly, the for-loop
                    // re-iterates immediately, and the daemon burns 100%
                    // CPU until systemd-oomd PSI-kills it. Observed in
                    // production: 11 min spin between an unplug at
                    // 20:38:31 and the user-toggle stop at 20:49:02.
                    //
                    // The fix is to remove the dead fd from epoll and
                    // exit the loop cleanly. systemd's Restart=on-failure
                    // brings us back; main.cpp's bounded find_device()
                    // retry then waits for udev to re-publish the device
                    // when it reappears (replug, resume from sleep, etc).
                    std::fprintf(stderr,
                        "[loginext] libevdev read error: %s — releasing device fd, exiting loop\n",
                        std::strerror(-rc));
                    epoll_ctl(loop.epoll_fd, EPOLL_CTL_DEL, device_fd, nullptr);
                    exit_code = 1;  // tell main.cpp to exit non-zero so
                                    // Restart=on-failure brings us back
                    *stop = 1;      // signal outer while to exit cleanly
                    break;          // skip remaining ready fds in this batch
                }
            } else if (fd == loop.timer_fd) {
                ++perf_timer_wakeups;
                timer_cb(timer_ctx);
            } else if (io_cb) {
                // Foreign fd (IPC listener / client) — dispatch to the
                // generic handler. Kept off the hot path deliberately: the
                // device and timer branches above are checked first.
                ++perf_io_wakeups;
                io_cb(fd, io_ctx);
            }
        }

        // --debug-perf: emit a one-line summary every ≥ 1 s. Reset cadence
        // is "elapsed", not strict periodic, so a quiet second can be
        // followed by a busy second without losing counts. The summary
        // attributes work to fd class so a CPU spinner is immediately
        // bisected: high `dev_events` with no user input → device fd
        // misbehaving, high `timer_wakeups` → pacer state machine wedged,
        // high `io_wakeups` → IPC client looping.
        if (debug_perf) {
            timespec perf_now{};
            clock_gettime(CLOCK_MONOTONIC, &perf_now);
            uint64_t since_us =
                (static_cast<uint64_t>(perf_now.tv_sec - perf_last_ts.tv_sec) * 1'000'000ULL)
              + (static_cast<uint64_t>(perf_now.tv_nsec) / 1'000ULL)
              - (static_cast<uint64_t>(perf_last_ts.tv_nsec) / 1'000ULL);
            if (since_us >= 1'000'000ULL) {
                std::fprintf(stderr,
                    "[loginext] perf[main]: %lu epoll wakeups, "
                    "%lu dev wakeups (%lu input_events), "
                    "%lu timer wakeups, %lu io wakeups in %.2fs\n",
                    static_cast<unsigned long>(perf_epoll_wakeups),
                    static_cast<unsigned long>(perf_dev_wakeups),
                    static_cast<unsigned long>(perf_dev_events),
                    static_cast<unsigned long>(perf_timer_wakeups),
                    static_cast<unsigned long>(perf_io_wakeups),
                    static_cast<double>(since_us) / 1'000'000.0);
                perf_epoll_wakeups = 0;
                perf_dev_wakeups   = 0;
                perf_dev_events    = 0;
                perf_timer_wakeups = 0;
                perf_io_wakeups    = 0;
                perf_last_ts       = perf_now;
            }
        }
    }

    std::fprintf(stderr, "[loginext] event loop exiting\n");
    return exit_code;
}

void shutdown_loop(EventLoop& loop) noexcept {
    if (loop.epoll_fd >= 0) {
        close(loop.epoll_fd);
        loop.epoll_fd = -1;
    }
}

} // namespace loginext::core
