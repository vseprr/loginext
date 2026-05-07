# LogiNext

**A userspace Linux daemon that gives Logitech mice the per-control customisation Logitech Options+ offers on Windows and macOS — starting with a polished, gesture-aware tab switcher on the MX Master 3S thumb wheel.**

There is no first-party Logitech software for Linux. `solaar` manages the device; `libinput` handles raw scrolling. Neither lets you say *"map this button to that action, and behave differently when Firefox is in front."* LogiNext fills that gap with a low-latency C++ daemon plus a separate Tauri-based UI: the daemon takes the device with an exclusive grab, transforms its events according to the presets you wired in the UI, and re-injects them through `uinput`.

The UI launches the daemon on demand and detaches from it. Once the daemon is running, closing the UI does not stop it; reopening the UI silently reconnects.

---

## Status — v1.0.0 stable

| Phase | What it covers | State |
|---|---|---|
| 1 | Thumb wheel → `Ctrl+Tab` / `Ctrl+Shift+Tab`, three sensitivity profiles, gesture heuristics | ✅ v1.0.0 |
| 2 | Neumorphism dark UI, systemd-driven daemon lifecycle, per-control preset assignment, Zoom + passthrough presets, per-app rules with per-app sensitivity / invert overrides, KWin DBus focus bridge for Plasma 6 (with cold-boot auto-bootstrap), udev rules for unprivileged hardware access, native Arch PKGBUILD with auto KPackage registration, always-on-top pin, secure `RuntimeDirectory=`-managed IPC socket | ✅ v1.0.0 |
| 3 | Other MX Master 3S controls (Back / Forward, gesture button, vertical wheel, Mode-shift) + new preset families (volume, custom keystroke, run command) | 🚧 planned |

v1.0.0 closes the cold-boot race that previously required the user to launch the UI before per-app rules would fire, the listener-thread CPU spinner that triggered systemd-oomd kills, and the IPC socket timeout the UI hit on slow boots. Per-app rules now apply within ~50 ms of daemon start, with no UI launch required. Idle CPU is ~40 ms per minute.

Detailed roadmap: [progress.md](./progress.md). Release-by-release feature history: [CHANGELOG.md](./CHANGELOG.md). Architectural decisions and deferred audit findings: [KNOWN_ISSUES.md](./KNOWN_ISSUES.md).

---

## Quick start (Arch / CachyOS)

Two equivalent paths, pick one:

**A — pacman-managed `-git` package (recommended on Arch).**

```bash
git clone https://github.com/vseprr/loginext.git
cd loginext/deploy
makepkg -si
```

The PKGBUILD is a `-git` recipe (`pkgver()` resolves at build time from `git describe`), so future updates are just `git pull` followed by `makepkg -si` — pacman cleanly upgrades the installed copy. To remove: `sudo pacman -R loginext-git`.

**B — script install (any distro with the dependencies).**

```bash
git clone https://github.com/vseprr/loginext.git
cd loginext
./deploy/install.sh
```

Installs `pacman` deps, builds the daemon and the Tauri UI in release mode, drops both binaries into `~/.local/bin`, registers an icon and `.desktop` entry, stages the systemd user unit.

Either way, **LogiNext appears in your application menu** — search for it and launch like any other GUI app. The script enables `loginext.service` as a systemd `--user` unit by default, so the daemon auto-starts at every login and per-app rules apply immediately on cold boot without you having to open the UI first. Pass `--no-enable` to `install.sh` (or `systemctl --user disable --now loginext.service` after a package install) if you'd rather have the UI spawn the daemon on demand.

---

## Highlights (Phase 1)

