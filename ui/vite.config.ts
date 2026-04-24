import { defineConfig } from "vite";

// Tauri embeds a fixed web view and expects assets on a known port/host.
// Docs: https://tauri.app/v2/guides/getting-started/setup/vite/
export default defineConfig({
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    host: "127.0.0.1",
  },
  envPrefix: ["VITE_", "TAURI_"],
  build: {
    target: "esnext",
    minify: false,          // faster dev iteration; Tauri already ships compressed
    sourcemap: true,
    outDir: "dist",
    emptyOutDir: true,
  },
});
