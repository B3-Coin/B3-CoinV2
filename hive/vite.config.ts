import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

// Two entries with no shared code: the main window and the approval window.
// Tauri serves dist/ in release builds and the dev server during `tauri dev`;
// nothing here is reachable from outside.
export default defineConfig({
  root: "web",
  clearScreen: false,
  build: {
    outDir: "../dist",
    emptyOutDir: true,
    target: "es2022",
    rollupOptions: {
      input: {
        main: fileURLToPath(new URL("./web/index.html", import.meta.url)),
        approval: fileURLToPath(new URL("./web/approval.html", import.meta.url)),
      },
    },
  },
  server: {
    host: "127.0.0.1",
    port: 1420,
    strictPort: true,
  },
});