- **Auto-detected** MX Master 3S over Bolt (`046d:b034`) or USB (`046d:c548`).
- **Exclusive grab** of the thumb wheel; no other process sees the raw events.
- **Gesture-aware heuristics:** leaky-bucket accumulator, velocity-aware threshold (slow tick → exactly one tab; fast swipe → smooth multi-tab), idle reset, leading-edge confirmation window (filters the 1-mm ghost moves a finger resting on the wheel produces), emit cooldown, ring-buffer pacing, damping.
- **Three sensitivity profiles** (`low` / `medium` / `high`), each tunable independently.
- **Hot reload** via `SIGHUP` — config changes apply without restarting the daemon.
- **Passthrough.** Every event other than the thumb wheel (clicks, motion, vertical scroll) is forwarded as-is on a virtual mouse, so the rest of the device behaves normally.
- **Zero heap allocation** in the event loop.
- **Single binary**, the only runtime dependency is `libevdev`.
- **Cold-boot KWin bootstrap.** The listener thread doesn't depend on session env vars (`XDG_CURRENT_DESKTOP`, `WAYLAND_DISPLAY`, `DISPLAY`) — those aren't exported into systemd-user when our service starts at boot. Instead, three direct probes (`org.kde.KWin` `NameHasOwner` on the user bus, `stat()` of `$XDG_RUNTIME_DIR/wayland-{0..3}`, `xcb_connect()` retry) run in a 60 s polling loop until a compositor surfaces, then a one-shot inline KWin script (loaded via `org.kde.kwin.Scripting.loadScript`) pushes the current active window straight into the daemon's `Activated` handler — bypassing the persistent `loginext-focus` script if it isn't enabled in `kwinrc`. End-to-end: per-app rules apply within ~50 ms of the daemon binding its bus name, with no UI launch and no manual `kwriteconfig` step required.

---

## Requirements

- Linux with `uinput` enabled (`CONFIG_INPUT_UINPUT=y` or built as a module).
- `libevdev` ≥ 1.13.
- `libxcb` + `xcb-util-wm` (Phase 2.5 active-window listener for X11 sessions; absent on a pure-Wayland host the X11 backend just stays idle, but the build still requires the headers).
- CMake ≥ 3.25, Ninja, GCC 14+ or Clang 18+.
- Read access to `/dev/input/eventX` and read/write access to `/dev/uinput`. Both `makepkg -si` and `deploy/install.sh` install [deploy/udev/99-loginext.rules](./deploy/udev/99-loginext.rules) for you, which grants the active session user ACL access via `TAG+="uaccess"` and falls back to `GROUP="input"` for non-logind setups. **No `sudo` for the daemon** — see the "Run unprivileged" section below for why and what to do if you see `permission denied`.

For the UI: Node 20+ and a recent Rust toolchain (Tauri 2.x).

Arch / CachyOS:

```bash
sudo pacman -S --needed cmake ninja libevdev libxcb xcb-util-wm gcc pkgconf nodejs npm rustup
rustup default stable
```

---

## Build

### Daemon

```bash
git clone https://github.com/vseprr/loginext.git
cd loginext
cmake -S . -B build -G Ninja
cmake --build build
```

The build is `-O2 -Wall -Wextra -Wpedantic -Werror` and must finish clean. LTO and `-march=native` are on by default; disable with `-DLOGINEXT_LTO=OFF -DLOGINEXT_NATIVE=OFF` for distribution builds.

### UI

```bash
cd ui
npm install
npm run tauri dev    # development with hot-reload
# or
npm run tauri:build  # release bundle (composite: vite build → tauri build)
```

`tauri:build` chains the Vite step explicitly. **Do not invoke `cargo build` directly inside `ui/src-tauri/`** — it produces a binary that loads from `devUrl` (port 1420) and renders an empty webview unless `npm run dev` is also running. Foot-gun documented in [ui/src-tauri/README.md](./ui/src-tauri/README.md).

The UI looks for the daemon binary in this order: `$LOGINEXT_DAEMON` (absolute path), then `../../build/loginext` relative to the UI executable (the dev workflow), then `loginext` on `$PATH`, then `/usr/local/bin/loginext`, then `/usr/bin/loginext`.

---

## Run

### From the UI (recommended)

```bash
cd ui
npm run tauri dev
```

