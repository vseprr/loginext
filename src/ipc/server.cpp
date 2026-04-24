#include "ipc/server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace loginext::ipc {

namespace {

// Compute $XDG_RUNTIME_DIR/loginext.sock or /tmp/loginext-<uid>.sock.
// Under sudo, XDG_RUNTIME_DIR is stripped and getuid() returns 0 — both
// wrong for agreeing with the UI process.  Fall back to SUDO_UID so the
// socket lands in the invoking user's runtime dir.
// Writes into `out` (capacity sock_path_buf). Returns true on success.
bool resolve_socket_path(char* out) noexcept {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        int n = std::snprintf(out, sock_path_buf, "%s/loginext.sock", xdg);
        return n > 0 && n < sock_path_buf;
    }

    // Under sudo: use the real user's runtime dir so the UI finds us.
    const char* sudo_uid = std::getenv("SUDO_UID");
    if (sudo_uid && *sudo_uid) {
        int n = std::snprintf(out, sock_path_buf,
                              "/run/user/%s/loginext.sock", sudo_uid);
        return n > 0 && n < sock_path_buf;
    }

    int n = std::snprintf(out, sock_path_buf, "/tmp/loginext-%u.sock",
                          static_cast<unsigned>(getuid()));
    return n > 0 && n < sock_path_buf;
}

int find_free_slot(IpcServer& s) noexcept {
    for (int i = 0; i < max_clients; ++i) {
        if (s.clients[static_cast<size_t>(i)].fd < 0) return i;
    }
    return -1;
}

int find_slot_by_fd(IpcServer& s, int fd) noexcept {
    for (int i = 0; i < max_clients; ++i) {
        if (s.clients[static_cast<size_t>(i)].fd == fd) return i;
    }
    return -1;
}

void close_client(IpcServer& s, int slot_idx, int epoll_fd) noexcept {
    auto& c = s.clients[static_cast<size_t>(slot_idx)];
    if (c.fd < 0) return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c.fd, nullptr);
    close(c.fd);
    c.fd = -1;
    c.recv_len = 0;
}

// Best-effort one-shot write of a response line followed by '\n'. UDS to a
// local process with a 1 KiB payload should never short-write in practice.
void write_line(int fd, const char* body, size_t n) noexcept {
    char buf[response_buf + 1];
    if (n > response_buf) n = response_buf;
    std::memcpy(buf, body, n);
    buf[n] = '\n';
    [[maybe_unused]] ssize_t w = write(fd, buf, n + 1);
    (void)w;
}

} // namespace

int init_server(IpcServer& s) noexcept {
    if (!resolve_socket_path(s.sock_path)) {
        std::fprintf(stderr, "[loginext] ipc: cannot resolve socket path\n");
        return -1;
    }

    // Stale socket from a previous crash — best-effort cleanup. Ignore ENOENT.
    if (unlink(s.sock_path) < 0 && errno != ENOENT) {
        std::fprintf(stderr, "[loginext] ipc: stale socket unlink failed: %s\n",
                     std::strerror(errno));
    }

    s.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s.listen_fd < 0) {
        std::fprintf(stderr, "[loginext] ipc: socket() failed: %s\n", std::strerror(errno));
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, s.sock_path, sizeof(addr.sun_path) - 1);

    if (bind(s.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[loginext] ipc: bind(%s) failed: %s\n",
                     s.sock_path, std::strerror(errno));
        close(s.listen_fd);
        s.listen_fd = -1;
        return -1;
    }

    // Restrict to user — UDS respects filesystem perms.
    if (chmod(s.sock_path, 0600) < 0) {
        std::fprintf(stderr, "[loginext] ipc: chmod(%s) failed: %s\n",
                     s.sock_path, std::strerror(errno));
    }

    // Under sudo the socket is owned by root. Transfer ownership to the
    // invoking user so the UI process (running unprivileged) can connect.
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");
    if (sudo_uid && *sudo_uid) {
        uid_t uid = static_cast<uid_t>(std::strtoul(sudo_uid, nullptr, 10));
        gid_t gid = sudo_gid && *sudo_gid
                   ? static_cast<gid_t>(std::strtoul(sudo_gid, nullptr, 10))
                   : uid;
        if (chown(s.sock_path, uid, gid) < 0) {
            std::fprintf(stderr, "[loginext] ipc: chown(%s, %u, %u) failed: %s\n",
                         s.sock_path, uid, gid, std::strerror(errno));
        }
    }

    if (listen(s.listen_fd, max_clients) < 0) {
        std::fprintf(stderr, "[loginext] ipc: listen() failed: %s\n", std::strerror(errno));
        close(s.listen_fd);
        s.listen_fd = -1;
        unlink(s.sock_path);
        return -1;
    }

    std::fprintf(stderr, "[loginext] ipc: listening on %s\n", s.sock_path);
    return 0;
}

