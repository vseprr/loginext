#include "scope/listener.hpp"

#include "scope/app_hash.hpp"
#include "util/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

extern "C" {
#include <wayland-client.h>
#include "plasma-window-management-client-protocol.h"
#include <systemd/sd-bus.h>
}

// ─────────────────────────────────────────────────────────────────────
//  Active-window detection.
//
//  Backends are tried in order until one binds — the rest of the daemon
//  never sees which one won, only the FNV-1a hash they publish:
//
//    1. Hyprland — if HYPRLAND_INSTANCE_SIGNATURE is set, connect to
//       /tmp/hypr/$HIS/.socket2.sock and consume the line-delimited event
//       stream (`activewindow>>class,title`). Pure UDS reads.
//
//    2. KWin D-Bus bridge — on KDE sessions (XDG_CURRENT_DESKTOP contains
//       "KDE"), claim `org.loginext.WindowFocus` on the user session bus
//       and wait for a tiny KWin script (deploy/kwin/loginext-focus) to
//       call our `Activated(s,s)` method on every workspace.windowActivated
//       signal. This is the only path that sees native Wayland windows on
//       Plasma 6 — KWin no longer advertises org_kde_plasma_window_management
//       to regular wayland clients, so the protocol backend below would
//       silently degrade to "XWayland-apps-only" via X11. sd-bus integrates
//       into the same select() loop the other backends use.
//
//    3. KDE Plasma Wayland (legacy / wlroots) — if WAYLAND_DISPLAY is set
//       and the compositor advertises `org_kde_plasma_window_management`,
//       bind v1 of the protocol, track per-window app_id + active-state
//       changes, and publish on every focus change. Works on Plasma 5 and
//       on wlroots-style compositors that implement the same interface.
//       Skipped on KDE Plasma 6 (handled by #2) to avoid double-publishing.
//
//    4. X11 / XWayland — fall back to libxcb. Read _NET_ACTIVE_WINDOW +
//       WM_CLASS at startup, subscribe to PropertyNotify on the root
//       window, and re-read on each notification.
//
//  Every branch only ever does one thing: writes the FNV-1a hash of the
//  focused window's class / app_id into l->active_app_hash. The hot path
//  reads it as an integer with memory_order_relaxed.
//
//  The introspection buffer (Listener::intro_name / intro_source) is
//  written from the same code path so the IPC `get_active_app` command
//  can report a human-readable name — but it is *never* read by on_event;
//  the mutex is contended at most ~10 Hz × on-demand UI requests.
// ─────────────────────────────────────────────────────────────────────

namespace loginext::scope {

namespace {

// ── Wake-pipe helpers ────────────────────────────────────────────────

void drain(int fd) noexcept {
    char b[64];
    while (read(fd, b, sizeof(b)) > 0) {}
}

// Defined further down — both backends share the same publish + log path so
// every focus change is visible at LX_INFO level in `loginext-logs` (e.g.
// "scope: focus → instance='Navigator' class='Firefox' hash=0x... [x11]").
void publish_and_log(Listener* l, uint32_t h, const char* src,
                     const char* instance, const char* klass) noexcept;

// ── Hyprland backend ─────────────────────────────────────────────────

int hypr_connect(const char* his) noexcept {
    char path[256];
    int n = std::snprintf(path, sizeof(path),
                          "/tmp/hypr/%s/.socket2.sock", his);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path)) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void hypr_loop(Listener* l, int sock) noexcept {
    // Line-buffered. Hyprland keeps each event on its own line, but the TCP
    // boundary may split them — we accumulate into `buf` until '\n'.
    char        buf[1024];
    std::size_t fill = 0;

    while (!l->stop.load(std::memory_order_relaxed)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock,            &rfds);
        FD_SET(l->wake_pipe[0], &rfds);
        int max_fd = sock > l->wake_pipe[0] ? sock : l->wake_pipe[0];

        int rc = select(max_fd + 1, &rfds, nullptr, nullptr, nullptr);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(l->wake_pipe[0], &rfds)) {
            drain(l->wake_pipe[0]);
            if (l->stop.load(std::memory_order_relaxed)) break;
        }
        if (!FD_ISSET(sock, &rfds)) continue;

        ssize_t n = read(sock, buf + fill, sizeof(buf) - fill);
        if (n <= 0) {
            LX_WARN("scope: hyprland socket closed (%s) — detector idle",
                    n == 0 ? "EOF" : std::strerror(errno));
            break;
        }
        fill += static_cast<std::size_t>(n);

        // Drain whole lines.
        std::size_t scan = 0;
        while (true) {
            char* nl = static_cast<char*>(std::memchr(buf + scan, '\n', fill - scan));
            if (!nl) break;
            std::size_t line_len = static_cast<std::size_t>(nl - (buf + scan));

            // Format: "activewindow>>CLASS,TITLE"
            constexpr const char* tag = "activewindow>>";
            constexpr std::size_t tag_len = 14;
            if (line_len > tag_len &&
                std::memcmp(buf + scan, tag, tag_len) == 0) {
                const char* cls = buf + scan + tag_len;
                std::size_t cls_len = line_len - tag_len;
                // Class ends at the first comma.
                if (const void* comma = std::memchr(cls, ',', cls_len)) {
                    cls_len = static_cast<std::size_t>(static_cast<const char*>(comma) - cls);
                }
                uint32_t h = (cls_len == 0) ? 0 : hash_app(cls, cls_len);
                // Stack-copy the class into a NUL-terminated buffer so the
                // logger gets a printable string without touching the read
                // buffer. cls_len is bounded by the line length (< 1 KiB).
                char klass[128];
                std::size_t cap = cls_len < sizeof(klass) - 1
                                  ? cls_len : sizeof(klass) - 1;
                std::memcpy(klass, cls, cap);
                klass[cap] = '\0';
                publish_and_log(l, h, "hyprland", klass, klass);
            }

            scan += line_len + 1;
        }
        // Compact remainder.
        if (scan > 0) {
            std::memmove(buf, buf + scan, fill - scan);
            fill -= scan;
        }
        if (fill == sizeof(buf)) {
            // Pathologically long line — drop it.
            fill = 0;
        }
    }

    close(sock);
}

// ── KWin D-Bus bridge backend (KDE Plasma 6 path) ────────────────────
//
// KDE Plasma 6's KWin restricts org_kde_plasma_window_management to trusted
// shell components, so the wayland-protocol backend below silently fails to
// see native Wayland windows on a stock Plasma 6 session. The supported
// workaround — the same one kdotool uses — is to ship a tiny KWin script
// that listens for `workspace.windowActivated` and forwards each event to
// us over the user session bus. This file owns the receive side: claim a
// well-known D-Bus name, expose an `Activated(ss)` method, and translate
// each call into the same publish_and_log() the other backends use.
//
// The hot path's contract is unchanged — sd-bus runs on the listener
// thread, the only thing that crosses the thread boundary is the same
// uint32_t atomic the X11 / Hyprland / Plasma backends already write to.
// `select()` over (sd_bus_get_fd(), wake_pipe[0]) keeps the shutdown path
// instant and avoids growing a second worker thread.