The Tauri shell probes `$XDG_RUNTIME_DIR/loginext.sock`. If the socket isn't alive, it spawns the daemon as a fully detached background process (`setsid`, stdio → `/dev/null`). Closing the UI window does **not** kill the daemon — reopening reconnects to the existing socket.

**KDE Plasma developers running `tauri dev`:** the package install path normally registers the focus-bridge KWin script via `kpackagetool6`, but a dev build hasn't been packaged yet. The Tauri shell auto-runs `kpackagetool6 --type KWin/Script --upgrade <path>` against the source tree at `deploy/kwin/loginext-focus/` on every launch, falling back to a direct copy into `~/.local/share/kwin/scripts/loginext-focus/` if the tooling isn't available. If you skip the UI entirely (running just the daemon for kernel-side debugging), you'll need to register the script by hand the first time:

```bash
kpackagetool6 --type KWin/Script --install deploy/kwin/loginext-focus
qdbus6 org.kde.KWin /KWin reconfigure
```

Without that step the daemon's KWin DBus listener binds successfully but never receives `Activated()` calls; it'll log a 30 s "ZERO Activated() calls received" warning to point you back here.

### From the terminal (for development / debugging)

```bash
sudo ./build/loginext --mode=medium
```

`sudo` is only needed if the running user lacks `/dev/input` access. CLI flags:

| Flag | Effect |
|---|---|
| `--mode=low\|medium\|high` | Override the sensitivity profile |
| `--invert=true\|false` | Override the axis-invert flag |
| `--config=PATH` | Use a non-default JSON config |
| `--quiet` | Suppress stderr (file log keeps recording) |
| `--verbose` | Lower the file-log threshold to Trace (per-event detail) |
| `--debug-events` | Dump raw libevdev events to stderr (hardware discovery; use with the UI in SYSTEM OFFLINE) |
| `--debug-perf` | Per-second wakeup / event / sd_bus_process counters from both threads (catches busy-loop regressions) |
| `--help` | Show usage and exit |

`SIGHUP` reloads the config file without restarting.

---

## Configuration

Default path: `$XDG_CONFIG_HOME/loginext/config.json` (falls back to `~/.config/loginext/config.json`).

```jsonc
{
    "sensitivity": "medium",   // "low" | "medium" | "high"
    "invert_hwheel": true,     // true is recommended for the MX Master 3S
    "active_preset": "tab_nav" // "tab_nav" | "zoom" | "none" (passthrough)
}
```

A starter file lives at [config/example.json](./config/example.json).

The UI writes this file on every change and asks the daemon to reload — you do not have to edit JSON by hand.

### Per-application rules (Phase 2.5+)

A sidecar text file at `$XDG_CONFIG_HOME/loginext/app_rules.txt` carries per-app overrides for the global preset, sensitivity, and invert axis:

```text
# Format: <app_id>=<preset>[,<mode>[,<invert>]]
#   preset:  tab_nav | zoom | none | (empty = tracked-only chip)
#   mode:    low | medium | high | (empty = inherit global)
#   invert:  true | false | (empty = inherit global)
firefox=tab_nav
code=zoom,high,true        # zoom preset, high sensitivity, inverted
gimp=zoom,,false           # zoom preset, inherit sensitivity, normal scroll
inkscape=                  # tracked-only chip (UI shows it; daemon ignores)
```

App ids are case-insensitive — they're the X11 `WM_CLASS` (instance name preferred), the Hyprland window class, or the KWin `resourceName`. The daemon hashes them at load time so the hot path only ever runs an integer compare against an atomic published by the active-window listener thread. Each field after `preset` is independent: an empty `mode` falls back to the global sensitivity at the moment of resolution (so a later `kwriteconfig`-style global change automatically propagates); a non-empty `mode` sticks until the user changes the rule.

