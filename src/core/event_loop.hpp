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
// Runs until *stop becomes true (set by SIGINT/SIGTERM handler).
// *reload is consumed (reset to 0) after each reload_cb invocation.
void run_loop(EventLoop& loop, int device_fd, void* evdev,
              volatile sig_atomic_t* stop,
              volatile sig_atomic_t* reload,
              EventCallback  event_cb,  void* event_ctx,
              TimerCallback  timer_cb,  void* timer_ctx,
              ReloadCallback reload_cb, void* reload_ctx) noexcept;

// Close the epoll fd.
void shutdown_loop(EventLoop& loop) noexcept;

} // namespace loginext::core
