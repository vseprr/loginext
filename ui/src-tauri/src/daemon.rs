//! Daemon lifecycle — check whether `loginext.sock` is alive on startup, and
//! if not, spawn the C++ daemon as a fully detached background process.
//!
//! Lifecycle contract:
//! - The UI process is the trigger, not the parent. Once spawned the daemon
//!   has its own session (setsid), no controlling terminal, and stdio routed
//!   to /dev/null. When the UI exits the daemon survives; reopening the UI
//!   silently reconnects to the existing socket.
//! - The probe is cheap: a non-blocking `connect()` to the UDS with a 200 ms
//!   timeout. No file fingerprints, no heartbeat shared state.
//! - If the spawn fails (binary not found, permission denied), we surface the
//!   reason on stderr exactly once and keep going — the UI degrades to its
//!   "daemon: unreachable" status bar instead of crashing.

use std::io;
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

// Avoid pulling the `libc` crate for a single setsid() call — matches the
// style established in ipc_bridge.rs.
extern "C" {
    fn setsid() -> i32;
    fn getuid() -> u32;
}

const PROBE_TIMEOUT: Duration = Duration::from_millis(200);
const SPAWN_WAIT_BUDGET: Duration = Duration::from_millis(3000);
const SPAWN_POLL_INTERVAL: Duration = Duration::from_millis(50);

/// Returns the resolved daemon socket path. Mirrors the C++ side
/// (ipc/server.cpp::resolve_socket_path) so they always agree.
pub fn socket_path() -> PathBuf {
    if let Ok(xdg) = std::env::var("XDG_RUNTIME_DIR") {
        if !xdg.is_empty() {
            return PathBuf::from(format!("{xdg}/loginext.sock"));
        }
    }
    let uid = unsafe { getuid() };
    PathBuf::from(format!("/tmp/loginext-{uid}.sock"))
}

/// Quick probe: is something accepting connections on the socket right now?
fn socket_alive(path: &Path) -> bool {
    match UnixStream::connect(path) {
        Ok(s) => {
            // Set a tight timeout so a stale-but-listening socket can't hang us.
            let _ = s.set_read_timeout(Some(PROBE_TIMEOUT));
            let _ = s.set_write_timeout(Some(PROBE_TIMEOUT));
            true
        }
        Err(_) => false,
    }
}

/// Resolve the daemon binary path. Search order:
///   1. $LOGINEXT_DAEMON (absolute path override — used by `cargo tauri dev`)
///   2. ../../build/loginext relative to the UI executable (dev workflow)
///   3. `loginext` on $PATH
///   4. /usr/local/bin/loginext, /usr/bin/loginext (system install)
fn resolve_daemon_binary() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("LOGINEXT_DAEMON") {
        let path = PathBuf::from(p);
        if path.is_file() {
            return Some(path);
        }
    }

    // Dev workflow: `cargo tauri dev` puts the UI binary under
    // ui/src-tauri/target/{debug,release}/loginext-ui. Walk up to the repo
    // root and look for build/loginext.
    if let Ok(exe) = std::env::current_exe() {
        let mut p = exe.clone();
        for _ in 0..6 {
            if let Some(parent) = p.parent() {
                let candidate = parent.join("build").join("loginext");
                if candidate.is_file() {
                    return Some(candidate);
                }
                p = parent.to_path_buf();
            } else {
                break;
            }
        }
    }

    // PATH lookup — manually walk to avoid pulling the `which` crate.
    if let Ok(path_env) = std::env::var("PATH") {
        for dir in path_env.split(':') {
            if dir.is_empty() {
                continue;
            }
            let candidate = Path::new(dir).join("loginext");
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }

    for fallback in ["/usr/local/bin/loginext", "/usr/bin/loginext"] {
        let p = PathBuf::from(fallback);
        if p.is_file() {
            return Some(p);
        }
    }
    None
}

/// Spawn the daemon as a detached background process. The child:
///   - is in its own session (setsid → no controlling tty, survives our exit)
///   - has stdio routed to /dev/null (no captured pipes that would tie its
///     lifetime to the UI's stdout buffer)
///   - is intentionally not awaited; std::process::Child::Drop is a no-op so
///     the kernel handle leaks. When the UI exits, init/systemd reparents and
///     reaps. If the daemon crashes mid-session, the brief zombie is reaped
///     when the UI exits — fine for our scope.
fn spawn_detached(daemon: &Path) -> io::Result<u32> {
    let mut cmd = Command::new(daemon);
    cmd.arg("--quiet")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());

    // SAFETY: setsid() is async-signal-safe and only mutates the calling
    // process's session id. Runs in the forked child between fork and exec,
    // before any Rust runtime state is touched.
    unsafe {
        cmd.pre_exec(|| {
            if setsid() < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }

    let child = cmd.spawn()?;
    Ok(child.id())
    // child dropped here — std::process::Child::Drop is a no-op on Unix
}

/// Poll the socket until either it accepts a connection or the budget runs
/// out. Returns true if the daemon answered before the deadline.
fn wait_for_socket(path: &Path) -> bool {
    let deadline = Instant::now() + SPAWN_WAIT_BUDGET;
    loop {
        if socket_alive(path) {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        std::thread::sleep(SPAWN_POLL_INTERVAL);
    }
}

/// Outcome of `ensure_running()` — exposed so the UI can show a meaningful
/// status bar message on first paint.
#[derive(Debug, Clone)]
pub enum SpawnOutcome {
    AlreadyRunning,
    Spawned { pid: u32 },
    SpawnFailed { reason: String },
    BinaryNotFound,
    Timeout,
}

/// Check the socket and spawn the daemon if needed. Logs one concise line to
/// stderr summarising the outcome — keeps `cargo tauri dev` output usable.
pub fn ensure_running() -> SpawnOutcome {
    let path = socket_path();

    if socket_alive(&path) {
        eprintln!("[loginext-ui] daemon: already running ({})", path.display());
        return SpawnOutcome::AlreadyRunning;
    }

    let Some(bin) = resolve_daemon_binary() else {
        eprintln!(
            "[loginext-ui] daemon: binary not found (set LOGINEXT_DAEMON or install loginext)"
        );
        return SpawnOutcome::BinaryNotFound;
    };

    let pid = match spawn_detached(&bin) {
        Ok(p) => p,
        Err(e) => {
            let reason = format!("{e}");
            eprintln!("[loginext-ui] daemon: spawn({}) failed: {}", bin.display(), reason);
            return SpawnOutcome::SpawnFailed { reason };
        }
    };

    if wait_for_socket(&path) {
        eprintln!(
            "[loginext-ui] daemon: spawned pid={} bin={} sock={}",
            pid,
            bin.display(),
            path.display()
        );
        SpawnOutcome::Spawned { pid }
    } else {
        eprintln!(
            "[loginext-ui] daemon: pid={} did not reach socket {} within {}ms",
            pid,
            path.display(),
            SPAWN_WAIT_BUDGET.as_millis()
        );
        SpawnOutcome::Timeout
    }
}