**UI workflow:** The UI manages everything through a context-aware preset selector. Click the Thumb Wheel control to expose the context bar, then pick "Global" or any per-app chip; the right panel reflects whatever sensitivity and invert that context resolves to, and writes back to the same context. To remove a chip, hover it and click the X that fades in on the right; deselecting a preset only *unbinds* the rule (the chip stays so you can re-bind without re-focusing the app). Clicking an already-active preset on the global context sets `active_preset` to `"none"` (raw passthrough, the thumb wheel emits unprocessed HWHEEL events).

A reference file with common bindings lives at [config/app_rules.example.txt](./config/app_rules.example.txt). Reload after editing with `pkill -HUP loginext` (or any UI action that triggers a reload).

Compositor coverage in v1.0.0: KDE Plasma 6 (KWin D-Bus bridge — primary path on Plasma, sees native Wayland windows that `org_kde_plasma_window_management` no longer exposes to regular clients), KDE Plasma 5 / wlroots-style Wayland (`org_kde_plasma_window_management` v1), Hyprland (`HYPRLAND_INSTANCE_SIGNATURE` → IPC event stream), X11 / XWayland (`_NET_ACTIVE_WINDOW` PropertyNotify). The listener probes them in that priority order with a 60 s grace window, so the daemon catches up on cold boot the moment any compositor surfaces — no env-var dependency.

---

## Troubleshooting & Diagnostics

### Where the logs live

The daemon writes detailed structured logs to:

```
$XDG_STATE_HOME/loginext/daemon.log     # usually $HOME/.local/state/loginext/daemon.log
```

The UI's stderr stays minimal in normal operation: socket path, daemon spawn outcome, connection state, critical errors. Per-event traces never reach stderr — they go to the file log only.

### Stream the daemon log live

```bash
deploy/scripts/loginext-logs           # tail -F the live log
deploy/scripts/loginext-logs -n 200    # last 200 lines, then follow
deploy/scripts/loginext-logs --path    # print the resolved log path
deploy/scripts/loginext-logs --clear   # confirm-then-truncate
```

Or directly:

```bash
tail -F "$XDG_STATE_HOME/loginext/daemon.log"
# or
tail -F "$HOME/.local/state/loginext/daemon.log"
```

For maximum verbosity (per-event traces) restart the daemon with `--verbose`:

```bash
pkill loginext
./build/loginext --verbose --quiet &     # quiet stderr, full file log
```

### Restart / kill the daemon

```bash
pkill -HUP loginext     # in-place config reload — preserves the daemon
pkill loginext          # full restart — UI will respawn it on next ping
```

The UI's DAEMON ONLINE/OFFLINE button drives the same lifecycle through IPC: clicking OFFLINE sends a `quit` command over the listener socket (the daemon flips its stop flag and unwinds cleanly). The cooperative path means the toggle works even when the daemon was started under a different uid (e.g. `sudo loginext`) — kill(2) EPERMs across uid boundaries, the UDS does not. If the IPC round-trip fails the UI falls back to SIGTERM → SIGKILL on the daemon's PID; you only see the EPERM error toast if both paths fail.

### Run unprivileged (the supported path)

The daemon is designed to run as your normal session user — **not** as root. Both pacman (`makepkg -si`) and `deploy/install.sh` install [deploy/udev/99-loginext.rules](./deploy/udev/99-loginext.rules), which:

- tags `/dev/uinput` and the MX Master 3S event nodes (`046d:b034`, `046d:c548`) with `TAG+="uaccess"` so systemd-logind ACL-grants access to whoever is currently logged in on the local seat, **and**
- as a parallel layer, sets `GROUP="input"` `MODE="0660"` so traditional input-group memberships still work on hosts without logind.

After install both paths run `udevadm control --reload-rules && udevadm trigger`, so the rules apply to already-plugged-in receivers without a reboot. If you ever see "permission denied on /dev/input/eventN", **replug the Bolt receiver once** (the easiest way to make logind reapply its uaccess ACL) and you're done.

**Do not use `sudo loginext`.** It's not a supported runtime: the user session's D-Bus broker rejects connections from uid 0 with `EPIPE` during the EXTERNAL auth handshake, so the KWin focus bridge can't bind and per-app rules silently fall back to the global preset. The daemon's listener will log a warning pointing you back here if it detects this.

