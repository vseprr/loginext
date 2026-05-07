# loginext-ui (Tauri shell)

Rust crate that hosts the LogiNext control panel webview. Frontend
sources live in `../src/`, daemon sources in `../../src/`. This
directory only owns the Tauri shell, the IPC bridge to the C++
daemon, and the systemd unit-healing logic.

## Building

From the **`ui/` directory** (one level up):

```bash
npm install            # once, or after dependency bumps
npm run tauri:build    # release build of the standalone UI binary
```

The output binary is `ui/src-tauri/target/release/loginext-ui`. It
embeds the Vite-built frontend assets from `ui/dist/` and runs
without a dev server.

For iterative development:

```bash
npm run tauri dev      # hot-reloads frontend; rebuilds Rust on save
```

## Do not invoke `cargo build` directly inside this directory

A common foot-gun: running `cargo build --release` inside `src-tauri/`
produces a binary that tries to load assets from `devUrl`
(`http://127.0.0.1:1420`, defined in `tauri.conf.json`). That URL is
only live while `npm run dev` is running, so a directly-cargo-built
binary renders an empty webview unless the Vite dev server happens to
be up — defeating the whole point of a release build.

The Vite asset build is wired into Tauri's `beforeBuildCommand` and
is only triggered by the Tauri CLI. The `npm run tauri:build`
composite script makes the order explicit:

```
vite build (writes ../dist/index.html + assets/)
   → tauri build (compiles Rust, embeds dist/, links binary)
```

If you find yourself running `cargo build` here for any reason other
than checking compile errors on a Rust-only change, you almost
certainly want `npm run tauri:build` instead.

## Files

- `src/lib.rs` — Tauri command surface, daemon lifecycle entry.
- `src/daemon.rs` — spawn/kill/probe of the C++ daemon binary.
- `src/service.rs` — systemd `--user` unit healing + enable/disable
  (template version is bumped here when the unit body changes).
- `tauri.conf.json` — frontend dist path, dev URL, bundler config.
