#include "config/args.hpp"
#include "config/loader.hpp"
#include "config/settings.hpp"
#include "core/device.hpp"
#include "core/emitter.hpp"
#include "core/event_loop.hpp"
#include "core/pacer.hpp"
#include "heuristics/scroll_state.hpp"
#include "ipc/dispatch.hpp"
#include "ipc/server.hpp"
#include "presets/preset.hpp"
#include "util/log.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <linux/input.h>
#include <string>
#include <sys/epoll.h>

namespace {

volatile sig_atomic_t  g_stop   = 0;
volatile sig_atomic_t  g_reload = 0;

void signal_stop(int /*sig*/) noexcept {
    g_stop = 1;
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
    loginext::ipc::IpcServer          ipc;
    loginext::ipc::DispatchCtx        ipc_ctx;
    int                               epoll_fd = -1;  // mirror of EventLoop's for ipc dispatch
};

void on_event(const input_event& ev, void* ctx) {
    auto* app = static_cast<AppContext*>(ctx);
    int64_t ts = timeval_to_ns(ev.time);

    if (ev.type == EV_REL && ev.code == REL_HWHEEL) {

        loginext::heuristics::tick_leak(app->scroll, ts, app->settings.profile);

        int32_t value = app->settings.invert_hwheel ? -ev.value : ev.value;
        auto dir = loginext::heuristics::process_hwheel(app->scroll, value, ts,
                                                        app->settings.profile);
        if (dir != loginext::heuristics::Direction::None) {
            // Resolve the logical tick under the currently-active preset.
            // The heuristic engine never sees this dispatch; the preset table
            // is constexpr and the lookup is a single switch.
            const auto combo = loginext::presets::resolve(
                app->settings.active_preset, dir);
            // Per-emit traces are file-only — would otherwise spam the
            // interactive terminal during normal scrolling.
            LX_TRACE("emit dir=%s preset=%s",
                     dir == loginext::heuristics::Direction::Right ? "right" : "left",
                     loginext::presets::preset_id_str(app->settings.active_preset));
            loginext::core::enqueue_combo(app->pacer, combo, ts);
        }

        loginext::core::check_damping(app->pacer, ts, app->emitter);
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
    bool ok = loginext::config::load_settings(app->config_path, app->settings);
    if (ok) {
        LX_INFO("config reloaded: mode=%s invert=%s",
                loginext::config::mode_name(app->settings.mode),
                app->settings.invert_hwheel ? "true" : "false");
    } else {
        LX_WARN("SIGHUP received but no config changes applied");
    }
    // Reset gesture state so the next event starts a clean leading-edge emit
    app->scroll = {};

    // Send the deferred ack to the UI client that requested this reload.
    loginext::ipc::send_reload_ack(app->ipc_ctx, ok);
}

// Dispatcher for every fd epoll wakes up on that isn't the device or the
// pacer timer. Today that's just the IPC listener + client sockets.
void on_io(int fd, void* ctx) {
    auto* app = static_cast<AppContext*>(ctx);
    if (fd == app->ipc.listen_fd) {
        loginext::ipc::on_accept(app->ipc, app->epoll_fd);
    } else if (loginext::ipc::owns_fd(app->ipc, fd)) {
        loginext::ipc::on_client_readable_fd(app->ipc, fd, app->epoll_fd,
                                             loginext::ipc::dispatch_with_fd,
                                             &app->ipc_ctx);
    }
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

    // --- Logger init (before anything that might log) ---
    loginext::util::LogConfig logcfg{};
    logcfg.stderr_enabled = !cli.quiet;
    logcfg.file_level = cli.verbose ? loginext::util::LogLevel::Trace
                                    : loginext::util::LogLevel::Debug;
    loginext::util::log_init(logcfg);

    // --- Settings: file first, then CLI overrides ---
    AppContext app{};
    app.config_path = cli.config_path.empty()
                    ? loginext::config::default_config_path()
                    : cli.config_path;

    loginext::config::load_settings(app.config_path, app.settings);

    if (cli.cli_mode_set)   app.settings.mode          = cli.mode;
    if (cli.cli_invert_set) app.settings.invert_hwheel = cli.invert_hwheel;
    loginext::config::apply_mode(app.settings);

    LX_INFO("config: mode=%s invert=%s path=%s",
            loginext::config::mode_name(app.settings.mode),
            app.settings.invert_hwheel ? "true" : "false",
            app.config_path.empty() ? "(none)" : app.config_path.c_str());
    if (loginext::util::log_file_path()[0] != '\0') {
        LX_INFO("log: %s", loginext::util::log_file_path());
    }

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

    // Ignore SIGPIPE so a disconnected UI client mid-write() never terminates us.
    signal(SIGPIPE, SIG_IGN);

    // --- Init ---
    auto dev = loginext::core::find_device();
    if (!dev.valid()) {
        LX_ERROR("MX Master 3S not found — check device is paired and /dev/input/event* is readable");
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    if (loginext::core::grab_device(dev) < 0) {
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    if (loginext::core::init_emitter(app.emitter) < 0) {
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    app.pacer.profile = &app.settings.profile;

    if (loginext::core::init_pacer(app.pacer) < 0) {
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    loginext::core::EventLoop loop{};
    if (loginext::core::init_loop(loop, dev.fd) < 0) {
        loginext::core::destroy_pacer(app.pacer);
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    if (loginext::core::register_timer(loop, app.pacer.timer_fd) < 0) {
        loginext::core::shutdown_loop(loop);
        loginext::core::destroy_pacer(app.pacer);
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    // IPC bring-up. A failure here is non-fatal: the daemon still does its
    // primary job (tab switching) even if the UI channel is dead.
    app.epoll_fd = loop.epoll_fd;
    app.ipc_ctx  = { &app.settings, &g_reload };

    if (loginext::ipc::init_server(app.ipc) == 0) {
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = app.ipc.listen_fd;
        if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, app.ipc.listen_fd, &ev) < 0) {
            LX_WARN("ipc: epoll add listener failed — UI channel disabled");
            loginext::ipc::shutdown_server(app.ipc);
        } else {
            LX_INFO("ipc: socket %s", app.ipc.sock_path);
        }
    }

    LX_INFO("running — SIGHUP reloads config, SIGTERM/Ctrl+C to stop");

    // --- Hot path (zero heap allocations from here) ---
    if (cli.debug_events) {
        LX_INFO("debug-events mode active: raw input_event dump on stderr");
    }

    loginext::core::run_loop(loop, dev.fd, dev.evdev,
                             &g_stop, &g_reload,
                             on_event,  &app,
                             on_timer,  &app,
                             on_reload, &app,
                             on_io,     &app,
                             cli.debug_events);

    // --- Teardown (reverse order of init) ---
    LX_INFO("shutting down");
    loginext::ipc::shutdown_server(app.ipc);
    loginext::core::shutdown_loop(loop);
    loginext::core::destroy_pacer(app.pacer);
    loginext::core::destroy_emitter(app.emitter);
    loginext::core::release_device(dev);
    loginext::util::log_shutdown();

    return EXIT_SUCCESS;
}
