#include "core/event_loop.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
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

void run_loop(EventLoop& loop, int device_fd, void* evdev_raw,
              volatile sig_atomic_t* stop,
              volatile sig_atomic_t* reload,
              EventCallback  event_cb,  void* event_ctx,
              TimerCallback  timer_cb,  void* timer_ctx,
              ReloadCallback reload_cb, void* reload_ctx) noexcept {

    auto* evdev = static_cast<libevdev*>(evdev_raw);
    constexpr int timeout_ms = -1;

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

        for (int i = 0; i < nfds; ++i) {
            int fd = loop.events[static_cast<size_t>(i)].data.fd;

            if (fd == device_fd) {
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
                    event_cb(ev, event_ctx);
                }
                if (rc != -EAGAIN && rc != -EINTR) {
                    std::fprintf(stderr, "[loginext] libevdev read error: %s\n", std::strerror(-rc));
                }
            } else if (fd == loop.timer_fd) {
                timer_cb(timer_ctx);
            }
        }
    }

    std::fprintf(stderr, "[loginext] event loop exiting\n");
}

void shutdown_loop(EventLoop& loop) noexcept {
    if (loop.epoll_fd >= 0) {
        close(loop.epoll_fd);
        loop.epoll_fd = -1;
    }
}

} // namespace loginext::core
