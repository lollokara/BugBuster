import { defineConfig, loadEnv } from "vite";
import preact from "@preact/preset-vite";
import path from "node:path";

/**
 * BugBuster web UI — Vite build config.
 *
 * Output lands in ../data/ so `pio run -t uploadfs` picks it up without
 * extra wiring. During development, set BB_DEV_TARGET to your ESP32 IP
 * (e.g. BB_DEV_TARGET=http://192.168.1.42) so /api calls are proxied
 * to a real board while the UI runs with hot reload.
 */
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "BB_");
  const target = env.BB_DEV_TARGET ?? "http://192.168.1.1";

  return {
    plugins: [preact()],
    root: ".",
    base: "./",
    build: {
      outDir: path.resolve(__dirname, "../data"),
      emptyOutDir: true,
      assetsDir: "assets",
      sourcemap: false,
      minify: "esbuild",
      target: "es2020",
      rollupOptions: {
        output: {
          // Keep filenames short — SPIFFS has a 32-char path limit (ESP-IDF
          // CONFIG_SPIFFS_OBJ_NAME_LEN default). Long fontsource names like
          // `jetbrains-mono-latin-wght-normal-<hash>.woff2` (56 chars) are
          // rejected by mkspiffs with SPIFFS_ERR_NAME_TOO_LONG.
          entryFileNames: "assets/app-[hash].js",
          chunkFileNames: "assets/c-[hash].js",
          // Pull extension from [ext]; drop the source [name] entirely.
          assetFileNames: (info) => {
            const ext = info.name?.split(".").pop() ?? "bin";
            if (ext === "css") return "assets/app-[hash].css";
            if (ext === "woff2" || ext === "woff") return `assets/f-[hash].${ext}`;
            return `assets/a-[hash].${ext}`;
          },
        },
      },
    },
    server: {
      port: 5173,
      strictPort: true,
      proxy: {
        "/api": { target, changeOrigin: true, secure: false },
      },
    },
  };
});
