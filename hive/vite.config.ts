import { defineConfig } from "vite";

// The main window's bundle. Tauri serves dist/ in release builds and the dev
// server below during `tauri dev`; nothing here is reachable from outside.
export default defineConfig({
  root: "web",
  clearScreen: false,
  build: {
    outDir: "../dist",
    emptyOutDir: true,
    target: "es2022",
  },
  server: {
    host: "127.0.0.1",
    port: 1420,
    strictPort: true,
  },
});