The IPC `quit` command remains a useful escape hatch even in the unprivileged flow — the UI's DAEMON OFFLINE button uses it preferentially over `kill(2)` so the lifecycle is uid-agnostic.

### Tauri UI on Plasma 6 Wayland (WebKit Error 71)

Some Wayland compositors — Plasma 6 + WebKitGTK 6 in particular — trip a wl_surface protocol error on the default DMA-BUF renderer:

```
Gdk-Message: Error 71 (Protocol error) dispatching to Wayland display.
```

The UI now sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` (and `WEBKIT_DISABLE_COMPOSITING_MODE=1` on Wayland) at the very top of `run()`, before any GTK init runs. Both are set conditionally — if you already exported the env var in a `.desktop` file or your shell, the binary leaves it alone. The PKGBUILD's installed `loginext.desktop` keeps the explicit `env WEBKIT_DISABLE_DMABUF_RENDERER=1` prefix as belt-and-braces, but it's no longer load-bearing for the binary itself.

If you still see Error 71 after this (rare — typically Nvidia + Mesa version mismatch), force the XWayland fallback explicitly:

```bash
GDK_BACKEND=x11 ./loginext-ui
```

This is a heavier hammer (the entire UI runs through XWayland and loses native Wayland niceties) but resolves every renderer-incompatibility issue we have seen in the wild.

### Find the socket and verify it's alive

```bash
echo "$XDG_RUNTIME_DIR/loginext.sock"
nc -U "$XDG_RUNTIME_DIR/loginext.sock" <<< '{"cmd":"ping"}'
# expected: {"ok":true,"v":1}
```

Other one-shot probes through the same socket: `'{"cmd":"get_active_app"}'` reports the focused window as the listener saw it (useful when a per-app rule is misbehaving — the `name` field is the exact key to put in `app_rules.txt`); `'{"cmd":"quit"}'` asks the daemon to shut down gracefully.

### UI / Daemon lifecycle (what to expect)

| Event | What happens |
|---|---|
| You launch the UI | Tauri probes the socket. If alive → connect. If dead → spawn daemon detached, wait up to 3 s for the socket. |
| You close the UI window | Daemon keeps running. Process tree: daemon's parent has been re-parented to init/systemd via `setsid`. |
| You reopen the UI | Socket probe succeeds → instant reconnect. Initial state fetched from the daemon, no flicker. |
| Daemon crashes mid-session | Heartbeat detects within 2–5 s, status bar flips to `daemon: unreachable`, Tauri runs the spawn check again. Backoff: 2 s → 4 s → 8 s → 16 s → 30 s (capped). |
| You change a setting in the UI | UI writes `config.json`, sends `reload`, daemon acks only after the new settings are live. |

### systemd user service (driven by the UI toggle)

Both the PKGBUILD and `deploy/install.sh` ship `loginext.service` as a systemd-user unit:
- pacman: `/usr/lib/systemd/user/loginext.service` (ExecStart=`/usr/bin/loginext --quiet`)
- script install: `~/.config/systemd/user/loginext.service` (ExecStart auto-rewritten to `~/.local/bin/loginext`)

The DAEMON ONLINE/OFFLINE button at the bottom of the UI is now a thin wrapper around the unit. Click ON → `systemctl --user enable --now loginext.service` (starts the daemon AND installs the autostart symlink, so it survives reboots). Click OFF → `systemctl --user disable --now loginext.service` (stops it AND removes the symlink). On UI launch the toggle reads `is-active` + `is-enabled` so its position is the source of truth from systemd, not a localStorage sentinel.

If the unit file isn't installed (older build, manual cmake-only flow), the toggle falls back to the legacy spawn-detached + `kill_daemon` IPC path — no functional regression, you just lose the autostart bit. To opt out manually:

```bash
systemctl --user disable --now loginext.service     # full off
journalctl --user -u loginext.service -f            # live log via journald
systemctl --user reload loginext.service            # SIGHUP-style config reload
```

---

## Sensitivity profiles

| Mode | Feel | When to pick it |
|---|---|---|
| `low` | Very stable; a finger resting on the wheel doesn't trigger. 80 ms leading-edge confirmation. | Browsing one tab at a time. |
| `medium` | Balanced — the default. | Daily use. |
| `high` | Snappy; catches the smallest motion. | Long tab lists, fast scanning. |

Profile parameters live as `constexpr` in [src/config/profile.hpp](./src/config/profile.hpp). The UI exposes the mode picker; future versions will expose individual parameters.

---

## Architecture

```
                ┌──────────────────────────┐
                │  LogiNext UI (Tauri)     │
                │  ─ vanilla TS frontend   │
                │  ─ Rust shell:           │
                │     • daemon.rs (spawn)  │
                │     • ipc_bridge.rs      │
                └────────┬─────────────────┘
                         │  per-request UnixStream
                         │  line-delimited JSON
                         ▼
