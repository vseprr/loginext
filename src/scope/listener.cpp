#include "scope/listener.hpp"

#include "scope/app_hash.hpp"
#include "util/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

// ─────────────────────────────────────────────────────────────────────
//  Active-window detection.
//
//  Two backends, picked at thread start:
//
//    1. Hyprland — if HYPRLAND_INSTANCE_SIGNATURE is set, connect to
//       /tmp/hypr/$HIS/.socket2.sock and consume the line-delimited event
//       stream (`activewindow>>class,title`). Pure UDS reads, no polling.
//
//    2. X11 — fall back to libxcb. Read _NET_ACTIVE_WINDOW + WM_CLASS at
//       startup, subscribe to PropertyNotify on the root window, and
//       re-read on each notification. select() blocks on the X server fd
//       + the wake-pipe so shutdown is instant.
//
//  Either branch only ever does one thing: writes the FNV-1a hash of the
//  app id into l->active_app_hash. The hot path reads it as an integer.
// ─────────────────────────────────────────────────────────────────────

namespace loginext::scope {

namespace {

// ── Wake-pipe helpers ────────────────────────────────────────────────

void drain(int fd) noexcept {
    char b[64];
    while (read(fd, b, sizeof(b)) > 0) {}
}

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
                l->active_app_hash.store(h, std::memory_order_relaxed);
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

// ── X11 backend ──────────────────────────────────────────────────────

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
        l->active_app_hash.store(0, std::memory_order_relaxed);
        return;
    }

    xcb_icccm_get_wm_class_reply_t cls{};
    auto cls_cookie = xcb_icccm_get_wm_class(c, active);
    if (!xcb_icccm_get_wm_class_reply(c, cls_cookie, &cls, nullptr)) {
        l->active_app_hash.store(0, std::memory_order_relaxed);
        return;
    }
    // Prefer instance_name (more specific, e.g. "navigator" → Firefox); fall
    // back to class_name. Either way: hash and publish.
    const char* name = (cls.instance_name && *cls.instance_name)
                       ? cls.instance_name : cls.class_name;
    uint32_t h = (name && *name) ? hash_app(name) : 0;
    l->active_app_hash.store(h, std::memory_order_relaxed);
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

    int xfd = xcb_get_file_descriptor(c);
    while (!l->stop.load(std::memory_order_relaxed)) {
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

        bool refresh = false;
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
