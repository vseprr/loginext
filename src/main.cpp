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
#include "scope/listener.hpp"
#include "scope/rules.hpp"
#include "scope/rules_loader.hpp"
#include "util/log.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
    std::string                       rules_path;
    loginext::heuristics::ScrollState scroll;
    loginext::core::PacingQueue       pacer;
    loginext::core::EmitterHandle     emitter;
    loginext::ipc::IpcServer          ipc;
    loginext::ipc::DispatchCtx        ipc_ctx;
    loginext::scope::RuleTable        rules;       // hot-path O(1) per-app lookup
    loginext::scope::Listener         scope;       // background detector → atomic hash
    int                               epoll_fd = -1;  // mirror of EventLoop's for ipc dispatch
};

void on_event(const input_event& ev, void* ctx) {
    auto* app = static_cast<AppContext*>(ctx);
    int64_t ts = timeval_to_ns(ev.time);

    if (ev.type == EV_REL && ev.code == REL_HWHEEL) {

        // Per-app scope: resolve effective preset, sensitivity profile,
        // and invert flag BEFORE running heuristics so the passthrough
        // branch can short-circuit and the heuristic uses the right
        // profile timings. AppRule wins on every field where it has a
        // non-inherit override; otherwise we fall back to settings.
        uint32_t app_hash = app->scope.active_app_hash.load(
            std::memory_order_relaxed);
        loginext::presets::PresetId       effective_preset = app->settings.active_preset;
        const loginext::config::Profile*  effective_profile = &app->settings.profile;
        bool                              effective_invert  = app->settings.invert_hwheel;

        loginext::scope::AppRule rule;
        if (loginext::scope::lookup(app->rules, app_hash, rule)) {
            effective_preset = rule.preset;
            if (rule.mode != loginext::config::SensitivityMode::Inherit) {
                effective_profile = &loginext::config::profile_for(rule.mode);
            }
            if (rule.invert != loginext::scope::invert_inherit) {
                effective_invert = (rule.invert == loginext::scope::invert_on);
            }
        }

        // Passthrough: explicit None preset (global or per-app) → forward
        // raw HWHEEL as-is so the wheel behaves as a normal unmapped
        // input device.
        if (effective_preset == loginext::presets::PresetId::None) {
            loginext::core::emit_passthrough(app->emitter, ev);
            return;
        }

        loginext::heuristics::tick_leak(app->scroll, ts, *effective_profile);

        int32_t value = effective_invert ? -ev.value : ev.value;
        auto dir = loginext::heuristics::process_hwheel(app->scroll, value, ts,
                                                        *effective_profile);
        if (dir != loginext::heuristics::Direction::None) {
            // Resolve the logical tick under the effective preset. The
            // heuristic engine never sees this dispatch; the preset table is
            // constexpr and the lookup is a single switch.
            const auto combo = loginext::presets::resolve(effective_preset, dir);
            // Per-emit traces are file-only — would otherwise spam the
            // interactive terminal during normal scrolling. Logs the
            // *effective* preset (after per-app override) so the trace
            // matches what actually gets emitted; logging active_preset
            // here would silently mask per-app rule resolution bugs.
            LX_TRACE("emit dir=%s preset=%s mode=%s invert=%s app_hash=0x%08x",
                     dir == loginext::heuristics::Direction::Right ? "right" : "left",
                     loginext::presets::preset_id_str(effective_preset),
                     loginext::config::mode_name(
                         effective_profile == &loginext::config::profile_low    ? loginext::config::SensitivityMode::Low :
                         effective_profile == &loginext::config::profile_high   ? loginext::config::SensitivityMode::High :
                                                                                   loginext::config::SensitivityMode::Medium),
                     effective_invert ? "true" : "false",
                     app_hash);
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
        LX_INFO("config reloaded: mode=%s invert=%s active_preset=%s",
                loginext::config::mode_name(app->settings.mode),
                app->settings.invert_hwheel ? "true" : "false",
                loginext::presets::preset_id_str(app->settings.active_preset));
    } else {
        LX_WARN("SIGHUP received but no config changes applied");
    }
    // Per-app scope rules live in a sidecar file (so the JSON parser stays
    // small per agents.md rule 2). Reload them on the same SIGHUP so the
    // table stays in sync with the user's edits.
    loginext::scope::load_rules(app->rules_path, app->rules);

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
    // --quiet raises the stderr threshold to Warn instead of disabling the
    // sink entirely. Disabling stderr made every WARN/ERROR vanish from
    // journald when running as a systemd user unit (ExecStart=... --quiet),
    // which masked real bind/sandbox/dbus failures and made debugging
    // impossible. Lifecycle Info chatter is still suppressed so journalctl
    // stays readable; problems still get through.
    logcfg.stderr_level = cli.quiet ? loginext::util::LogLevel::Warn
                                    : loginext::util::LogLevel::Info;
    logcfg.file_level = cli.verbose ? loginext::util::LogLevel::Trace
                                    : loginext::util::LogLevel::Debug;
    loginext::util::log_init(logcfg);

    // --- Settings: file first, then CLI overrides ---
    AppContext app{};
    app.config_path = cli.config_path.empty()
                    ? loginext::config::default_config_path()
                    : cli.config_path;
    app.rules_path  = loginext::scope::default_rules_path();

    loginext::config::load_settings(app.config_path, app.settings);
    loginext::scope::load_rules(app.rules_path, app.rules);

    if (cli.cli_mode_set)   app.settings.mode          = cli.mode;
    if (cli.cli_invert_set) app.settings.invert_hwheel = cli.invert_hwheel;
    loginext::config::apply_mode(app.settings);

    LX_INFO("config: mode=%s invert=%s active_preset=%s path=%s",
            loginext::config::mode_name(app.settings.mode),
            app.settings.invert_hwheel ? "true" : "false",
            loginext::presets::preset_id_str(app.settings.active_preset),
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

    // --- IPC bring-up (early) ---
    // Must run BEFORE the device-retry loop below. The UI's daemon-startup
    // probe checks for the listener socket at $XDG_RUNTIME_DIR/loginext.sock
    // with a 5 s timeout. If the daemon is mid-retry waiting for udev to
    // enumerate /dev/input/event*, the socket would not exist yet and the
    // UI would log `systemd-managed socket … did not come up within 5000ms`
    // and load with an empty preset list. Binding the socket here, before
    // the retry, makes the UI's existence probe pass immediately. The
    // epoll registration for the listener fd is deferred to after
    // init_loop() below — until then no requests are serviced, but the
    // kernel queues client connects on the listen backlog and they're
    // accepted as soon as the event loop starts.
    //
    // Settings + rules are already loaded (lines above) so the dispatch
    // context is fully populated. A bind failure here is non-fatal: the
    // daemon still does its primary job (input grab + emit) even if the
    // UI channel is dead — same posture as the original late bring-up.
    app.ipc_ctx = { &app.settings, &g_reload, -1, &app.scope, &app.rules,
                    &g_stop };
    bool ipc_bound = (loginext::ipc::init_server(app.ipc) == 0);
    if (ipc_bound) {
        LX_INFO("ipc: socket %s (epoll registration deferred)", app.ipc.sock_path);
    } else {
        LX_WARN("ipc: socket bind failed — UI channel disabled");
    }

    // --- Init ---
    // Bounded udev grace window: at cold boot the user-manager's
    // default.target is reached before udev finishes enumerating
    // /dev/input/event*, so a freshly-started systemd unit can land
    // here with no Logitech device visible yet. Without this retry
    // the daemon would exit non-zero, systemd would restart it on
    // RestartSec=15, and a slow boot can chew through StartLimitBurst=5
    // before the receiver appears. 10 attempts × 2 s = 20 s grace per
    // restart cycle — sized against StartLimitIntervalSec=120 in the
    // unit file so the burst guard remains meaningful.
    auto dev = loginext::core::find_device();
    if (!dev.valid()) {
        constexpr int kFindRetries = 9;       // + the one above = 10 total attempts
        constexpr int kFindIntervalSec = 2;
        for (int attempt = 1; attempt <= kFindRetries; ++attempt) {
            if (g_stop) {
                LX_INFO("device search interrupted by signal — exiting");
                if (ipc_bound) loginext::ipc::shutdown_server(app.ipc);
                loginext::util::log_shutdown();
                return EXIT_FAILURE;
            }
            LX_WARN("MX Master 3S not yet enumerated by udev "
                    "(attempt %d/%d) — retrying in %d s",
                    attempt, kFindRetries + 1, kFindIntervalSec);
            // nanosleep returns -1/EINTR when SIGTERM/SIGINT lands (sa_flags=0
            // above so signals interrupt syscalls); next iteration checks
            // g_stop and exits cleanly. Don't restart the sleep on EINTR.
            struct timespec req{kFindIntervalSec, 0};
            nanosleep(&req, nullptr);
            dev = loginext::core::find_device();
            if (dev.valid()) break;
        }
    }
    if (!dev.valid()) {
        LX_ERROR("MX Master 3S not found after retries — check device is paired and /dev/input/event* is readable");
        if (ipc_bound) loginext::ipc::shutdown_server(app.ipc);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    if (loginext::core::grab_device(dev) < 0) {
        if (ipc_bound) loginext::ipc::shutdown_server(app.ipc);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    if (loginext::core::init_emitter(app.emitter) < 0) {
        if (ipc_bound) loginext::ipc::shutdown_server(app.ipc);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    app.pacer.profile = &app.settings.profile;

    if (loginext::core::init_pacer(app.pacer) < 0) {
        if (ipc_bound) loginext::ipc::shutdown_server(app.ipc);
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    loginext::core::EventLoop loop{};
    if (loginext::core::init_loop(loop, dev.fd) < 0) {
        if (ipc_bound) loginext::ipc::shutdown_server(app.ipc);
        loginext::core::destroy_pacer(app.pacer);
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    if (loginext::core::register_timer(loop, app.pacer.timer_fd) < 0) {
        if (ipc_bound) loginext::ipc::shutdown_server(app.ipc);
        loginext::core::shutdown_loop(loop);
        loginext::core::destroy_pacer(app.pacer);
        loginext::core::destroy_emitter(app.emitter);
        loginext::core::release_device(dev);
        loginext::util::log_shutdown();
        return EXIT_FAILURE;
    }

    // IPC epoll registration. The listener was already bound above (before
    // the device retry) so the UI's existence probe passed regardless of
    // udev timing; here we wire the listen fd into the event loop so
    // accept() actually fires on incoming connects. A failure here is
    // non-fatal: the daemon still does its primary job (input grab + emit)
    // even if the UI channel is dead.
    app.epoll_fd = loop.epoll_fd;

    if (ipc_bound) {
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = app.ipc.listen_fd;
        if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, app.ipc.listen_fd, &ev) < 0) {
            LX_WARN("ipc: epoll add listener failed — UI channel disabled");
            loginext::ipc::shutdown_server(app.ipc);
            ipc_bound = false;
        } else {
            LX_INFO("ipc: epoll registration complete — accepting clients");
        }
    }

    // Per-app scope listener — own thread, never touches the epoll loop.
    // A failure to start is non-fatal: the daemon falls back to global-only
    // resolution and logs a warning. start() does not block on compositor
    // I/O; the actual probe runs inside the spawned thread.
    app.scope.debug_perf = cli.debug_perf;
    if (loginext::scope::start(app.scope) != 0) {
        LX_WARN("scope: listener disabled — per-app rules inactive");
    }

    LX_INFO("running — SIGHUP reloads config, SIGTERM/Ctrl+C to stop");

    // --- Hot path (zero heap allocations from here) ---
    if (cli.debug_events) {
        LX_INFO("debug-events mode active: raw input_event dump on stderr");
    }
    if (cli.debug_perf) {
        LX_INFO("debug-perf mode active: per-second counters via "
                "perf[main] / perf[listener] log lines");
    }

    int loop_rc = loginext::core::run_loop(loop, dev.fd, dev.evdev,
                             &g_stop, &g_reload,
                             on_event,  &app,
                             on_timer,  &app,
                             on_reload, &app,
                             on_io,     &app,
                             cli.debug_events,
                             cli.debug_perf);

    // --- Teardown (reverse order of init) ---
    LX_INFO("shutting down");
    loginext::scope::stop(app.scope);
    loginext::ipc::shutdown_server(app.ipc);
    loginext::core::shutdown_loop(loop);
    loginext::core::destroy_pacer(app.pacer);
    loginext::core::destroy_emitter(app.emitter);
    loginext::core::release_device(dev);
    loginext::util::log_shutdown();

    // Non-zero on a fatal device error so systemd's Restart=on-failure
    // fires; the bounded find_device() retry above then waits for udev
    // to re-publish the device on replug / resume-from-sleep.
    return loop_rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
