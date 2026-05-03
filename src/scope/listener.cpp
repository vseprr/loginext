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
    int rc = sd_bus_open_user(&bus);
    if (rc < 0) {
        LX_DEBUG("scope: sd_bus_open_user failed (%s) — kwin-dbus disabled",
                 std::strerror(-rc));
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

    LX_INFO("scope: kwin-dbus active-window detector online "
            "(awaiting KWin script publish on %s)", kwin_dbus_service);

    int bus_fd = sd_bus_get_fd(bus);
    if (bus_fd < 0) {
        LX_WARN("scope: sd_bus_get_fd failed (%s)", std::strerror(-bus_fd));
        sd_bus_release_name(bus, kwin_dbus_service);
        sd_bus_slot_unref(slot);
        sd_bus_unref(bus);
        return false;
    }

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

        timeval  tv{};
        timeval* tvp = nullptr;
        if (bus_deadline_us != UINT64_MAX) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t now_us = static_cast<uint64_t>(now.tv_sec) * 1'000'000ULL
                            + static_cast<uint64_t>(now.tv_nsec) / 1'000ULL;
            uint64_t rel = bus_deadline_us > now_us
                           ? bus_deadline_us - now_us : 0;
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

        int sel = select(max_fd + 1, &rfds, &wfds, nullptr, tvp);
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
        LX_INFO("scope: focus → (none) [%s]", src);
    } else {
        LX_INFO("scope: focus → instance='%s' class='%s' hash=0x%08x [%s] "
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

void x11_loop(Listener* l) noexcept {
    int screen_idx = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_idx);
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

// ── Thread entry ─────────────────────────────────────────────────────

void* thread_main(void* arg) noexcept {
    auto* l = static_cast<Listener*>(arg);

    if (const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); his && *his) {
        int sock = hypr_connect(his);
        if (sock >= 0) {
            LX_INFO("scope: hyprland active-window detector online");
            hypr_loop(l, sock);
            return nullptr;
        }
        LX_WARN("scope: hyprland socket connect failed, falling through");
    }

    // KDE Plasma — preferred backend on a KDE session, regardless of
    // wayland-vs-X11. The KWin script + sd-bus bridge is the only path that
    // sees native Wayland windows on Plasma 6 (where org_kde_plasma_window_
    // management is locked down to trusted shell components). On older
    // Plasma versions it works just as well, and is cheaper than the
    // protocol backend (no per-window state, no roundtrips on bring-up).
    // We gate this on XDG_CURRENT_DESKTOP so non-KDE compositors don't
    // hold the well-known D-Bus name for nothing.
    if (const char* xdg = std::getenv("XDG_CURRENT_DESKTOP");
        xdg && std::strstr(xdg, "KDE")) {
        if (kwin_dbus_loop(l)) {
            return nullptr;
        }
        LX_INFO("scope: kwin-dbus bridge unavailable — trying wayland protocol");
    }

    // KDE Plasma Wayland (legacy / wlroots) — must be tried before X11. On a
    // Plasma Wayland session XWayland is usually running too (so DISPLAY is
    // set), but its _NET_ACTIVE_WINDOW only reflects legacy X11 apps. Native
    // Wayland apps would be invisible to the X11 backend, which is exactly
    // the failure mode we're working around here.
    if (const char* wd = std::getenv("WAYLAND_DISPLAY"); wd && *wd) {
        if (kde_wayland_loop(l)) {
            return nullptr;
        }
        LX_INFO("scope: wayland session has no org_kde_plasma_window_management "
                "(not KWin?) — trying X11");
    }

    if (const char* disp = std::getenv("DISPLAY"); disp && *disp) {
        LX_INFO("scope: x11 active-window detector online (DISPLAY=%s)", disp);
        x11_loop(l);
        return nullptr;
    }

    LX_INFO("scope: no supported compositor detected — global preset only");
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