void shutdown_server(IpcServer& s) noexcept {
    for (int i = 0; i < max_clients; ++i) {
        auto& c = s.clients[static_cast<size_t>(i)];
        if (c.fd >= 0) { close(c.fd); c.fd = -1; }
    }
    if (s.listen_fd >= 0) {
        close(s.listen_fd);
        s.listen_fd = -1;
    }
    if (s.sock_path[0] != '\0') {
        unlink(s.sock_path);
        s.sock_path[0] = '\0';
    }
}

bool owns_fd(const IpcServer& s, int fd) noexcept {
    if (fd < 0) return false;
    if (fd == s.listen_fd) return true;
    for (int i = 0; i < max_clients; ++i) {
        if (s.clients[static_cast<size_t>(i)].fd == fd) return true;
    }
    return false;
}

void on_accept(IpcServer& s, int epoll_fd) noexcept {
    for (;;) {
        int cfd = accept4(s.listen_fd, nullptr, nullptr,
                          SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[loginext] ipc: accept4 failed: %s\n", std::strerror(errno));
            return;
        }

        int slot = find_free_slot(s);
        if (slot < 0) {
            std::fprintf(stderr, "[loginext] ipc: client rejected — all %d slots full\n",
                         max_clients);
            close(cfd);
            continue;
        }

        epoll_event ev{};
        ev.events  = EPOLLIN;  // level-triggered; simpler framing
        ev.data.fd = cfd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            std::fprintf(stderr, "[loginext] ipc: epoll add client failed: %s\n",
                         std::strerror(errno));
            close(cfd);
            continue;
        }

        auto& c = s.clients[static_cast<size_t>(slot)];
        c.fd = cfd;
        c.recv_len = 0;
    }
}

void on_client_readable(IpcServer& s, int client_fd, int epoll_fd,
                        CommandHandler handler, void* handler_ctx) noexcept {
    int slot = find_slot_by_fd(s, client_fd);
    if (slot < 0) return;
    auto& c = s.clients[static_cast<size_t>(slot)];

    // Single read into whatever space remains in the recv buffer.
    int cap = client_recv_buf - c.recv_len;
    if (cap <= 0) {
        // Line too long / garbage — drop the client rather than grow.
        close_client(s, slot, epoll_fd);
        return;
    }

    ssize_t n = read(c.fd, c.recv_buf + c.recv_len, static_cast<size_t>(cap));
    if (n == 0) {                         // EOF
        close_client(s, slot, epoll_fd);
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        close_client(s, slot, epoll_fd);
        return;
    }
    c.recv_len += static_cast<int>(n);

    // Dispatch every complete line in the buffer.
    int start = 0;
    for (int i = 0; i < c.recv_len; ++i) {
        if (c.recv_buf[i] != '\n') continue;

        const char* line     = c.recv_buf + start;
        size_t      line_len = static_cast<size_t>(i - start);

        char resp[response_buf];
        int  rn = handler(line, line_len, resp, response_buf, handler_ctx);
        if (rn < 0) {
            close_client(s, slot, epoll_fd);
            return;
        }
        if (rn > 0) write_line(c.fd, resp, static_cast<size_t>(rn));
        start = i + 1;
    }

    // Shift any partial line back to the start of the buffer.
    if (start > 0) {
        int remaining = c.recv_len - start;
        if (remaining > 0) {
            std::memmove(c.recv_buf, c.recv_buf + start, static_cast<size_t>(remaining));
        }
        c.recv_len = remaining;
    }
}

void on_client_readable_fd(IpcServer& s, int client_fd, int epoll_fd,
                           CommandHandlerFd handler, void* handler_ctx) noexcept {
    int slot = find_slot_by_fd(s, client_fd);
    if (slot < 0) return;
    auto& c = s.clients[static_cast<size_t>(slot)];

    int cap = client_recv_buf - c.recv_len;
    if (cap <= 0) {
        close_client(s, slot, epoll_fd);
        return;
    }

    ssize_t n = read(c.fd, c.recv_buf + c.recv_len, static_cast<size_t>(cap));
    if (n == 0) {
        close_client(s, slot, epoll_fd);
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        close_client(s, slot, epoll_fd);
        return;
    }
    c.recv_len += static_cast<int>(n);

    int start = 0;
    for (int i = 0; i < c.recv_len; ++i) {
        if (c.recv_buf[i] != '\n') continue;

        const char* line     = c.recv_buf + start;
        size_t      line_len = static_cast<size_t>(i - start);

        char resp[response_buf];
        int  rn = handler(line, line_len, resp, response_buf, handler_ctx, c.fd);
        if (rn < 0) {
            close_client(s, slot, epoll_fd);
            return;
        }
        if (rn > 0) write_line(c.fd, resp, static_cast<size_t>(rn));
        start = i + 1;
    }

    if (start > 0) {
        int remaining = c.recv_len - start;
        if (remaining > 0) {
            std::memmove(c.recv_buf, c.recv_buf + start, static_cast<size_t>(remaining));
        }
        c.recv_len = remaining;
    }
}

} // namespace loginext::ipc
