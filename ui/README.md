# LogiNext UI (Phase 2 skeleton)

Tauri 2.x shell + vanilla TypeScript frontend. Neumorphism-dark tokens live in
[`src/style.css`](./src/style.css); the three-column layout is assembled in
[`src/views/main.ts`](./src/views/main.ts).

## Layout

```
ui/
├── index.html
├── package.json
├── tsconfig.json
├── vite.config.ts
├── src/
│   ├── main.ts                # mount point
│   ├── style.css              # design tokens + base components
│   ├── components/            # Card / Toggle / Slider / Segmented
│   ├── views/main.ts          # 3-column: Devices / Controls / Preset
│   └── ipc/client.ts          # typed wrapper around `invoke("ipc_request")`
└── src-tauri/
    ├── Cargo.toml
    ├── tauri.conf.json
    ├── build.rs
    └── src/
        ├── main.rs            # Tauri entry + command registration
        └── ipc_bridge.rs      # UnixStream client → loginext daemon
```

## Running (dev)

```bash
# Terminal 1 — daemon (Phase 1)
cmake --build build && ./build/loginext

# Terminal 2 — UI
cd ui
npm install
npm run tauri dev
```

The daemon exposes a UDS at `$XDG_RUNTIME_DIR/loginext.sock`. The UI connects
per-request (short-lived `UnixStream`) and speaks line-delimited JSON. See
[`../src/ipc/protocol.hpp`](../src/ipc/protocol.hpp) for the wire format.
