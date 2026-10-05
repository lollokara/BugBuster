// Browser entry: window.bbScriptEditor and window.bbScriptCatalogue.
import { SUPPORTED_SCHEMA } from "./catalogue.js";
import { flatten, planInsert, search, splitExample } from "./docs.js";
import { mount } from "./editor.js";
import { defaultCatalogueUrl, store } from "./store.js";

// document.currentScript is only valid while this file is first evaluated.
const scriptUrl = defaultCatalogueUrl();

let flatCache = { catalogue: null, entries: [] };
function entries() {
  if (flatCache.catalogue !== store.catalogue) flatCache = { catalogue: store.catalogue, entries: flatten(store.catalogue) };
  return flatCache.entries;
}

const clone = (v) => JSON.parse(JSON.stringify(v));

const catalogueApi = {
  schema: SUPPORTED_SCHEMA,
  /** {state: empty|loading|ready|failed, code: ''|invalid|unsupported-schema|network|http, message, url, schema, moduleCount, exampleCount} */
  status: () => ({ ...store.status }),
  /** The parsed catalogue JSON as loaded (schema, modules, examples), or null. */
  get data() {
    return store.raw;
  },
  toJSON: () => store.raw,
  load: (url) => store.load(url ?? scriptUrl),
  set: (jsonOrText) => store.set(jsonOrText),
  onChange: (fn) => store.onChange(fn),
  /** All doc entries (module, class, namespace, function, method, constant, example). */
  flatten: () => clone(entries()),
  /** Ranked search over flatten(); opts {kinds:[...], limit}. */
  search: (query, opts) => clone(search(entries(), query, opts)),
  /** Look up one entry by id/path, e.g. "daq.run.new" or "example:background_logger.py". */
  entry: (idOrPath) => {
    const e = entries().find((x) => x.id === idOrPath || x.path === idOrPath);
    return e ? clone(e) : null;
  },
  examples: () => clone(store.catalogue?.examples ?? []),
  /** Split an example into docstring, import lines and body. */
  splitExample,
  /** Plan inserting an entry into source: {missingImports, changes:[{from,to,insert}], body}. */
  planInsert: (source, entryOrId, selection) => {
    const entry = typeof entryOrId === "string" ? entries().find((x) => x.id === entryOrId || x.path === entryOrId) : entryOrId;
    return entry ? clone(planInsert(String(source ?? ""), entry, selection, store.index)) : null;
  },
};

window.bbScriptCatalogue = catalogueApi;
window.bbScriptEditor = { version: 1, mount };
