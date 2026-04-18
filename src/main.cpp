#include "config/args.hpp"
#include "config/loader.hpp"
#include "config/settings.hpp"
#include "core/device.hpp"
#include "core/emitter.hpp"
#include "core/event_loop.hpp"
#include "core/pacer.hpp"
#include "heuristics/scroll_state.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <linux/input.h>
#include <string>

namespace {

volatile bool          g_stop   = false;
volatile sig_atomic_t  g_reload = 0;

void signal_stop(int /*sig*/) noexcept {
    g_stop = true;
}

void signal_reload(int /*sig*/) noexcept {
    g_reload = 1;
}

int64_t timeval_to_ns(const timeval& tv) noexcept {
    return static_cast<int64_t>(tv.tv_sec) * 1'000'000'000LL
         + static_cast<int64_t>(tv.tv_usec) * 1'000LL;
}

// All hot-path state packed into one struct, passed via void* ctx
struct AppContext {
    loginext::config::Settings        settings;
    std::string                       config_path;
    loginext::heuristics::ScrollState scroll;
    loginext::core::PacingQueue       pacer;
    loginext::core::EmitterHandle     emitter;
};

void on_event(const input_event& ev, void* ctx) {
    auto* app = static_cast<AppContext*>(ctx);
    int64_t ts = timeval_to_ns(ev.time);

    if (ev.type == EV_REL && ev.code == REL_HWHEEL) {
        loginext::heuristics::tick_leak(app->scroll, ts, app->settings.profile);

        int32_t value = app->settings.invert_hwheel ? -ev.value : ev.value;
        auto result = loginext::heuristics::process_hwheel(app->scroll, value, ts,
                                                           app->settings.profile);
        if (result != loginext::heuristics::ActionResult::None) {
            loginext::core::enqueue_action(app->pacer, result, ts);
        }

        loginext::core::check_damping(app->pacer, ts);
        return;
    }

    // Everything else → passthrough to virtual mouse
    if (ev.type == EV_SYN || ev.type == EV_REL || ev.type == EV_KEY ||
        ev.type == EV_MSC) {
        loginext::core::emit_passthrough(app->emitter, ev);
    }
}

void on_timer(void* ctx) {
    auto* app = static_cast<AppContext*>(ctx);
    loginext::core::process_timer(app->pacer, app->emitter);
}

void on_reload(void* ctx) {
    auto* app = static_cast<AppContext*>(ctx);
    if (loginext::config::load_settings(app->config_path, app->settings)) {
        std::fprintf(stderr, "[loginext] config reloaded: mode=%s invert=%s\n",
                     loginext::config::mode_name(app->settings.mode),
                     app->settings.invert_hwheel ? "true" : "false");
    } else {
        std::fprintf(stderr, "[loginext] SIGHUP: no config changes applied\n");
    }
    // Reset gesture state so the next event starts a clean leading-edge emit
    app->scroll.accumulator   = 0;
    app->scroll.direction     = 0;
    app->scroll.last_event_ns = 0;
    app->scroll.last_emit_ns  = 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // --- CLI parse ---
    loginext::config::CliOptions cli{};
    if (loginext::config::parse_args(argc, argv, cli) != 0) {
        loginext::config::print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (cli.help) {
        loginext::config::print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    // --- Settings: file first, then CLI overrides ---
    AppContext app{};
    app.config_path = cli.config_path.empty()
                    ? loginext::config::default_config_path()
                    : cli.config_path;

    loginext::config::load_settings(app.config_path, app.settings);

    if (cli.cli_mode_set)   app.settings.mode          = cli.mode;
    if (cli.cli_invert_set) app.settings.invert_hwheel = cli.invert_hwheel;
    loginext::config::apply_mode(app.settings);

    std::fprintf(stderr, "[loginext] config: mode=%s invert=%s path=%s\n",
                 loginext::config::mode_name(app.settings.mode),
                 app.settings.invert_hwheel ? "true" : "false",
                 app.config_path.empty() ? "(none)" : app.config_path.c_str());

    // --- Signal setup ---
    struct sigaction sa_stop{};
    sa_stop.sa_handler = signal_stop;
    sa_stop.sa_flags = 0;  // no SA_RESTART, so epoll_wait returns EINTR
    sigemptyset(&sa_stop.sa_mask);
    sigaction(SIGINT,  &sa_stop, nullptr);
    sigaction(SIGTERM, &sa_stop, nullptr);

    struct sigaction sa_hup{};
    sa_hup.sa_handler = signal_reload;
    sa_hup.sa_flags = 0;
    sigemptyset(&sa_hup.sa_mask);
    sigaction(SIGHUP, &sa_hup, nullptr);

    // --- Init ---
    auto dev = loginext::core::find_device();
    if (!dev.valid()) {
        std::fprintf(stderr, "[loginext] MX Master 3S not found\n");
        return EXIT_FAILURE;
    }

    if (loginext::core::grab_device(dev) < 0) {
        loginext::core::release_device(dev);
        return EXIT_FAILURE;
    }

    if (loginext::core::init_emitter(app.emitter) < 0) {
        loginext::core::release_device(dev);
        return EXIT_FAILURE;
    }

    app.pacer.profile = &app.settings.profile;

    if (loginext::core::init_pacer(app.pacer) < 0) {
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        return EXIT_FAILURE;
    }

    loginext::core::EventLoop loop{};
    if (loginext::core::init_loop(loop, dev.fd) < 0) {
        loginext::core::destroy_pacer(app.pacer);
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        return EXIT_FAILURE;
    }

    if (loginext::core::register_timer(loop, app.pacer.timer_fd) < 0) {
        loginext::core::shutdown_loop(loop);
        loginext::core::destroy_pacer(app.pacer);
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        return EXIT_FAILURE;
    }

    std::fprintf(stderr, "[loginext] running — SIGHUP reloads config, Ctrl+C to stop\n");

    // --- Hot path (zero heap allocations from here) ---
    loginext::core::run_loop(loop, dev.fd, dev.evdev,
                             &g_stop, &g_reload,
                             on_event,  &app,
                             on_timer,  &app,
                             on_reload, &app);

    // --- Teardown (reverse order of init) ---
    loginext::core::shutdown_loop(loop);
    loginext::core::destroy_pacer(app.pacer);
    loginext::core::destroy_emitter(app.emitter);
    loginext::core::release_device(dev);

    return EXIT_SUCCESS;
}
