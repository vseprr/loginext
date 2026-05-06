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
use std::io::{BufRead, BufReader, Write};
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
    fn kill(pid: i32, sig: i32) -> i32;
}

const SIGTERM: i32 = 15;
const SIGKILL: i32 = 9;
const KILL_GRACE: Duration = Duration::from_millis(2000);
const KILL_POLL: Duration = Duration::from_millis(50);

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
///   3. Sibling: same directory as the UI executable (matches install.sh,
///      which drops both binaries into ~/.local/bin)
///   4. `loginext` on $PATH
///   5. Absolute fallbacks — these MUST cover ~/.local/bin because GUI
///      launchers (.desktop, gnome-shell, KDE krunner) typically inherit a
///      minimal PATH that does not include the user's local bin dir.
///      See: https://specifications.freedesktop.org/basedir-spec — XDG does
///      not mandate ~/.local/bin be on PATH for graphical sessions.
///
/// Public so the systemd self-heal in `service.rs` can reuse it: a
/// regenerated unit file's `ExecStart=` must point at exactly the same
/// binary the spawn-detached path would launch, otherwise the toggle's
/// two modes diverge in subtle ways (different `--quiet` framing, etc.).
pub fn resolve_daemon_binary() -> Option<PathBuf> {
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

        // Sibling check: install.sh puts loginext + loginext-ui in the same
        // directory (~/.local/bin). When launched from a .desktop file that
        // dir is rarely on PATH, so this branch is the one that actually
        // catches a normal install.
        if let Some(parent) = exe.parent() {
            let sibling = parent.join("loginext");
            if sibling.is_file() {
                return Some(sibling);
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

    // Absolute fallbacks. Order: user-local first (matches install.sh), then
    // system paths. ~/.local/bin is expanded from $HOME because GUI sessions
    // do not always set it on PATH.
    let mut absolute: Vec<PathBuf> = Vec::with_capacity(4);
    if let Ok(home) = std::env::var("HOME") {
        if !home.is_empty() {
            absolute.push(PathBuf::from(format!("{home}/.local/bin/loginext")));
        }
    }
    absolute.push(PathBuf::from("/usr/local/bin/loginext"));
    absolute.push(PathBuf::from("/usr/bin/loginext"));
    for p in absolute {
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

/// Outcome of `kill_daemon()` — surfaced to the UI so it can decide whether
/// to flip the toggle or surface an error toast.
#[derive(Debug, Clone)]
pub enum KillOutcome {
    /// At least one `loginext` process was sent SIGTERM and the socket went
    /// silent within the grace window.
    Killed { pid: u32 },
    /// Socket was already cold and no matching process was found — treat as
    /// success from the UI's perspective.
    NotRunning,
    /// Found a process but couldn't deliver the signal (EPERM/ESRCH).
    SignalFailed { reason: String },
    /// SIGTERM delivered, but the socket never closed within `KILL_GRACE`.
    /// We escalate to SIGKILL once before reporting this — a daemon that
    /// ignores SIGKILL is a kernel-level problem we can't paper over.
    Timeout,
}

/// Locate running `loginext` daemons by walking `/proc`. Tries two methods:
///   1. Resolve `/proc/<pid>/exe` symlink and compare basename to `loginext`.
///   2. Fall back to reading `/proc/<pid>/comm` (world-readable, works when
///      the daemon runs as root via sudo and /exe is unreadable by the user).
fn find_daemon_pids() -> Vec<i32> {
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return out;
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let Some(s) = name.to_str() else { continue };
        let Ok(pid) = s.parse::<i32>() else { continue };

        // Method 1: /proc/<pid>/exe (most reliable but requires same-user)
        let exe = entry.path().join("exe");
        if let Ok(target) = std::fs::read_link(&exe) {
            if target.file_name().and_then(|n| n.to_str()) == Some("loginext") {
                out.push(pid);
                continue;
            }
        }

        // Method 2: /proc/<pid>/comm (world-readable, 16-char truncated name)
        let comm = entry.path().join("comm");
        if let Ok(content) = std::fs::read_to_string(&comm) {
            if content.trim() == "loginext" {
                out.push(pid);
            }
        }
    }
    out
}

/// Ask the daemon to shut itself down via the IPC `quit` command. Returns
/// true if the daemon answered ok and the socket subsequently went silent
/// inside the grace window. False on any error (no socket, malformed reply,
/// daemon ignored the request) — the caller should fall back to signals.
///
/// Why this exists: when the daemon was started under a different uid (e.g.
/// `sudo loginext` for raw /dev/input access), kill(2) returns EPERM and
/// the user is stuck. The UDS, however, is chowned to the invoking user
/// during init (see ipc/server.cpp), so a UI running unprivileged can still
/// reach it. Routing the stop through IPC sidesteps the uid-mismatch trap
/// entirely.
fn quit_via_ipc(path: &Path) -> bool {
    let stream = match UnixStream::connect(path) {
        Ok(s) => s,
        Err(_) => return false,
    };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let _ = stream.set_write_timeout(Some(Duration::from_millis(500)));

    let mut writer = &stream;
    if writer.write_all(b"{\"cmd\":\"quit\"}\n").is_err() {
        return false;
    }
    let mut reader = BufReader::new(&stream);
    let mut resp = String::new();
    if reader.read_line(&mut resp).is_err() {
        return false;
    }
    // Don't hard-fail on the parse — the daemon's reply schema is small and
    // an "ok":true substring match is robust enough for a fire-and-poll
    // shutdown. The deadline below is the real success signal.
    if !resp.contains("\"ok\":true") {
        return false;
    }

    let deadline = Instant::now() + KILL_GRACE;
    while Instant::now() < deadline {
        if !socket_alive(path) {
            return true;
        }
        std::thread::sleep(KILL_POLL);
    }
    false
}

/// Stop every running `loginext` daemon. Tries cooperative IPC first, then
/// SIGTERM, then SIGKILL — only the first path works when the daemon was
/// launched under a different uid (sudo), and the kernel-level signals are
/// kept as a fallback for a wedged daemon that ignored its IPC.
pub fn kill_daemon() -> KillOutcome {
    let path = socket_path();
    let pids = find_daemon_pids();

    if pids.is_empty() && !socket_alive(&path) {
        return KillOutcome::NotRunning;
    }

    // Cooperative shutdown — works regardless of who owns the daemon
    // process, because the listener UDS has the invoking user's perms.
    if socket_alive(&path) && quit_via_ipc(&path) {
        let primary = pids.first().map(|&p| p as u32).unwrap_or(0);
        eprintln!("[loginext-ui] daemon: stopped via IPC quit (pid={primary})");
        return KillOutcome::Killed { pid: primary };
    }

    // No PID but the socket is still warm and IPC quit didn't take it down
    // — something is listening that we genuinely cannot reach. Bail rather
    // than lie about success.
    if pids.is_empty() {
        return KillOutcome::SignalFailed {
            reason: "socket alive but no matching loginext process found".to_string(),
        };
    }

    let primary = pids[0] as u32;
    let mut last_err: Option<String> = None;
    let mut any_signaled = false;
    for &pid in &pids {
        let rc = unsafe { kill(pid, SIGTERM) };
        if rc == 0 {
            any_signaled = true;
        } else {
            last_err = Some(format!("pid={pid}: {}", io::Error::last_os_error()));
        }
    }

    // Poll the socket — once the daemon's epoll loop exits and the listener
    // is dropped, connect() starts refusing. Cheaper and more reliable than
    // waitpid(), which wouldn't work anyway since we're not the parent.
    if any_signaled {
        let deadline = Instant::now() + KILL_GRACE;
        while Instant::now() < deadline {
            if !socket_alive(&path) {
                eprintln!("[loginext-ui] daemon: terminated pid={primary}");
                return KillOutcome::Killed { pid: primary };
            }
            std::thread::sleep(KILL_POLL);
        }
    }

    // Daemon stuck or SIGTERM was rejected (cross-uid) — escalate to
    // SIGKILL. If this also EPERMs, surface a clean message instead of the
    // raw errno: the user needs to know it's a permissions problem so they
    // can stop the daemon manually (or run the UI as the same uid).
    let mut any_killed = false;
    for &pid in &pids {
        let rc = unsafe { kill(pid, SIGKILL) };
        if rc == 0 {
            any_killed = true;
        } else {
            last_err = Some(format!("pid={pid} SIGKILL: {}", io::Error::last_os_error()));
        }
    }
    if any_killed {
        let kill_deadline = Instant::now() + KILL_GRACE;
        while Instant::now() < kill_deadline {
            if !socket_alive(&path) {
                eprintln!("[loginext-ui] daemon: SIGKILL'd pid={primary}");
                return KillOutcome::Killed { pid: primary };
            }
            std::thread::sleep(KILL_POLL);
        }
    }

    if let Some(reason) = last_err {
        // Most common failure here is EPERM — the daemon is owned by a
        // different uid and the IPC path also failed. Translate it once so
        // the UI toast is actionable.
        if reason.contains("Operation not permitted") || reason.contains("os error 1") {
            return KillOutcome::SignalFailed {
                reason: format!(
                    "daemon owned by another user — start it as your user (avoid `sudo loginext`) or stop it manually. Underlying: {reason}"
                ),
            };
        }
        KillOutcome::SignalFailed { reason }
    } else {
        KillOutcome::Timeout
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

