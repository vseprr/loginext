#pragma once

#include "config/settings.hpp"

#include <csignal>
#include <cstddef>

// Forward declare the scope types the introspection commands depend on so
// the IPC layer doesn't pull in the listener-thread world. Both modules are
// already linked into the daemon — this just keeps include directions clean.
namespace loginext::scope {
    struct Listener;
    struct RuleTable;
}

namespace loginext::ipc {

// Context pointer handed to the CommandHandler on every line. Keeps the
// handler purely functional — no globals, no singletons.
//
// `scope` and `rules` are nullable: if the per-app scope listener never
// started (older session, no compositor), the introspection commands
// degrade gracefully instead of returning bogus data.
struct DispatchCtx {
    loginext::config::Settings*       settings;
    volatile sig_atomic_t*            reload_flag;       // raised to trigger SIGHUP-style reload
    int                               reload_pending_fd = -1;  // client fd awaiting reload ack
    loginext::scope::Listener*        scope = nullptr;
    const loginext::scope::RuleTable* rules = nullptr;
};

// CommandHandler-compatible entry point. Parses the first "cmd" key out of
// `line` and routes to a per-command response builder.
//
// Returns the number of bytes written into `resp` (never terminated), or
// -1 on unrecoverable protocol error (caller should drop the client).
// Returns 0 for deferred responses (e.g. reload — ack sent after event loop
// processes the reload).
int dispatch(const char* line, size_t len,
             char* resp, size_t resp_cap,
             void* ctx) noexcept;

// Same as dispatch(), but carries the client fd so handlers that defer their
// response (e.g. reload) can stash it for a later ack.
int dispatch_with_fd(const char* line, size_t len,
                     char* resp, size_t resp_cap,
                     void* ctx, int client_fd) noexcept;

// Write the reload acknowledgment to the client that requested it. Called
// from the event loop's reload callback after config is live. The pending
// fd is reset to -1 after the ack is sent.
void send_reload_ack(DispatchCtx& ctx, bool success) noexcept;

} // namespace loginext::ipc
