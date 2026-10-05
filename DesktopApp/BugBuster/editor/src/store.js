// Catalogue store: loads/validates the shared firmware API catalogue and exposes the engine.
// A missing or invalid catalogue leaves completion/docs empty with a labelled status; it never throws.

import { FirmwareAPIIndex, SUPPORTED_SCHEMA, tryDecode } from "./catalogue.js";
import { ScriptCompletionEngine } from "./completion.js";

const listeners = new Set();
let loadSeq = 0;
let current = { state: "empty", code: "", message: "No catalogue loaded", url: "", schema: "", moduleCount: 0, exampleCount: 0 };
let raw = null;
let catalogue = null;
let engine = null;

function publish(next) {
  current = next;
  for (const fn of [...listeners]) {
    try {
      fn(current);
    } catch (e) {
      console.error("bbScriptCatalogue listener failed", e);
    }
  }
}

export function defaultCatalogueUrl() {
  const src = typeof document !== "undefined" ? document.currentScript?.src : "";
  return src ? new URL("firmware-api.json", src).href : "public/firmware-api.json";
}

export const store = {
  get status() {
    return current;
  },
  get raw() {
    return raw;
  },
  get catalogue() {
    return catalogue;
  },
  get engine() {
    return engine;
  },
  get index() {
    return engine?.index ?? null;
  },
  onChange(fn) {
    listeners.add(fn);
    return () => listeners.delete(fn);
  },

  /** Install a parsed object or JSON text. Returns the resulting status. */
  set(input, url = "") {
    loadSeq++;
    const decoded = tryDecode(input);
    if (decoded.ok) {
      catalogue = decoded.catalogue;
      raw = typeof input === "string" ? JSON.parse(input) : input;
      engine = new ScriptCompletionEngine(new FirmwareAPIIndex(catalogue));
      publish({
        state: "ready",
        code: "",
        message: "",
        url,
        schema: SUPPORTED_SCHEMA,
        moduleCount: catalogue.modules.length,
        exampleCount: catalogue.examples.length,
      });
    } else {
      catalogue = null;
      raw = null;
      engine = null;
      publish({ state: "failed", code: decoded.error.code, message: decoded.error.message, url, schema: "", moduleCount: 0, exampleCount: 0 });
    }
    return current;
  },

  /** Fetch and install the catalogue; a stale response never overwrites a newer one. */
  async load(url = defaultCatalogueUrl()) {
    const seq = ++loadSeq;
    publish({ ...current, state: "loading", code: "", message: "Loading API catalogue", url });
    let text;
    try {
      const res = await fetch(url, { cache: "no-store" });
      if (!res.ok) throw Object.assign(new Error(`HTTP ${res.status}`), { code: "http" });
      text = await res.text();
    } catch (e) {
      if (seq === loadSeq) {
        catalogue = null;
        raw = null;
        engine = null;
        publish({
          state: "failed",
          code: e.code ?? "network",
          message: `API catalogue unavailable: ${e.message ?? e}`,
          url,
          schema: "",
          moduleCount: 0,
          exampleCount: 0,
        });
      }
      return current;
    }
    if (seq !== loadSeq) return current;
    const status = store.set(text, url);
    loadSeq = seq;
    return status;
  },
};
