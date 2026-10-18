import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

// One entry per window, with no shared code: main, approval, passphrase and
// console.
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
        passphrase: fileURLToPath(new URL("./web/passphrase.html", import.meta.url)),
        console: fileURLToPath(new URL("./web/console.html", import.meta.url)),
      },
    },
  },
  server: {
    host: "127.0.0.1",
    port: 1420,
    strictPort: true,
  },
});
