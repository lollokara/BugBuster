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
        let raw: Buffer =
          item.type === "asset"
            ? typeof item.source === "string"
              ? Buffer.from(item.source)
              : Buffer.from(item.source)
            : Buffer.from((item as any).code, "utf-8");
        // Vite replaces __VITE_PRELOAD__ → [] in the written .js files, but
        // gzipEmitter runs in generateBundle before that substitution.  Do it
        // here so the .gz files match what the browser expects: an empty deps
        // array rather than the __VITE_PRELOAD__ identifier (which would
        // resolve to window.__VITE_PRELOAD__ at runtime and cause a crash if
        // it is a function with .length > 0).
        if (raw.includes("__VITE_PRELOAD__")) {
          raw = Buffer.from(raw.toString("utf-8").replace(/__VITE_PRELOAD__/g, "[]"), "utf-8");
        }
        const source = raw;
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
 * Replace __VITE_PRELOAD__ with a no-op polyfill so that dynamic chunk
 * imports work on the ESP32 where the Vite polyfill chunk is never loaded.
 * Without this the browser throws "ReferenceError: __VITE_PRELOAD__ is not
 * defined" when navigating to any lazy-loaded route.
 *
 * Previously this was done via Vite's `define` option, but esbuild's define
 * rejects arrow-function expressions when processing CommonJS modules (such as
 * @lezer/lr, a CodeMirror transitive dependency). Using a renderChunk plugin
 * instead runs after esbuild and avoids the restriction.
 */
function preloadPolyfill(): Plugin {
  return {
    name: "bb-preload-polyfill",
    apply: "build",
    renderChunk(code) {
      if (code.includes("__VITE_PRELOAD__")) {
        return { code: code.replace(/__VITE_PRELOAD__/g, "((f)=>f())"), map: null };
      }
      return null;
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
    plugins: [preact(), preloadPolyfill(), gzipEmitter()],
    root: ".",
    base: "./",
    build: {
      outDir: path.resolve(__dirname, "../data"),
      emptyOutDir: true,
      assetsDir: "assets",
      sourcemap: false,
      minify: "esbuild",
      target: "es2020",
      // Disable module preload polyfill — ESP32 serves content from SPIFFS and
      // cannot inject the polyfill reliably, causing "__VITE_PRELOAD__ is not
      // defined" at runtime. Dynamic imports still work; they just aren't hinted.
      modulePreload: false,
      rollupOptions: {
        output: {
          // Embed build timestamp so chunk content — and therefore their
          // content-hash filenames — change on every rebuild.  Without this,
          // browsers that previously cached chunks as `immutable` keep serving
          // stale content even after a SPIFFS update.
          banner: `/* bb-build:${Date.now()} */`,
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
