import { defineConfig, loadEnv, type Plugin } from "vite";
import preact from "@preact/preset-vite";
import path from "node:path";
import { gzipSync, constants as zlibConstants } from "node:zlib";

/**
 * Hand-rolled gzip emitter — small enough to avoid pulling in another
 * devDep. Only compresses JS/CSS/HTML/SVG/JSON outputs since fonts/woff2
 * are already compressed. Files smaller than 1 KB are skipped (gzip
 * overhead > savings). Each pre-compressed file is written to disk as
 * `<name>.gz` so the firmware can serve it with `Content-Encoding: gzip`
 * by reading the same path with the `.gz` suffix.
 *
 * If this needs to grow (brotli, customizable thresholds, multi-pass), the
 * standard off-the-shelf option is `vite-plugin-compression2`. Keeping this
 * file dependency-free is a deliberate choice — `node:zlib` ships with
 * Node, the function is ~25 lines, and we own the behavior end-to-end.
 */
function gzipEmitter(): Plugin {
  const COMPRESSIBLE = /\.(js|mjs|css|html|svg|json|map|txt)$/i;
  return {
    name: "bb-gzip-emitter",
    apply: "build",
    generateBundle(_options, bundle) {
      for (const fileName of Object.keys(bundle)) {
        if (!COMPRESSIBLE.test(fileName)) continue;
        const item = bundle[fileName]!;
        const source =
          item.type === "asset"
            ? typeof item.source === "string"
              ? Buffer.from(item.source)
              : Buffer.from(item.source)
            : Buffer.from((item as any).code, "utf-8");
        if (source.length < 1024) continue;
        const compressed = gzipSync(source, {
          level: zlibConstants.Z_BEST_COMPRESSION,
        });
        // Skip if compression made it bigger (shouldn't happen for text).
        if (compressed.length >= source.length) continue;
        this.emitFile({
          type: "asset",
          fileName: fileName + ".gz",
          source: compressed,
        });
      }
    },
  };
}

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
    plugins: [preact(), gzipEmitter()],
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
