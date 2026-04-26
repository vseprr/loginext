//! UDS client that bridges `invoke("ipc_request", { line })` from the UI
//! to the daemon listening on `$XDG_RUNTIME_DIR/loginext.sock`.
//!
//! Design:
//! - One blocking `UnixStream` per request. Short-lived, local sockets —
//!   connection pooling would add lifecycle bugs for no measurable gain.
//! - Line-delimited: write the JSON request followed by `\n`, read until
//!   we see a `\n` back, return the body (sans newline) to the frontend.
//! - Any I/O error is surfaced as a JSON error envelope so the UI's
//!   typed client (`src/ipc/client.ts`) sees the same shape regardless of
//!   whether the daemon answered or the socket is missing.
//!
//! Phase 2 scope only. Multi-message streaming (for the "live preview"
//! Phase 2.4 debug overlay) will need a long-lived reader task.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::time::Duration;

use crate::daemon;

fn err_json(msg: &str) -> String {
    // Escape only the double quote — dispatch.cpp never produces raw newlines
    // or backslashes in `err` codes, and the UI tolerates JSON.parse failure.
    let escaped = msg.replace('"', "\\\"");
    format!("{{\"ok\":false,\"err\":\"{escaped}\"}}")
}

/// Round-trip a single line to the daemon. Returns the response body (no
/// trailing newline) or a synthesized error envelope.
pub fn request(mut line: String) -> String {
    if !line.ends_with('\n') {
        line.push('\n');
    }

    let path = daemon::socket_path();
    let stream = match UnixStream::connect(&path) {
        Ok(s) => s,
        Err(e) => return err_json(&format!("connect({}): {}", path.display(), e)),
    };

    // Bounded I/O — the UI must never hang if the daemon stops draining.
    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let _ = stream.set_write_timeout(Some(Duration::from_millis(500)));

    let mut writer = &stream;
    if let Err(e) = writer.write_all(line.as_bytes()) {
        return err_json(&format!("write: {e}"));
    }

    let mut reader = BufReader::new(&stream);
    let mut resp = String::new();
    match reader.read_line(&mut resp) {
        Ok(0) => err_json("eof"),
        Ok(_) => {
            if resp.ends_with('\n') {
                resp.pop();
                if resp.ends_with('\r') {
                    resp.pop();
                }
            }
            resp
        }
        Err(e) => err_json(&format!("read: {e}")),
    }
}

/// Write `~/.config/loginext/config.json` with the given settings.
/// Creates the parent directory if it does not exist.
pub fn write_config(sensitivity: &str, invert_hwheel: bool) -> Result<(), String> {
    let dir = config_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("mkdir: {e}"))?;

    let path = dir.join("config.json");
    let body = format!(
        "{{\n    \"sensitivity\": \"{sensitivity}\",\n    \"invert_hwheel\": {invert_hwheel}\n}}\n"
    );

    std::fs::write(&path, body).map_err(|e| format!("write({}): {e}", path.display()))
}

fn config_dir() -> PathBuf {
    if let Ok(xdg) = std::env::var("XDG_CONFIG_HOME") {
        if !xdg.is_empty() {
            return PathBuf::from(format!("{xdg}/loginext"));
        }
    }
    if let Ok(home) = std::env::var("HOME") {
        if !home.is_empty() {
            return PathBuf::from(format!("{home}/.config/loginext"));
        }
    }
    PathBuf::from("/tmp/loginext")
}
