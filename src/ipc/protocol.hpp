#pragma once

#include <cstddef>

// Line-delimited JSON protocol between the daemon and a local UI client.
//
// Transport : SOCK_STREAM Unix domain socket at
//             $XDG_RUNTIME_DIR/loginext.sock  (fallback: /tmp/loginext-<uid>.sock)
// Framing   : each message is a single JSON object terminated by '\n'.
// Direction : request/response, half-duplex per line. The client issues one
//             command, the daemon writes exactly one response line.
//
// Shape (request) : {"cmd":"<name>", ...}
// Shape (response): {"ok":true, ...}  or  {"ok":false,"err":"<code>"}
//
// The schema is intentionally flat — it is consumed by the same stack-based
// parser used for the config file (see src/config/loader.cpp). No escapes,
// no nesting beyond what each handler explicitly emits.

namespace loginext::ipc {

// Hard cap on concurrent UI connections. The UI is a single process; 4 slots
// leaves headroom for a CLI client (`loginext-ctl`) and an inspection tool.
constexpr int max_clients = 4;

// Per-client receive buffer. A single JSON request is expected to stay well
// under this; overflow closes the offending client.
constexpr int client_recv_buf = 1024;

// Upper bound on a single response line. Sized for `list_presets` /
// `list_devices` + a handful of small payloads; grows with the schema.
constexpr int response_buf = 1024;

// sockaddr_un::sun_path is 108 bytes on Linux; we keep a local copy for
// unlink() during shutdown.
constexpr int sock_path_buf = 108;

} // namespace loginext::ipc