constexpr const char* kwin_dbus_service   = "org.loginext.WindowFocus";
constexpr const char* kwin_dbus_path      = "/org/loginext/WindowFocus";
constexpr const char* kwin_dbus_interface = "org.loginext.WindowFocus";

int kwin_on_activated(sd_bus_message* m, void* userdata,
                      sd_bus_error* /*ret_error*/) {
    auto* l = static_cast<Listener*>(userdata);
    const char* resource_class = nullptr;
    const char* resource_name  = nullptr;
    int rc = sd_bus_message_read(m, "ss", &resource_class, &resource_name);
    if (rc < 0) return rc;

    // First call from the KWin script — flip the diagnostic flag the loop
    // checks at the 30s mark. Single-thread access (sd_bus_process and the
    // loop run on the same listener thread), so the plain bool is fine.
    l->kwin_received_any = true;

    // KWin's resourceName matches X11's WM_CLASS instance_name (lower-cased
    // app id, e.g. "navigator" for Firefox); resourceClass matches the X11
    // class (e.g. "firefox"). Prefer resourceName for the same reasons the
    // X11 backend prefers instance_name — it's what users type in
    // app_rules.txt. Fall back to resourceClass if KWin reports an empty
    // resourceName (some toolkits do).
    const char* rule_key = (resource_name && *resource_name)
                           ? resource_name : resource_class;
    uint32_t h = (rule_key && *rule_key) ? hash_app(rule_key) : 0;
    publish_and_log(l, h, "kwin-dbus", resource_name, resource_class);

    // Reply with an empty return so the KWin script's callDBus completes
    // cleanly. The script doesn't await the reply but failing to send one
    // logs a noisy warning in the user's journal.
    return sd_bus_reply_method_return(m, "");
}

const sd_bus_vtable kwin_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Activated", "ss", "", kwin_on_activated,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

