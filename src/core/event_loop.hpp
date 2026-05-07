#pragma once

#include "config/constants.hpp"

#include <array>
#include <csignal>
#include <linux/input.h>
#include <sys/epoll.h>

namespace loginext::core {

// Zero-overhead callback: function pointer + opaque context, no std::function
using EventCallback  = void(*)(const input_event& ev, void* ctx);
using TimerCallback  = void(*)(void* ctx);
using ReloadCallback = void(*)(void* ctx);
// Catch-all for fds the event loop itself does not own (e.g. IPC listener,
// IPC client sockets). Invoked once per epoll wake-up that mentions `fd`.
using IoCallback     = void(*)(int fd, void* ctx);

struct EventLoop {
    int epoll_fd = -1;
    int timer_fd = -1;  // pacer timerfd, registered alongside device fd
    std::array<epoll_event, config::max_epoll_events> events{};
};

// Create epoll instance and register the device fd (edge-triggered).
[[nodiscard]] int init_loop(EventLoop& loop, int device_fd) noexcept;

// Register the pacer's timerfd in the same epoll instance.
[[nodiscard]] int register_timer(EventLoop& loop, int timer_fd) noexcept;

// Block on epoll, drain libevdev events, dispatch through callbacks.
// Runs until *stop becomes true (set by SIGINT/SIGTERM handler) or a fatal
// device error (e.g. -ENODEV on USB unplug) forces an early exit.
// *reload is consumed (reset to 0) after each reload_cb invocation.
// `debug_events` is a hardware-discovery flag: when true, every raw input_event
// drained from libevdev is dumped to stderr before normal dispatch. The check
// is a single predicted-not-taken branch; production runs (default false) pay
// nothing measurable.
//
// Return value:
//   0 — clean stop (signal-driven OR cooperative IPC `quit`).
//   1 — fatal device error (caller should exit non-zero so systemd's
//       Restart=on-failure brings the daemon back, then the bounded
//       find_device() retry in main.cpp waits for udev to re-publish
//       the device on replug / resume-from-sleep).
[[nodiscard]] int run_loop(EventLoop& loop, int device_fd, void* evdev,
              volatile sig_atomic_t* stop,
              volatile sig_atomic_t* reload,
              EventCallback  event_cb,  void* event_ctx,
              TimerCallback  timer_cb,  void* timer_ctx,
              ReloadCallback reload_cb, void* reload_ctx,
              IoCallback     io_cb,     void* io_ctx,
              bool           debug_events,
              bool           debug_perf  = false) noexcept;

// Close the epoll fd.
void shutdown_loop(EventLoop& loop) noexcept;

} // namespace loginext::core