$XDG_RUNTIME_DIR/loginext.sock
                         │
                         ▼
            ┌─────────────────────────────┐
            │  loginext daemon (C++)      │
            │  single thread,             │
            │  epoll + timerfd + uinput   │
            └────┬───────┬───────┬────────┘
                 │       │       │
   /dev/input/   │       │       │  $XDG_STATE_HOME/loginext/daemon.log
   eventX        │       │       │  (Trace/Debug/Info/Warn/Error)
   (libevdev,    │       │       │
    exclusive    │       │       └──────────────► tail -F or `loginext-logs`
    grab)        │       │
                 │       │
            HWHEEL ev    │ everything else
                 │       │
                 ▼       ▼
   ┌──────────────────┐  ┌────────────────┐
   │ heuristics/      │  │ virtual mouse  │
   │  - accumulator   │  │ (uinput)       │
   │  - velocity      │  └────────────────┘
   │  - confirm win   │
   │  - cooldown      │
   └──────┬───────────┘
          │ ActionResult
          ▼
   ┌──────────────────┐
   │ core/pacer       │
   │  - ring buffer   │
   │  - timerfd       │
   │  - damping       │
   └──────┬───────────┘
          ▼
   ┌──────────────────┐
   │ virtual keyboard │
   │ Ctrl+Tab /       │
   │ Ctrl+Shift+Tab   │
   │ (uinput)         │
   └──────────────────┘
```

Internal rules: [agents.md](./agents.md). Performance discipline: [OPTIMIZATIONS.md](./OPTIMIZATIONS.md). Audit history: [KNOWN_ISSUES.md](./KNOWN_ISSUES.md).

---

## Project layout

```
src/
├── core/         # event loop, device grab, emitter, pacer
├── heuristics/   # scroll state + velocity engine
├── presets/      # constexpr (PresetId, Direction) → KeyCombo table
├── scope/        # per-app O(1) rule table + async active-window listener
├── config/       # constants, profiles, CLI args, JSON loader
├── ipc/          # UDS server + dispatch
├── util/         # logger
└── main.cpp
ui/
├── src/          # vanilla TS — components, views, ipc client
└── src-tauri/    # Rust shell — daemon spawn, IPC bridge
deploy/
├── PKGBUILD     # native Arch -git package recipe
├── install.sh   # script-based install (any distro with deps)
├── systemd/     # user-unit template
└── scripts/     # loginext-logs (tail helper)
config/
├── example.json
└── app_rules.example.txt
```

---

## Contributing

Phase 1 is stable. PRs welcome on Phase 2 / 3 work — read [agents.md](./agents.md) first, in particular the "no framework / no heap on the hot path" rules.

```bash
cmake --build build           # must finish clean under -Werror
./build/loginext --help
```

---

## License

[MIT](./LICENSE).

---

## Disclaimer

LogiNext is **not** affiliated with, endorsed by, or supported by Logitech International S.A. "Logitech", "MX Master", and "Options+" are trademarks of their respective owners and are referenced here only for compatibility purposes.