bool kwin_dbus_loop(Listener* l) noexcept {
    sd_bus* bus = nullptr;

    // The supported runtime is "daemon runs as the same user as the KDE
    // session" — guaranteed by the udev rules shipped at
    // deploy/udev/99-loginext.rules (TAG+="uaccess" + GROUP="input" on
    // /dev/input/event* for the MX Master 3S, plus /dev/uinput). Under
    // that contract sd_bus_open_user() is the correct call: it picks up
    // the user's session bus from $DBUS_SESSION_BUS_ADDRESS or
    // $XDG_RUNTIME_DIR/bus and connects without further ceremony.
    //
    // We deliberately do NOT try to bridge to the user's bus when running
    // as root. Phase 2.7.1 / 2.7.2 attempted that path and hit the dbus
    // broker's "EXTERNAL auth from uid 0" rejection — the session bus
    // drops the connection with EPIPE before the name claim is even
    // issued. Recommending unprivileged execution via udev is both simpler
    // and more secure than fighting that policy.
    int rc = sd_bus_open_user(&bus);
    if (rc < 0) {
        LX_WARN("scope: sd_bus_open_user failed (%s) — kwin-dbus disabled. "
                "If you started loginext via sudo, stop it and rerun as your "
                "regular user; install the udev rules from deploy/udev/ for "
                "unprivileged /dev/input + /dev/uinput access.",
                std::strerror(-rc));
        if (bus) sd_bus_unref(bus);
        return false;
    }

    sd_bus_slot* slot = nullptr;
    rc = sd_bus_add_object_vtable(bus, &slot, kwin_dbus_path,
                                  kwin_dbus_interface, kwin_vtable, l);
    if (rc < 0) {
        LX_WARN("scope: sd_bus_add_object_vtable failed (%s)",
                std::strerror(-rc));
        sd_bus_unref(bus);
        return false;
    }

    rc = sd_bus_request_name(bus, kwin_dbus_service, 0);
    if (rc < 0) {
        // Most common failure: another loginext instance already owns the
        // name. Don't fight over it — fall through so the wayland / X11
        // backend takes over for this process (the other instance is
        // presumably already publishing focus events).
        LX_WARN("scope: D-Bus name '%s' busy (%s) — kwin-dbus disabled",
                kwin_dbus_service, std::strerror(-rc));
        sd_bus_slot_unref(slot);
        sd_bus_unref(bus);
        return false;
    }

    LX_INFO_C(Scope, "kwin-dbus active-window detector online "
            "(awaiting KWin script publish on %s)", kwin_dbus_service);

    // ── Cold-boot bootstrap: ask KWin to publish the active window NOW ──
    //
    // Why this exists: on cold boot KWin starts roughly in parallel with
    // the daemon. KWin loads the persistent `loginext-focus` script (if
    // enabled in kwinrc), and the script's load-time `publishCurrent()`
    // runs ONCE — but it can fire BEFORE the daemon has claimed
    // `org.loginext.WindowFocus` on the bus, in which case sd-bus
    // discards the call with NameHasNoOwner and the daemon never sees
    // it. The script's 2 s heartbeat is supposed to recover, but on
    // some sessions the script is not auto-enabled in kwinrc at all
    // and the only path to a working state was the UI's
    // `register_kwin_script` flow (which the user explicitly does NOT
    // want to depend on for cold boot — see commit message for issue
    // #1).
    //
    // Two-layer bootstrap, stronger to weaker:
    //
    //   1. **Inline one-shot script via Scripting.loadScript** — writes
    //      a tiny `.js` to the daemon's state dir, asks KWin to load
    //      and run it. The script reads `workspace.activeWindow` and
    //      callDBus's our `Activated` method directly. Bypasses the
    //      persistent script entirely; works even if the user has
    //      `loginext-focusEnabled=false` in kwinrc.
    //
    //   2. **org.kde.KWin.reconfigure** — fallback if loadScript fails
    //      (older KWin that doesn't expose Scripting on the user bus,
    //      missing $XDG_STATE_HOME, etc.). Asks KWin to re-read kwinrc
    //      and reload all enabled scripts, which fires the persistent
    //      `loginext-focus` script's `publishCurrent()` if it IS
    //      enabled. Cheap; KWin KCMs trigger the same call.
    //
    // Both calls are best-effort. Failures here are non-fatal — the
    // persistent script's heartbeat (when present) is still the
    // long-term steady-state mechanism.
    bool bootstrap_published = false;
    {
        char state_dir[256] = {0};
        if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
            std::snprintf(state_dir, sizeof(state_dir), "%s/loginext", xdg);
        } else if (const char* home = std::getenv("HOME"); home && *home) {
            std::snprintf(state_dir, sizeof(state_dir),
                          "%s/.local/state/loginext", home);
        }

        if (state_dir[0] != '\0') {
            char script_path[320];
            std::snprintf(script_path, sizeof(script_path),
                          "%s/kwin-bootstrap-%d.js",
                          state_dir, static_cast<int>(getpid()));

            // Inline script: pull the current active window and push it
            // to our `Activated` method via callDBus. KWin's QtScript
            // engine accepts both `workspace.activeWindow` (Plasma 6)
            // and `workspace.activeClient` (Plasma 5 fallback).
            static constexpr const char inline_body[] =
                "var w = workspace.activeWindow || workspace.activeClient;\n"
                "if (w) {\n"
                "    callDBus(\n"
                "        \"org.loginext.WindowFocus\",\n"
                "        \"/org/loginext/WindowFocus\",\n"
                "        \"org.loginext.WindowFocus\",\n"
                "        \"Activated\",\n"
                "        \"\" + (w.resourceClass || \"\"),\n"
                "        \"\" + (w.resourceName  || \"\")\n"
                "    );\n"
                "}\n";

            FILE* f = std::fopen(script_path, "w");
            if (f) {
                std::fwrite(inline_body, 1, sizeof(inline_body) - 1, f);
                std::fclose(f);

                char plugin_name[64];
                std::snprintf(plugin_name, sizeof(plugin_name),
                              "loginext-bootstrap-%d",
                              static_cast<int>(getpid()));

                // Step 1: Scripting.loadScript(path, name) → returns int
                // script id. KWin parses the file, registers it under
                // the given plugin name, and returns a numeric handle.
                sd_bus_message* reply = nullptr;
                sd_bus_error    err{};
                int             load_rc = sd_bus_call_method(bus,
                    "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                    "loadScript", &err, &reply,
                    "ss", script_path, plugin_name);
                int32_t script_id = -1;
                if (load_rc >= 0 && reply) {
                    sd_bus_message_read(reply, "i", &script_id);
                    sd_bus_message_unref(reply);
                }
                if (load_rc < 0 || script_id < 0) {
                    LX_DEBUG("scope: kwin Scripting.loadScript failed (%s)",
                             err.message ? err.message
                                         : (load_rc < 0 ? std::strerror(-load_rc)
                                                        : "no script id returned"));
                }
                sd_bus_error_free(&err);

                // Step 2: Script<id>.run() — actually execute the
                // script body. loadScript only registers; without
                // run(), KWin doesn't dispatch the JS.
                if (script_id >= 0) {
                    char obj_path[64];
                    std::snprintf(obj_path, sizeof(obj_path),
                                  "/Scripting/Script%d", script_id);
                    err = {};
                    int run_rc = sd_bus_call_method(bus,
                        "org.kde.KWin", obj_path, "org.kde.kwin.Script",
                        "run", &err, nullptr, "");
                    if (run_rc < 0) {
                        LX_DEBUG("scope: kwin Script%d.run() failed (%s)",
                                 script_id,
                                 err.message ? err.message
                                             : std::strerror(-run_rc));
                    } else {
                        LX_INFO("scope: kwin bootstrap script run "
                                "(id=%d) — active window should publish "
                                "via Activated within ~50ms",
                                script_id);
                        bootstrap_published = true;
                    }
                    sd_bus_error_free(&err);
                }

                // KWin reads the script body into memory at
                // loadScript() time, so the on-disk file is no longer
                // needed. Removing it keeps the state dir tidy across
                // daemon restarts.
                std::remove(script_path);
            } else {
                LX_DEBUG("scope: kwin bootstrap fopen(%s) failed: %s — "
                         "falling back to reconfigure",
                         script_path, std::strerror(errno));
            }
        }
    }

    // Fallback path: if the inline script bootstrap above didn't fire
    // (older KWin / missing state dir / file write blocked by sandbox),
    // ask KWin to reload all enabled scripts. Useful when the user has
    // the persistent `loginext-focus` script enabled — its load-time
    // `publishCurrent()` will then push the active window.
    if (!bootstrap_published) {
        sd_bus_error rc_err{};
        int rc_call = sd_bus_call_method(bus,
            "org.kde.KWin", "/KWin", "org.kde.KWin", "reconfigure",
            &rc_err, nullptr, "");
        if (rc_call < 0) {
            LX_DEBUG("scope: kwin reconfigure also failed (%s) — bootstrap "
                     "skipped; the persistent script's 2 s heartbeat is "
                     "the only remaining seed mechanism",
                     rc_err.message ? rc_err.message : std::strerror(-rc_call));
        } else {
            LX_INFO("scope: kwin reconfigure dispatched (loadScript "
                    "bootstrap unavailable) — persistent script reload "
                    "should publish active window within ~50ms");
        }
        sd_bus_error_free(&rc_err);
    }

    int bus_fd = sd_bus_get_fd(bus);
    if (bus_fd < 0) {
        LX_WARN("scope: sd_bus_get_fd failed (%s)", std::strerror(-bus_fd));
        sd_bus_release_name(bus, kwin_dbus_service);
        sd_bus_slot_unref(slot);
        sd_bus_unref(bus);
        return false;
    }

    // Diagnostic deadline: warn the user 30s after binding if the KWin
    // script hasn't published a single Activated() event. The classic
    // failure mode is that the script files are installed system-wide
    // (`/usr/share/kwin/scripts/loginext-focus/`) but the per-user
    // `kwinrc` doesn't have `loginext-focusEnabled=true` because the
    // pacman post-install hook can't write into a user's $HOME. Without
    // this warning the user only sees "Currently focused: (unknown)" in
    // the UI and has no obvious next step.
    timespec bind_ts{};
    clock_gettime(CLOCK_MONOTONIC, &bind_ts);
    constexpr uint64_t kwin_warn_after_us = 30'000'000ULL;
    bool warned_no_kwin_events = false;

    // --debug-perf: per-second counters. Every 1000 ms we emit one LX_INFO
    // line summarising select() wakeups, sd_bus_process work, and Activated
    // calls processed. Lets the user pinpoint which loop is consuming CPU
    // when the daemon's cgroup shows sustained pressure. Zero overhead when
    // the flag is off (the perf_* updates compile away to dead stores when
    // the surrounding `if` is constant-false at runtime, but we keep them
    // unconditional anyway — atomic increments on a thread-local would be
    // more work than always-on integer adds on a hot path that wakes at
    // most every 2 s).
    timespec perf_last_ts = bind_ts;
    uint64_t perf_select_wakeups   = 0;
    uint64_t perf_select_zero_to   = 0;   // wakeups where select returned with 0 timeout
    uint64_t perf_bus_process_iter = 0;   // sd_bus_process invocations (sum across drains)

    while (!l->stop.load(std::memory_order_relaxed)) {
        // Drain everything sd-bus has queued internally before sleeping —
        // sd_bus_process() returns >0 while progress was made, 0 when the
        // queue is empty. Failing to do this can leave a method call sitting
        // in the bus's read buffer while we block on select().
        for (;;) {
            rc = sd_bus_process(bus, nullptr);
            if (rc < 0) {
                LX_WARN("scope: sd_bus_process failed (%s) — kwin-dbus exiting",
                        std::strerror(-rc));
                goto teardown;
            }
            ++perf_bus_process_iter;
            if (rc == 0) break;
        }
        if (l->stop.load(std::memory_order_relaxed)) break;

        // sd-bus tells us which fd events to wait for and (optionally) an
        // absolute CLOCK_MONOTONIC deadline. We must respect both: missing a
        // POLLOUT readiness when sd-bus has buffered output causes write
        // stalls, and ignoring the timeout breaks reply-tracking timeouts.
        int wanted = sd_bus_get_events(bus);
        if (wanted < 0) {
            LX_WARN("scope: sd_bus_get_events failed (%s)",
                    std::strerror(-wanted));
            break;
        }

        uint64_t bus_deadline_us = UINT64_MAX;
        rc = sd_bus_get_timeout(bus, &bus_deadline_us);
        if (rc < 0) {
            LX_WARN("scope: sd_bus_get_timeout failed (%s)",
                    std::strerror(-rc));
            break;
        }

        // Resolve "now" once — used both for the diagnostic clamp below
        // and for the sd-bus relative-deadline computation.
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_us = static_cast<uint64_t>(now.tv_sec) * 1'000'000ULL
                        + static_cast<uint64_t>(now.tv_nsec) / 1'000ULL;
        uint64_t bind_us = static_cast<uint64_t>(bind_ts.tv_sec) * 1'000'000ULL
                         + static_cast<uint64_t>(bind_ts.tv_nsec) / 1'000ULL;

        // Three terminating conditions for the warning window — once any
        // fires, `warned_no_kwin_events` flips and the diagnostic deadline
        // is dropped from the select() math. Without this gate, a healthy
        // session that received Activated() events inside the window would
        // leave the deadline computed-but-already-passed indefinitely,
        // and `rel = deadline_us > now_us ? deadline_us - now_us : 0`
        // below would pin tv to 0/0 every iteration → busy spin at
        // ~720k wakeups/s (observed: 100% CPU on the listener thread,
        // kicking in exactly 30 s after bind in --debug-perf traces).
        if (!warned_no_kwin_events) {
            if (l->kwin_received_any) {
                // Events arrived inside the window. The KWin bridge is
                // alive; we no longer need the diagnostic deadline. Don't
                // log — the focus → … line above already shows the
                // bridge working.
                warned_no_kwin_events = true;
            } else if (now_us - bind_us >= kwin_warn_after_us) {
                // Genuinely silent for 30 s — fire the help message. The
                // most common cause is the LogiNext Focus Bridge KWin
                // script not being enabled in kwinrc.
                LX_WARN("scope: 30s elapsed since kwin-dbus bind, ZERO "
                        "Activated() calls received. The 'LogiNext Focus "
                        "Bridge' KWin script is almost certainly not "
                        "enabled. Fix it without leaving the shell:\n"
                        "    kwriteconfig6 --file kwinrc --group Plugins "
                        "--key loginext-focusEnabled true\n"
                        "    qdbus6 org.kde.KWin /KWin reconfigure\n"
                        "or via System Settings → Window Management → "
                        "KWin Scripts. Until then, per-app rules cannot "
                        "match (active_app_hash stays at 0 → global "
                        "preset wins on every event).");
                warned_no_kwin_events = true;
            }
        }

        // Build the select() timeout. Two deadlines compete: sd-bus's own
        // timer wheel and (for the duration of the diagnostic window) our
        // 30 s warning trigger. Pick whichever fires first.
        uint64_t deadline_us = bus_deadline_us;
        if (!warned_no_kwin_events) {
            uint64_t our_deadline_us = bind_us + kwin_warn_after_us;
            if (deadline_us == UINT64_MAX || our_deadline_us < deadline_us) {
                deadline_us = our_deadline_us;
            }
        }

        timeval  tv{};
        timeval* tvp = nullptr;
        if (deadline_us != UINT64_MAX) {
            uint64_t rel = deadline_us > now_us ? deadline_us - now_us : 0;
            tv.tv_sec  = static_cast<time_t>(rel / 1'000'000ULL);
            tv.tv_usec = static_cast<suseconds_t>(rel % 1'000'000ULL);
            tvp = &tv;
        }

        fd_set rfds;
        fd_set wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        if (wanted & POLLIN)  FD_SET(bus_fd, &rfds);
        if (wanted & POLLOUT) FD_SET(bus_fd, &wfds);
        FD_SET(l->wake_pipe[0], &rfds);
        int max_fd = bus_fd > l->wake_pipe[0] ? bus_fd : l->wake_pipe[0];

        // Diagnostic: a 0 / near-zero relative timeout means the deadline
        // already passed, which is the canonical CPU-spin signature
        // (select returns immediately, loop iterates, no useful work). The
        // counter lets --debug-perf surface this without tracing every
        // syscall.
        const bool tvp_zero = (tvp != nullptr
                              && tv.tv_sec == 0 && tv.tv_usec == 0);
        if (tvp_zero) ++perf_select_zero_to;

        int sel = select(max_fd + 1, &rfds, &wfds, nullptr, tvp);
        ++perf_select_wakeups;
        if (sel < 0) {
            if (errno == EINTR) continue;
            LX_WARN("scope: select failed in kwin-dbus loop (%s)",
                    std::strerror(errno));
            break;
        }

        if (FD_ISSET(l->wake_pipe[0], &rfds)) {
            drain(l->wake_pipe[0]);
            if (l->stop.load(std::memory_order_relaxed)) break;
        }
        // sd_bus_process() at the top of the next iteration handles whatever
        // arrived on bus_fd — no need to dispatch from inside the select branch.

        // Per-second perf summary (--debug-perf only). Cheap clock_gettime
        // call already happens above as `now`, but the summary cadence is
        // independent of bus_deadline so we recompute here.
        if (l->debug_perf) {
            timespec perf_now{};
            clock_gettime(CLOCK_MONOTONIC, &perf_now);
            uint64_t since_us =
                (static_cast<uint64_t>(perf_now.tv_sec - perf_last_ts.tv_sec) * 1'000'000ULL)
              + (static_cast<uint64_t>(perf_now.tv_nsec) / 1'000ULL)
              - (static_cast<uint64_t>(perf_last_ts.tv_nsec) / 1'000ULL);
            if (since_us >= 1'000'000ULL) {
                LX_INFO("perf[listener]: %lu select wakeups, %lu zero-timeout, "
                        "%lu sd_bus_process iters in %.2fs",
                        static_cast<unsigned long>(perf_select_wakeups),
                        static_cast<unsigned long>(perf_select_zero_to),
                        static_cast<unsigned long>(perf_bus_process_iter),
                        static_cast<double>(since_us) / 1'000'000.0);
                perf_select_wakeups   = 0;
                perf_select_zero_to   = 0;
                perf_bus_process_iter = 0;
                perf_last_ts          = perf_now;
            }
        }
    }

teardown:
    sd_bus_release_name(bus, kwin_dbus_service);
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    return true;
}

// ── KDE Plasma Wayland backend ───────────────────────────────────────
//
// org_kde_plasma_window_management is the KWin-specific protocol that gives
// us per-window app_id + active state. We bind v1 — every event we need
// (app_id_changed, state_changed, unmapped) exists at v1, so the listener
// struct's higher-version slots can stay NULL: KWin will not invoke them
// because version negotiation gates which events the server emits.
//
// State per window is kept in a fixed-capacity slot array — no allocations,
// no STL containers — and lives entirely on the listener thread. The hot
// path's contract (one atomic, one integer) is unchanged.

constexpr std::size_t plasma_max_windows = 64;

struct PlasmaSlot;

struct PlasmaState {
    Listener*                                  l        = nullptr;
    wl_display*                                display  = nullptr;
    wl_registry*                               registry = nullptr;
    org_kde_plasma_window_management*          wm       = nullptr;
    PlasmaSlot*                                slots    = nullptr;   // → array of plasma_max_windows
};

// One per tracked window. The slot's address is the user_data we hand to
// org_kde_plasma_window_add_listener — that's how each event finds its
// per-window state without a lookup. `in_use=false` marks a free slot.
struct PlasmaSlot {
    PlasmaState*            owner   = nullptr;
    org_kde_plasma_window*  window  = nullptr;
    bool                    in_use  = false;
    bool                    active  = false;
    char                    app_id[128]{};
};

// ── Per-window event handlers ────────────────────────────────────────

void plw_title_changed(void*, org_kde_plasma_window*, const char*) {}

void plw_app_id_changed(void* data, org_kde_plasma_window*, const char* app_id) {
    auto* slot = static_cast<PlasmaSlot*>(data);
    if (!app_id) app_id = "";
    std::size_t n = std::strlen(app_id);
    if (n >= sizeof(slot->app_id)) n = sizeof(slot->app_id) - 1;
    std::memcpy(slot->app_id, app_id, n);
    slot->app_id[n] = '\0';
    // If this window is *currently* the active one, re-publish so the rule
    // engine notices an app_id changing under us (rare — happens during
    // some app launches before the proper id is known).
    if (slot->active) {
        uint32_t h = slot->app_id[0] ? hash_app(slot->app_id) : 0;
        publish_and_log(slot->owner->l, h, "kde-wayland",
                        slot->app_id, slot->app_id);
    }
}

void plw_state_changed(void* data, org_kde_plasma_window*, uint32_t flags) {
    auto* slot = static_cast<PlasmaSlot*>(data);
    bool now_active =
        (flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE) != 0;
    if (now_active == slot->active) return;
    slot->active = now_active;
    // Only publish on the activate transition. Deactivation is implied by
    // some other window's activate, so we'd just log churn. If no window
    // becomes active for a while (rare on KDE), the hash stays at the last
    // value — exactly the same behaviour as the X11 backend.
    if (now_active) {
        uint32_t h = slot->app_id[0] ? hash_app(slot->app_id) : 0;
        publish_and_log(slot->owner->l, h, "kde-wayland",
                        slot->app_id, slot->app_id);
    }
}

void plw_virtual_desktop_changed(void*, org_kde_plasma_window*, int32_t) {}
void plw_themed_icon_name_changed(void*, org_kde_plasma_window*, const char*) {}

void plw_unmapped(void* data, org_kde_plasma_window* win) {
    auto* slot = static_cast<PlasmaSlot*>(data);
    org_kde_plasma_window_destroy(win);
    slot->window  = nullptr;
    slot->in_use  = false;
    slot->active  = false;
    slot->app_id[0] = '\0';
    slot->owner   = nullptr;
}

void plw_pid_changed(void*, org_kde_plasma_window*, uint32_t) {}

// All slots above v1 stay NULL — version negotiation guarantees the server
// won't fire them. Listed only for documentation.
const org_kde_plasma_window_listener plasma_window_listener = {
    .title_changed              = plw_title_changed,
    .app_id_changed             = plw_app_id_changed,
    .state_changed              = plw_state_changed,
    .virtual_desktop_changed    = plw_virtual_desktop_changed,
    .themed_icon_name_changed   = plw_themed_icon_name_changed,
    .unmapped                   = plw_unmapped,
    .initial_state              = nullptr,
    .parent_window              = nullptr,
    .geometry                   = nullptr,
    .icon_changed               = nullptr,
    .pid_changed                = plw_pid_changed,
    .virtual_desktop_entered    = nullptr,
    .virtual_desktop_left       = nullptr,
    .application_menu           = nullptr,
    .activity_entered           = nullptr,
    .activity_left              = nullptr,
    .resource_name_changed      = nullptr,
    .client_geometry            = nullptr,
};

// ── Window-management event handlers ─────────────────────────────────

PlasmaSlot* plasma_alloc_slot(PlasmaState* st) noexcept {
    for (std::size_t i = 0; i < plasma_max_windows; ++i) {
        if (!st->slots[i].in_use) {
            st->slots[i] = {};            // reset to defaults
            st->slots[i].owner  = st;
            st->slots[i].in_use = true;
            return &st->slots[i];
        }
    }
    return nullptr;
}

void plw_register_window(PlasmaState* st, org_kde_plasma_window* win) noexcept {
    if (!win) return;
    PlasmaSlot* slot = plasma_alloc_slot(st);
    if (!slot) {
        // No room — orphan the resource (server-side state still tracked,
        // we just won't react to its events). Logged at debug since hitting
        // this means the user has > 64 windows open, which is unusual but
        // not an error.
        LX_DEBUG("scope: kde-wayland slot table full (max=%zu) — window dropped",
                 plasma_max_windows);
        org_kde_plasma_window_destroy(win);
        return;
    }
    slot->window = win;
    org_kde_plasma_window_add_listener(win, &plasma_window_listener, slot);
}

void pwm_show_desktop_changed(void*, org_kde_plasma_window_management*, uint32_t) {}

void pwm_window(void* data, org_kde_plasma_window_management* wm, uint32_t id) {
    auto* st = static_cast<PlasmaState*>(data);
    org_kde_plasma_window* win = org_kde_plasma_window_management_get_window(wm, id);
    plw_register_window(st, win);
}

const org_kde_plasma_window_management_listener plasma_wm_listener = {
    .show_desktop_changed         = pwm_show_desktop_changed,
    .window                       = pwm_window,
    .stacking_order_changed       = nullptr,   // v11+, never fires at v1
    .stacking_order_uuid_changed  = nullptr,   // v12+
    .window_with_uuid             = nullptr,   // v13+
    .stacking_order_changed_2     = nullptr,   // v17+
};

// ── Registry: spot the management interface, bind v1 ─────────────────

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t /*version*/) {
    auto* st = static_cast<PlasmaState*>(data);
    if (std::strcmp(interface, "org_kde_plasma_window_management") == 0 && !st->wm) {
        // We bind v1 deliberately — every event we need (app_id_changed,
        // state_changed, unmapped) is in v1, and binding minimum keeps
        // the listener-struct slots above v1 from ever being invoked.
        auto* proxy = wl_registry_bind(registry, name,
                                       &org_kde_plasma_window_management_interface,
                                       1);
        st->wm = static_cast<org_kde_plasma_window_management*>(proxy);
        org_kde_plasma_window_management_add_listener(st->wm, &plasma_wm_listener, st);
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

// ── Backend entry: bind, then drive the standard wayland event loop ──

bool kde_wayland_loop(Listener* l) noexcept {
    PlasmaState st{};
    PlasmaSlot  slot_storage[plasma_max_windows]{};
    st.l       = l;
    st.slots   = slot_storage;
    st.display = wl_display_connect(nullptr);
    if (!st.display) {
        const char* wd_env = std::getenv("WAYLAND_DISPLAY");
        LX_DEBUG("scope: wl_display_connect failed (no compositor at "
                 "WAYLAND_DISPLAY=%s)",
                 wd_env ? wd_env : "");
        return false;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    // First roundtrip: server sends globals.
    wl_display_roundtrip(st.display);
    if (!st.wm) {
        // Compositor doesn't speak the KDE protocol (Sway, Mutter, …) — let
        // the caller fall through to the next backend.
        wl_display_disconnect(st.display);
        return false;
    }
    // Second roundtrip: management listener now installed, server sends one
    // `window` event per existing window plus their initial states.
    wl_display_roundtrip(st.display);

    {
        const char* wd_env = std::getenv("WAYLAND_DISPLAY");
        LX_INFO("scope: kde-wayland active-window detector online "
                "(WAYLAND_DISPLAY=%s)",
                wd_env ? wd_env : "");
    }

    int wl_fd = wl_display_get_fd(st.display);

    // Standard prepare-read / read-events / dispatch-pending pattern. This
    // is the canonical wayland-client idiom for an external event loop —
    // it handles the case where another part of the program is also
    // dispatching events (we aren't, but the pattern is the safe default).
    while (!l->stop.load(std::memory_order_relaxed)) {
        while (wl_display_prepare_read(st.display) != 0) {
            wl_display_dispatch_pending(st.display);
        }
        if (wl_display_flush(st.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(st.display);
            LX_WARN("scope: kde-wayland flush failed (%s) — detector exiting",
                    std::strerror(errno));
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(wl_fd,           &rfds);
        FD_SET(l->wake_pipe[0], &rfds);
        int max_fd = wl_fd > l->wake_pipe[0] ? wl_fd : l->wake_pipe[0];

        int rc = select(max_fd + 1, &rfds, nullptr, nullptr, nullptr);
        if (rc < 0) {
            wl_display_cancel_read(st.display);
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(wl_fd, &rfds)) {
            if (wl_display_read_events(st.display) < 0) {
                LX_WARN("scope: kde-wayland read_events failed (%s) — exiting",
                        std::strerror(errno));
                break;
            }
        } else {
            wl_display_cancel_read(st.display);
        }

        if (FD_ISSET(l->wake_pipe[0], &rfds)) {
            drain(l->wake_pipe[0]);
            if (l->stop.load(std::memory_order_relaxed)) break;
        }

        wl_display_dispatch_pending(st.display);
    }

    // Tear down: destroy any windows we still own (slot resources), then the
    // wm proxy, then the registry, then the display. Order matters — the
    // server keys child resources off the parent.
    for (auto& s : slot_storage) {
        if (s.in_use && s.window) {
            org_kde_plasma_window_destroy(s.window);
            s.window = nullptr;
            s.in_use = false;
        }
    }
    if (st.wm)       org_kde_plasma_window_management_destroy(st.wm);
    if (st.registry) wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);
    return true;
}

// ── X11 backend ──────────────────────────────────────────────────────

void publish_and_log(Listener* l, uint32_t h, const char* src,
                     const char* instance, const char* klass) noexcept {
    // Stash a human-readable name for the IPC `get_active_app` command. We
    // do this *before* the hash short-circuit because the name is also the
    // useful debugging signal — a UI request shouldn't see stale text after
    // the user adjusts a rule and re-focuses.
    {
        const char* name = (instance && *instance) ? instance
                          : (klass    && *klass)   ? klass
                          : "";
        std::lock_guard<std::mutex> lk(l->intro_mutex);
        std::strncpy(l->intro_name,   name, sizeof(l->intro_name)   - 1);
        l->intro_name[sizeof(l->intro_name)   - 1] = '\0';
        std::strncpy(l->intro_source, src,  sizeof(l->intro_source) - 1);
        l->intro_source[sizeof(l->intro_source) - 1] = '\0';
    }

    uint32_t prev = l->active_app_hash.exchange(h, std::memory_order_relaxed);
    if (prev == h) return;  // No change → no log spam.
    if (h == 0) {
        LX_INFO_C(Scope, "focus → (none) [%s]", src);
    } else {
        LX_INFO_C(Scope, "focus → instance='%s' class='%s' hash=0x%08x [%s] "
                "(rule key after lower-case: '%s')",
                instance ? instance : "",
                klass    ? klass    : "",
                h, src,
                instance && *instance ? instance :
                klass    && *klass    ? klass    : "");
    }
}

void x11_publish_active(Listener* l, xcb_connection_t* c,
                        xcb_window_t root,
                        xcb_atom_t   net_active_window) noexcept {
    auto cookie = xcb_get_property(c, 0, root, net_active_window,
                                   XCB_ATOM_WINDOW, 0, 1);
    xcb_generic_error_t* err = nullptr;
    auto* reply = xcb_get_property_reply(c, cookie, &err);
    if (err) { std::free(err); return; }
    if (!reply) return;

    xcb_window_t active = 0;
    if (xcb_get_property_value_length(reply) >= 4) {
        active = *static_cast<xcb_window_t*>(xcb_get_property_value(reply));
    }
    std::free(reply);
    if (!active) {
        publish_and_log(l, 0, "x11:no_active", nullptr, nullptr);
        return;
    }

    xcb_icccm_get_wm_class_reply_t cls{};
    auto cls_cookie = xcb_icccm_get_wm_class(c, active);
    if (!xcb_icccm_get_wm_class_reply(c, cls_cookie, &cls, nullptr)) {
        publish_and_log(l, 0, "x11:no_wm_class", nullptr, nullptr);
        return;
    }
    // Prefer instance_name (more specific, e.g. "navigator" → Firefox); fall
    // back to class_name. Either way: hash and publish.
    const char* name = (cls.instance_name && *cls.instance_name)
                       ? cls.instance_name : cls.class_name;
    uint32_t h = (name && *name) ? hash_app(name) : 0;
    publish_and_log(l, h, "x11", cls.instance_name, cls.class_name);
    xcb_icccm_get_wm_class_reply_wipe(&cls);
}

void x11_loop(Listener* l, const char* display = nullptr) noexcept {
    int screen_idx = 0;
    // `display` is non-null when the polling probe in thread_main
    // (probe_x11_connect) had to fall back to a literal ":0" / ":1"
    // because $DISPLAY wasn't yet exported into our environ. Passing
    // it through avoids a second connection-failed cycle inside the
    // backend.
    xcb_connection_t* c = xcb_connect(display, &screen_idx);
    if (!c || xcb_connection_has_error(c)) {
        if (c) xcb_disconnect(c);
        LX_WARN("scope: xcb_connect failed — active-window detection disabled");
        return;
    }

    const xcb_setup_t* setup  = xcb_get_setup(c);
    auto               iter   = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_idx && iter.rem; ++i) xcb_screen_next(&iter);
    xcb_window_t       root   = iter.data->root;

    // Intern _NET_ACTIVE_WINDOW.
    auto atom_cookie = xcb_intern_atom(c, 1, 18, "_NET_ACTIVE_WINDOW");
    auto* atom_reply = xcb_intern_atom_reply(c, atom_cookie, nullptr);
    if (!atom_reply || atom_reply->atom == XCB_ATOM_NONE) {
        if (atom_reply) std::free(atom_reply);
        xcb_disconnect(c);
        LX_WARN("scope: _NET_ACTIVE_WINDOW unavailable — detector idle");
        return;
    }
    xcb_atom_t net_active_window = atom_reply->atom;
    std::free(atom_reply);

    // Subscribe to PropertyNotify on the root window.
    uint32_t mask[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, mask);
    xcb_flush(c);

    // Initial publish so we don't sit at "global" until the user clicks.
    x11_publish_active(l, c, root, net_active_window);

    LX_INFO("scope: subscribed to _NET_ACTIVE_WINDOW (atom=0x%x) on root 0x%x",
            net_active_window, root);

    int xfd = xcb_get_file_descriptor(c);
    while (!l->stop.load(std::memory_order_relaxed)) {
        // libxcb buffers events internally — `select()` only sees the kernel
        // socket. Drain the queued side first so we never sleep through a
        // notification that arrived while we were processing the previous one.
        bool refresh = false;
        while (auto* ev = xcb_poll_for_queued_event(c)) {
            uint8_t kind = ev->response_type & ~0x80;
            if (kind == XCB_PROPERTY_NOTIFY) {
                auto* pn = reinterpret_cast<xcb_property_notify_event_t*>(ev);
                if (pn->atom == net_active_window) refresh = true;
            }
            std::free(ev);
        }
        if (refresh) {
            x11_publish_active(l, c, root, net_active_window);
            continue;  // re-check the queue before sleeping
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(xfd,            &rfds);
        FD_SET(l->wake_pipe[0], &rfds);
        int max_fd = xfd > l->wake_pipe[0] ? xfd : l->wake_pipe[0];

        int rc = select(max_fd + 1, &rfds, nullptr, nullptr, nullptr);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(l->wake_pipe[0], &rfds)) {
            drain(l->wake_pipe[0]);
            if (l->stop.load(std::memory_order_relaxed)) break;
        }

        // Drain newly-arrived events from the kernel socket into libxcb's
        // queue, then loop back to the queued-drain branch above.
        while (auto* ev = xcb_poll_for_event(c)) {
            uint8_t kind = ev->response_type & ~0x80;
            if (kind == XCB_PROPERTY_NOTIFY) {
                auto* pn = reinterpret_cast<xcb_property_notify_event_t*>(ev);
                if (pn->atom == net_active_window) refresh = true;
            }
            std::free(ev);
        }
        if (refresh) x11_publish_active(l, c, root, net_active_window);
        if (xcb_connection_has_error(c)) break;
    }

    xcb_disconnect(c);
}

// ── Compositor probes (env-var-free) ─────────────────────────────────
//
// systemd --user services started via WantedBy=default.target run BEFORE
// Plasma exports session env vars (XDG_CURRENT_DESKTOP, WAYLAND_DISPLAY,
// DISPLAY). `getenv()` reads a snapshot of the process's environ at exec
// time and never updates in-process — so a daemon that gates its
// compositor backends on those vars will permanently miss every backend
// on cold boot, no matter how long it polls.
//
// The probes below replace those env-var gates with direct existence
// checks that depend ONLY on $XDG_RUNTIME_DIR, which PAM exports before
// systemd-user starts. They're called from a polling loop in thread_main
// so the listener can wait for whichever compositor surfaces first.

// Returns true if `org.kde.KWin` currently has an owner on the user's
// session bus. Opens a private bus connection per call (cheap — sd-bus
// reuses the cached XDG_RUNTIME_DIR/bus socket) so this can be called
// from outside kwin_dbus_loop without affecting its state.
bool probe_kwin_on_bus() noexcept {
    sd_bus* bus = nullptr;
    int rc = sd_bus_open_user(&bus);
    if (rc < 0) {
        if (bus) sd_bus_unref(bus);
        return false;
    }
    sd_bus_message* reply = nullptr;
    sd_bus_error    err{};
    rc = sd_bus_call_method(bus,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner",
        &err, &reply, "s", "org.kde.KWin");
    int has = 0;
    if (rc >= 0 && reply) {
        sd_bus_message_read(reply, "b", &has);
        sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&err);
    sd_bus_unref(bus);
    return has != 0;
}

// Returns true if a Wayland compositor socket exists at
// `$XDG_RUNTIME_DIR/wayland-N` for any N in 0..3. KDE Plasma 6 always
// uses wayland-0; the loop covers nested-compositor / multi-seat setups
// out of paranoia. Stat-only — no connection, no event drain.
bool probe_wayland_socket() noexcept {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) return false;
    for (int n = 0; n < 4; ++n) {
        char path[256];
        int written = std::snprintf(path, sizeof(path), "%s/wayland-%d",
                                    runtime, n);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) {
            continue;
        }
        struct stat st{};
        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) return true;
    }
    return false;
}

