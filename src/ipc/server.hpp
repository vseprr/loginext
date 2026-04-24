#pragma once

#include "ipc/protocol.hpp"

#include <array>
#include <cstddef>

namespace loginext::ipc {

// Dispatch signature. `line` is a single JSON request (no trailing '\n').
// Writes the response body into `resp` (without trailing '\n') and returns
// its length. Returning 0 means "no response, drop client silently"; a
// negative value means "protocol error — close client".
using CommandHandler = int (*)(const char* line, size_t len,
                               char* resp, size_t resp_cap,
                               void* ctx) noexcept;

// Extended handler that receives the client fd — needed for commands that
// defer their response (e.g. reload waits for event loop to finish).
using CommandHandlerFd = int (*)(const char* line, size_t len,
                                 char* resp, size_t resp_cap,
                                 void* ctx, int client_fd) noexcept;

struct ClientSlot {
    int  fd       = -1;
    int  recv_len = 0;
    char recv_buf[client_recv_buf];
};

struct IpcServer {
    int  listen_fd = -1;
    char sock_path[sock_path_buf] = {};
    std::array<ClientSlot, max_clients> clients{};
};

// Bind the UDS listener and return 0 on success, -1 on failure. On success
// `s.listen_fd` is a non-blocking listening socket that the caller must
// register with its epoll instance.
[[nodiscard]] int init_server(IpcServer& s) noexcept;

// Close listener + all client fds and unlink the socket path. Idempotent.
void shutdown_server(IpcServer& s) noexcept;

// Returns true if `fd` is the listener or any active client slot — cheap
// identity check used by the event loop's io_cb dispatcher.
[[nodiscard]] bool owns_fd(const IpcServer& s, int fd) noexcept;

// Accept as many pending connections as possible (edge-triggered friendly).
// New clients are registered with `epoll_fd`. Silently drops excess
// connections when all slots are occupied.
void on_accept(IpcServer& s, int epoll_fd) noexcept;

// Drain one client fd, invoke `handler` for every complete line, write the
// response, and recycle the slot on EOF / protocol error.
void on_client_readable(IpcServer& s, int client_fd, int epoll_fd,
                        CommandHandler handler, void* handler_ctx) noexcept;

// Same as above but uses CommandHandlerFd — passes client_fd to the handler.
void on_client_readable_fd(IpcServer& s, int client_fd, int epoll_fd,
                           CommandHandlerFd handler, void* handler_ctx) noexcept;

} // namespace loginext::ipc
