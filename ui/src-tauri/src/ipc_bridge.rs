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

// ─────────────────────────────────────────────────────────────────────
// app_rules.txt round-trip.
//
// Same pattern as write_config: the UI owns the text file, the daemon
// reloads it on `request("reload")`. Format mirrors src/scope/rules_loader.cpp
// — one rule per line, `<app>=<preset>`, `#` comments — so manual edits
// remain a valid escape hatch (and the file ships as documentation in the
// PKGBUILD). The Rust side only validates shape (ASCII, no '=' or '\n' in
// the app id); preset name validation lives on the daemon's load path,
// where it has the canonical preset table to compare against.
// ─────────────────────────────────────────────────────────────────────

/// One rule entry. `app` is the FNV-1a-hashed lookup key — the X11 instance
/// name, Hyprland class, KWin resourceName, or KWin resourceClass — exactly
/// as the daemon's listener publishes it. `preset` is one of the daemon's
/// `list_presets` ids (currently `tab_nav` or `zoom`).
#[derive(serde::Deserialize, serde::Serialize, Clone)]
pub struct AppRuleEntry {
    pub app: String,
    pub preset: String,
}

fn rules_path() -> PathBuf {
    config_dir().join("app_rules.txt")
}

/// Returns a JSON envelope `{"ok":true,"rules":[{"app":"...","preset":"..."}, ...]}`.
/// A missing file is reported as `{"ok":true,"rules":[]}` — the daemon
/// treats it the same way (every event resolves against the global preset).
pub fn read_app_rules() -> String {
    let path = rules_path();
    let text = match std::fs::read_to_string(&path) {
        Ok(s) => s,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(),
        Err(e) => return err_json(&format!("read({}): {}", path.display(), e)),
    };

    let mut entries: Vec<AppRuleEntry> = Vec::new();
    for raw in text.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        // Split on the FIRST '=' only — the app side is allowed to contain
        // anything except '=' itself, and we want to be forgiving about
        // trailing whitespace + comments on the value side. The daemon's
        // loader uses the same split.
        let Some(eq) = line.find('=') else { continue };
        let app = line[..eq].trim().to_string();
        let preset = line[eq + 1..].trim().to_string();
        if app.is_empty() || preset.is_empty() {
            continue;
        }
        entries.push(AppRuleEntry { app, preset });
    }

    // Build the JSON manually — the existing write_config and dispatch.cpp
    // outputs do the same thing. Avoids pulling serde_json into the build
    // for a structure this trivial.
    let mut out = String::from("{\"ok\":true,\"rules\":[");
    for (i, e) in entries.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"app\":\"{}\",\"preset\":\"{}\"}}",
            json_escape(&e.app),
            json_escape(&e.preset)
        ));
    }
    out.push_str("]}");
    out
}

/// Atomically rewrite `~/.config/loginext/app_rules.txt` from `rules`.
/// Validates each entry's `app` field (no '=' or newline — those would
/// produce a file the daemon parser would reject silently). After a
/// successful write the caller should `request("reload")` so the daemon
/// re-hashes and the new map goes live.
pub fn write_app_rules(rules: &[AppRuleEntry]) -> Result<(), String> {
    for r in rules {
        if r.app.is_empty() {
            return Err("rule has empty app id".into());
        }
        if r.preset.is_empty() {
            return Err(format!("rule '{}' has empty preset", r.app));
        }
        if r.app.contains('=') || r.app.contains('\n') || r.app.contains('\r') {
            return Err(format!(
                "app id '{}' contains '=' or newline (would corrupt the file)",
                r.app
            ));
        }
        if r.preset.contains('=') || r.preset.contains('\n') || r.preset.contains('\r') {
            return Err(format!(
                "preset '{}' contains '=' or newline",
                r.preset
            ));
        }
    }

    let dir = config_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("mkdir: {e}"))?;

    let mut body = String::with_capacity(64 + rules.len() * 32);
    body.push_str("# LogiNext — per-application preset overrides.\n");
    body.push_str("# Generated by the UI; manual edits survive but the next save overwrites them.\n");
    body.push_str("# Format: <app_id>=<preset_id>. See src/scope/rules_loader.cpp for parsing rules.\n\n");
    for r in rules {
        body.push_str(&r.app);
        body.push('=');
        body.push_str(&r.preset);
        body.push('\n');
    }

    // Write to a temp file in the same directory + rename so a crash
    // mid-write can never leave a half-written rules file the daemon would
    // partially load on its next reload.
    let final_path = rules_path();
    let tmp_path = dir.join("app_rules.txt.tmp");
    std::fs::write(&tmp_path, body)
        .map_err(|e| format!("write({}): {e}", tmp_path.display()))?;
    std::fs::rename(&tmp_path, &final_path)
        .map_err(|e| format!("rename({} → {}): {e}", tmp_path.display(), final_path.display()))
}

// JSON string escape — matches dispatch.cpp::json_escape_into. Escapes only
// '"' and '\\', drops control chars (anything < 0x20). Keeps the surface
// small because resource names / preset ids never contain them; if they
// somehow did, dropping is preferable to producing JSON the UI parser would
// reject.
fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        if b == b'"' || b == b'\\' {
            out.push('\\');
            out.push(b as char);
        } else if b >= 0x20 && b < 0x7f {
            out.push(b as char);
        }
    }
    out
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