// Returns the first display string that gives us a working xcb_connect()
// (caller-owned, statically-allocated → no free needed), or nullptr if
// none worked. We try $DISPLAY first when set, then `:0` and `:1` as
// fallbacks for the cold-boot case where the env var hasn't been
// exported into our environ yet but XWayland is running on the
// canonical socket.
const char* probe_x11_display() noexcept {
    static thread_local char chosen[8];
    const char* candidates[3] = { nullptr, nullptr, nullptr };
    int n = 0;
    if (const char* env = std::getenv("DISPLAY"); env && *env) {
        candidates[n++] = env;
    }
    candidates[n++] = ":0";
    candidates[n++] = ":1";
    for (int i = 0; i < n; ++i) {
        xcb_connection_t* c = xcb_connect(candidates[i], nullptr);
        bool ok = (c != nullptr) && (xcb_connection_has_error(c) == 0);
        if (c) xcb_disconnect(c);
        if (ok) {
            std::strncpy(chosen, candidates[i], sizeof(chosen) - 1);
            chosen[sizeof(chosen) - 1] = '\0';
            return chosen;
        }
    }
    return nullptr;
}

// ── Thread entry ─────────────────────────────────────────────────────

void* thread_main(void* arg) noexcept {
    auto* l = static_cast<Listener*>(arg);

    // Backends are tried in priority order: Hyprland → KDE (kwin-dbus) →
    // KDE/wlroots Wayland (org_kde_plasma_window_management) → X11. The
    // first one that probes successfully wins; the rest never run, so
    // the daemon doesn't claim well-known D-Bus names on hosts that
    // don't need them.
    //
    // Polling exists because of the systemd-user / Plasma cold-boot
    // race: when our service is started by `WantedBy=default.target`,
    // PAM has already exported XDG_RUNTIME_DIR, but the desktop
    // session hasn't yet exported XDG_CURRENT_DESKTOP / WAYLAND_DISPLAY
    // / DISPLAY. The probes above are env-var-free (or env-var-tolerant
    // for X11), so the polling loop catches up the moment KWin /
    // wayland / XWayland become reachable on disk + bus — typically
    // within a few seconds of login on Plasma 6.
    constexpr int probe_max_attempts  = 30;   // 30 × 2s = 60s total grace
    constexpr int probe_interval_sec  = 2;
    bool announced_polling = false;

    for (int attempt = 0; attempt < probe_max_attempts; ++attempt) {
        if (l->stop.load(std::memory_order_relaxed)) return nullptr;

        // 1. Hyprland — env-var IS reliable on Hyprland because the
        // Hyprland launcher sets it before exec'ing user services.
        // Keep the existing path; the failure mode is "not Hyprland",
        // which the next probes handle.
        if (const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
            his && *his) {
            int sock = hypr_connect(his);
            if (sock >= 0) {
                LX_INFO("scope: hyprland active-window detector online");
                hypr_loop(l, sock);
                return nullptr;
            }
            LX_WARN("scope: hyprland socket connect failed, "
                    "falling through to other backends");
        }

        // 2. KDE Plasma — preferred on KDE regardless of wayland-vs-X11.
        // Direct bus probe replaces the old XDG_CURRENT_DESKTOP gate.
        if (probe_kwin_on_bus()) {
            if (kwin_dbus_loop(l)) return nullptr;
            LX_INFO("scope: kwin-dbus bridge unavailable — "
                    "trying wayland protocol");
        }

        // 3. KDE Wayland (legacy / wlroots) — direct socket probe.
        // kde_wayland_loop itself returns false if the compositor
        // doesn't speak org_kde_plasma_window_management, so a non-KDE
        // Wayland session falls through to X11 on the same iteration.
        if (probe_wayland_socket()) {
            if (kde_wayland_loop(l)) return nullptr;
            LX_INFO("scope: wayland session has no "
                    "org_kde_plasma_window_management — trying X11");
        }

        // 4. X11 / XWayland — try $DISPLAY then :0 / :1 fallbacks.
        if (const char* disp = probe_x11_display(); disp != nullptr) {
            LX_INFO("scope: x11 active-window detector online (DISPLAY=%s)",
                    disp);
            x11_loop(l, disp);
            return nullptr;
        }

        // None of the probes saw a compositor yet. Wait and retry.
        if (!announced_polling) {
            LX_INFO("scope: no compositor ready yet — polling every %ds "
                    "for up to %ds (cold-boot race against Plasma's "
                    "session-env import)",
                    probe_interval_sec,
                    probe_max_attempts * probe_interval_sec);
            announced_polling = true;
        }

        // Shutdown-aware sleep. wake_pipe[0] is written by stop() so
        // teardown unblocks immediately instead of waiting up to
        // probe_interval_sec.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(l->wake_pipe[0], &rfds);
        timeval tv{};
        tv.tv_sec  = probe_interval_sec;
        tv.tv_usec = 0;
        int rc = select(l->wake_pipe[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (rc > 0 && FD_ISSET(l->wake_pipe[0], &rfds)) {
            drain(l->wake_pipe[0]);
            if (l->stop.load(std::memory_order_relaxed)) return nullptr;
        }
    }

    LX_WARN("scope: no compositor detected after %ds — per-app rules "
            "disabled, daemon will use the global preset on every event",
            probe_max_attempts * probe_interval_sec);

    // Idle until shutdown so the caller's join() returns cleanly.
    char b;
    while (!l->stop.load(std::memory_order_relaxed)) {
        if (read(l->wake_pipe[0], &b, 1) < 0 && errno != EINTR) break;
    }
    return nullptr;
}

} // namespace

int start(Listener& l) noexcept {
    if (pipe2(l.wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        LX_WARN("scope: pipe2 failed (%s) — listener disabled", std::strerror(errno));
        return -1;
    }
    l.stop.store(false, std::memory_order_relaxed);
    int rc = pthread_create(&l.thread, nullptr, &thread_main, &l);
    if (rc != 0) {
        close(l.wake_pipe[0]);
        close(l.wake_pipe[1]);
        l.wake_pipe[0] = l.wake_pipe[1] = -1;
        LX_WARN("scope: pthread_create failed (%s) — listener disabled",
                std::strerror(rc));
        return -1;
    }
    l.started = true;
    return 0;
}

void stop(Listener& l) noexcept {
    if (!l.started) return;
    l.stop.store(true, std::memory_order_relaxed);
    if (l.wake_pipe[1] >= 0) {
        char b = 0;
        [[maybe_unused]] ssize_t w = write(l.wake_pipe[1], &b, 1);
    }
    pthread_join(l.thread, nullptr);
    if (l.wake_pipe[0] >= 0) close(l.wake_pipe[0]);
    if (l.wake_pipe[1] >= 0) close(l.wake_pipe[1]);
    l.wake_pipe[0] = l.wake_pipe[1] = -1;
    l.started = false;
}

} // namespace loginext::scope
