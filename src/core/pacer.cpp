#include "core/pacer.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>

namespace loginext::core {

namespace {

void arm_timer(const PacingQueue& q) noexcept {
    itimerspec ts{};
    const int64_t ns = q.profile->pacing_interval_ns;
    ts.it_value.tv_sec  = ns / 1'000'000'000LL;
    ts.it_value.tv_nsec = ns % 1'000'000'000LL;
    timerfd_settime(q.timer_fd, 0, &ts, nullptr);
}

void disarm_timer(int fd) noexcept {
    itimerspec ts{};
    timerfd_settime(fd, 0, &ts, nullptr);
}

} // namespace

int init_pacer(PacingQueue& q) noexcept {
    q.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (q.timer_fd < 0) {
        std::fprintf(stderr, "[loginext] timerfd_create failed: %s\n", std::strerror(errno));
        return -1;
    }
    std::fprintf(stderr, "[loginext] pacer initialized (timerfd=%d)\n", q.timer_fd);
    return 0;
}

void enqueue_action(PacingQueue& q, loginext::heuristics::ActionResult action,
                    int64_t now_ns) noexcept {
    q.last_input_ns = now_ns;

    if (q.count >= config::max_queued_actions) {
        // Queue full — drop oldest to make room (prevents stale buildup)
        q.head = static_cast<uint8_t>((q.head + 1) % config::max_queued_actions);
        q.count--;
    }

    q.buffer[q.tail] = action;
    q.tail = static_cast<uint8_t>((q.tail + 1) % config::max_queued_actions);
    q.count++;

    if (q.count == 1) {
        arm_timer(q);
    }
}

void process_timer(PacingQueue& q, EmitterHandle& em) noexcept {
    uint64_t expirations = 0;
    [[maybe_unused]] auto r = read(q.timer_fd, &expirations, sizeof(expirations));

    if (q.count == 0) {
        disarm_timer(q.timer_fd);
        return;
    }

    auto action = q.buffer[q.head];
    q.head = static_cast<uint8_t>((q.head + 1) % config::max_queued_actions);
    q.count--;

    emit_action(em, action);

    if (q.count > 0) {
        arm_timer(q);
    }
}

void check_damping(PacingQueue& q, int64_t now_ns) noexcept {
    if (q.count == 0) return;
    if (q.last_input_ns == 0) return;

    int64_t silence = now_ns - q.last_input_ns;
    if (silence >= q.profile->damping_timeout_ns) {
        q.head = 0;
        q.tail = 0;
        q.count = 0;
        disarm_timer(q.timer_fd);
    }
}

void destroy_pacer(PacingQueue& q) noexcept {
    if (q.timer_fd >= 0) {
        close(q.timer_fd);
        q.timer_fd = -1;
    }
}

} // namespace loginext::core
