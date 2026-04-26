# LogiNext

**A userspace Linux daemon that gives Logitech mice the per-control customisation Logitech Options+ offers on Windows and macOS — starting with a polished, gesture-aware tab switcher on the MX Master 3S thumb wheel.**

There is no first-party Logitech software for Linux. `solaar` manages the device; `libinput` handles raw scrolling. Neither lets you say *"map this button to that action, and behave differently when Firefox is in front."* LogiNext fills that gap with a low-latency C++ daemon plus a separate Tauri-based UI: the daemon takes the device with an exclusive grab, transforms its events according to the presets you wired in the UI, and re-injects them through `uinput`.

The UI launches the daemon on demand and detaches from it. Once the daemon is running, closing the UI does not stop it; reopening the UI silently reconnects.

---

## Status

| Phase | What it covers | State |
|---|---|---|
| 1 | Thumb wheel → `Ctrl+Tab` / `Ctrl+Shift+Tab`, three sensitivity profiles, gesture heuristics | ✅ shipped |
| 2 | Neumorphism dark UI, daemon spawn/respawn lifecycle, per-control preset assignment | 🚧 in progress |
| 3 | Other MX Master 3S controls (Back/Forward, gesture button, vertical wheel) + new presets (volume, zoom, custom keystroke) | planned |

Detailed roadmap: [progress.md](./progress.md). Shipped fixes: [CHANGELOG.md](./CHANGELOG.md).

---

## Quick start (Arch / CachyOS)

```bash
git clone https://github.com/vseprr/loginext.git
cd loginext
./deploy/install.sh
```

The script installs all `pacman` dependencies, builds the daemon and the Tauri UI in release mode, drops both binaries into `~/.local/bin`, registers an icon and `.desktop` entry, and stages the systemd user unit. After it finishes, **LogiNext appears in your application menu** — search for it and launch like any other GUI app. No manual build steps; the long-form sections below are for contributors and packagers.

Pass `--enable-service` to also enable `loginext.service` so the daemon auto-starts at login (otherwise the UI spawns it on demand, which is the recommended workflow).

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

---

## Requirements

- Linux with `uinput` enabled (`CONFIG_INPUT_UINPUT=y` or built as a module).
- `libevdev` ≥ 1.13.
- CMake ≥ 3.25, Ninja, GCC 14+ or Clang 18+.
- Read access to `/dev/input/eventX` and read/write access to `/dev/uinput`. Either:
  - membership in the `input` group, **or**
  - a udev rule granting `uinput` group access, **or**
  - run with `sudo` for testing.

For the UI: Node 20+ and a recent Rust toolchain (Tauri 2.x).

Arch / CachyOS:

```bash
sudo pacman -S --needed cmake ninja libevdev gcc pkgconf nodejs npm rustup
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
npm run tauri dev   # development with hot-reload
# or
npm run tauri build # release bundle
```

The UI looks for the daemon binary in this order: `$LOGINEXT_DAEMON` (absolute path), then `../../build/loginext` relative to the UI executable (the dev workflow), then `loginext` on `$PATH`, then `/usr/local/bin/loginext`, then `/usr/bin/loginext`.

---

## Run

### From the UI (recommended)

```bash
cd ui
npm run tauri dev
```

The Tauri shell probes `$XDG_RUNTIME_DIR/loginext.sock`. If the socket isn't alive, it spawns the daemon as a fully detached background process (`setsid`, stdio → `/dev/null`). Closing the UI window does **not** kill the daemon — reopening reconnects to the existing socket.

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
| `--help` | Show usage and exit |

`SIGHUP` reloads the config file without restarting.

---

## Configuration

Default path: `$XDG_CONFIG_HOME/loginext/config.json` (falls back to `~/.config/loginext/config.json`).

```jsonc
{
    "sensitivity": "medium",   // "low" | "medium" | "high"
    "invert_hwheel": true      // true is recommended for the MX Master 3S
}
```

A starter file lives at [config/example.json](./config/example.json).

The UI writes this file on every change and asks the daemon to reload — you do not have to edit JSON by hand.

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

### Find the socket and verify it's alive

```bash
echo "$XDG_RUNTIME_DIR/loginext.sock"
nc -U "$XDG_RUNTIME_DIR/loginext.sock" <<< '{"cmd":"ping"}'
# expected: {"ok":true,"v":1}
```

### UI / Daemon lifecycle (what to expect)

| Event | What happens |
|---|---|
| You launch the UI | Tauri probes the socket. If alive → connect. If dead → spawn daemon detached, wait up to 3 s for the socket. |
| You close the UI window | Daemon keeps running. Process tree: daemon's parent has been re-parented to init/systemd via `setsid`. |
| You reopen the UI | Socket probe succeeds → instant reconnect. Initial state fetched from the daemon, no flicker. |
| Daemon crashes mid-session | Heartbeat detects within 2–5 s, status bar flips to `daemon: unreachable`, Tauri runs the spawn check again. Backoff: 2 s → 4 s → 8 s → 16 s → 30 s (capped). |
| You change a setting in the UI | UI writes `config.json`, sends `reload`, daemon acks only after the new settings are live. |

### Optional: run the daemon as a systemd user service

The default workflow (UI spawns daemon on demand) needs no system integration. If you want the daemon to start at login regardless of whether the UI is opened:

```bash
mkdir -p ~/.config/systemd/user
cp deploy/systemd/loginext.service ~/.config/systemd/user/
# Edit ExecStart= if `loginext` is not on $PATH, then:
systemctl --user daemon-reload
systemctl --user enable --now loginext.service
journalctl --user -u loginext.service -f
```

`SIGHUP`-style reloads still work via `systemctl --user reload loginext.service`.

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
├── config/       # constants, profiles, CLI args, JSON loader
├── ipc/          # UDS server + dispatch
├── util/         # logger
└── main.cpp
ui/
├── src/          # vanilla TS — components, views, ipc client
└── src-tauri/    # Rust shell — daemon spawn, IPC bridge
deploy/
├── systemd/      # user-unit template
└── scripts/      # loginext-logs (tail helper)
config/example.json
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
