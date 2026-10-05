// Shared firmware API catalogue (schema bugbuster.firmware-api/1): decode, validate and index.
// Both the completion engine and the docs helpers read the normalised form built here.

export const SUPPORTED_SCHEMA = "bugbuster.firmware-api/1";

export class CatalogueError extends Error {
  constructor(code, message) {
    super(message);
    this.name = "CatalogueError";
    this.code = code;
  }
}

const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const str = (v, fallback = "") => (typeof v === "string" ? v : fallback);

function need(obj, key, where) {
  if (!isObj(obj) || typeof obj[key] !== "string" || obj[key] === "") {
    throw new CatalogueError("invalid", `${where}: missing string "${key}"`);
  }
  return obj[key];
}

function list(obj, key, where) {
  const v = obj[key];
  if (v === undefined || v === null) return [];
  if (!Array.isArray(v)) throw new CatalogueError("invalid", `${where}: "${key}" is not an array`);
  return v;
}

function firstLine(doc) {
  return doc.split("\n").find((l) => l.length > 0) ?? "";
}

function decodeDetail(o, where) {
  const keys = list(o, "return_keys", where).map((k, i) => ({
    name: need(k, "name", `${where}.return_keys[${i}]`),
    doc: str(k.doc),
  }));
  const raises = list(o, "raises", where).map((r, i) => ({
    type: need(r, "type", `${where}.raises[${i}]`),
    doc: str(r.doc),
  }));
  return {
    summary: str(o.summary),
    description: str(o.description),
    returnsDoc: str(o.returns_doc),
    returnKeys: keys,
    raises,
    notes: str(o.notes),
    examples: list(o, "examples", where).filter((e) => typeof e === "string"),
  };
}

function withSummary(item) {
  item.summary = item.detail.summary || firstLine(item.doc);
  return item;
}

function decodeParam(p, where) {
  const kind = need(p, "kind", where);
  const param = {
    name: need(p, "name", where),
    kind: ["positional", "keyword_only", "var_positional", "var_keyword"].includes(kind) ? kind : "positional",
    annotation: typeof p.annotation === "string" ? p.annotation : null,
    defaultValue: typeof p.default === "string" ? p.default : null,
    doc: str(p.doc),
  };
  param.acceptsKeyword = param.kind === "positional" || param.kind === "keyword_only";
  param.isRequired = param.defaultValue === null && param.acceptsKeyword;
  return param;
}

function decodeFunction(f, where, owner) {
  const name = need(f, "name", where);
  const fn = {
    name,
    signature: need(f, "signature", `${where}(${name})`),
    params: list(f, "params", `${where}(${name})`).map((p, i) => decodeParam(p, `${where}(${name}).params[${i}]`)),
    returns: typeof f.returns === "string" ? f.returns : null,
    doc: str(f.doc),
    detail: decodeDetail(f, `${where}(${name})`),
    owner,
  };
  return withSummary(fn);
}

function decodeConstant(c, where) {
  const name = need(c, "name", where);
  return {
    name,
    annotation: typeof c.annotation === "string" ? c.annotation : null,
    value: typeof c.value === "string" ? c.value : null,
    doc: str(c.doc),
  };
}

function decodeClass(c, where) {
  const name = need(c, "name", where);
  need(c, "kind", `${where}.${name}`);
  const cls = {
    name,
    kind: c.kind === "namespace" ? "namespace" : "class",
    doc: str(c.doc),
    methods: list(c, "methods", `${where}.${name}`).map((m, i) => decodeFunction(m, `${where}.${name}.methods[${i}]`, name)),
    constants: list(c, "constants", `${where}.${name}`).map((k, i) => decodeConstant(k, `${where}.${name}.constants[${i}]`)),
    detail: decodeDetail(c, `${where}.${name}`),
  };
  cls.constructor_ = cls.kind === "class" ? (cls.methods.find((m) => m.name === "__init__") ?? null) : null;
  cls.constructorSignature = constructorSignature(cls);
  return withSummary(cls);
}

/** `__init__(channel: int) -> None` shown as `Channel(channel: int)`. */
export function constructorSignature(cls) {
  const ctor = cls.constructor_;
  if (!ctor) return `${cls.name}()`;
  let sig = ctor.signature;
  if (sig.startsWith("__init__")) sig = cls.name + sig.slice("__init__".length);
  if (sig.endsWith(" -> None")) sig = sig.slice(0, -" -> None".length);
  return sig;
}

function decodeModule(m, where) {
  const name = need(m, "name", where);
  const mod = {
    name,
    doc: str(m.doc),
    functions: list(m, "functions", name).map((f, i) => decodeFunction(f, `${name}.functions[${i}]`, null)),
    classes: list(m, "classes", name).map((c, i) => decodeClass(c, `${name}.classes[${i}]`)),
    constants: list(m, "constants", name).map((c, i) => decodeConstant(c, `${name}.constants[${i}]`)),
    detail: decodeDetail(m, name),
  };
  return withSummary(mod);
}

/** Validate and normalise a parsed catalogue; throws CatalogueError. */
export function decodeCatalogue(data) {
  if (!isObj(data)) throw new CatalogueError("invalid", "catalogue is not a JSON object");
  const schema = str(data.schema);
  if (schema !== SUPPORTED_SCHEMA) {
    throw new CatalogueError("unsupported-schema", `unsupported catalogue schema "${schema}" (expected ${SUPPORTED_SCHEMA})`);
  }
  if (!Array.isArray(data.modules)) throw new CatalogueError("invalid", 'catalogue has no "modules" array');
  const modules = data.modules.map((m, i) => decodeModule(m, `modules[${i}]`));
  const examples = list(data, "examples", "catalogue").map((e, i) => ({
    name: need(e, "name", `examples[${i}]`),
    title: str(e.title),
    doc: str(e.doc),
    source: need(e, "source", `examples[${i}]`),
  }));
  return { schema, modules, examples };
}

/** Parse JSON text or accept a parsed object; returns {ok, catalogue?, error?} and never throws. */
export function tryDecode(input) {
  try {
    const data = typeof input === "string" ? JSON.parse(input) : input;
    return { ok: true, catalogue: decodeCatalogue(data) };
  } catch (e) {
    if (e instanceof CatalogueError) return { ok: false, error: { code: e.code, message: e.message } };
    return { ok: false, error: { code: "invalid", message: `catalogue is not valid JSON: ${e?.message ?? e}` } };
  }
}

/** Lookup structure over a decoded catalogue. */
export class FirmwareAPIIndex {
  constructor(catalogue) {
    this.catalogue = catalogue;
    this.byName = new Map(catalogue.modules.map((m) => [m.name, m]));
  }

  get moduleNames() {
    return this.catalogue.modules.map((m) => m.name);
  }

  module(name) {
    return this.byName.get(name) ?? null;
  }

  /** "m.C" -> class or namespace C of module m. */
  type(path) {
    const parts = path.split(".");
    if (parts.length !== 2) return null;
    return this.module(parts[0])?.classes.find((c) => c.name === parts[1]) ?? null;
  }

  targetForQualified(path) {
    const parts = path.split(".");
    if (parts.length === 1 && this.module(parts[0])) return { kind: "module", path: parts[0] };
    if (parts.length === 2 && this.type(path)) return { kind: "type", path };
    return null;
  }

  /** A dotted receiver as written in the script: `daq`, `daq.run`, `bb`, `ch`. */
  resolve(expr, bindings) {
    const parts = expr.split(".");
    if (parts.includes("")) return null;
    const head = parts[0];
    let current;
    if (bindings.instances.has(head)) {
      current = { kind: "instance", path: bindings.instances.get(head) };
    } else if (bindings.aliases.has(head) && this.targetForQualified(bindings.aliases.get(head))) {
      current = this.targetForQualified(bindings.aliases.get(head));
    } else if (this.module(head)) {
      current = { kind: "module", path: head };
    } else {
      return null;
    }
    for (const name of parts.slice(1)) {
      if (current.kind !== "module") return null;
      if (!this.module(current.path)?.classes.some((c) => c.name === name)) return null;
      current = { kind: "type", path: `${current.path}.${name}` };
    }
    return current;
  }

  functionNamed(name, moduleName) {
    const mod = this.module(moduleName);
    if (!mod) return null;
    return mod.functions.find((f) => f.name === name) ?? mod.classes.find((c) => c.name === name)?.constructor_ ?? null;
  }

  /** A catalogue path to its callable: `daq.vdut`, `bugbuster.Channel`, `daq.run.new`. */
  callableQualified(qualified) {
    const parts = qualified.split(".");
    if (parts.length === 2) return this.functionNamed(parts[1], parts[0]);
    if (parts.length === 3) return this.type(`${parts[0]}.${parts[1]}`)?.methods.find((m) => m.name === parts[2]) ?? null;
    return null;
  }

  /** The function behind a callee as written: `daq.vdut`, `ch.set_voltage`, `bugbuster.Channel`, `vdut`. */
  callable(expr, bindings) {
    if (bindings.aliases.has(expr)) return this.callableQualified(bindings.aliases.get(expr));
    const dot = expr.lastIndexOf(".");
    if (dot < 0) return null;
    const owner = expr.slice(0, dot);
    const name = expr.slice(dot + 1);
    const target = this.resolve(owner, bindings);
    if (!target) return null;
    if (target.kind === "module") return this.functionNamed(name, target.path);
    return this.type(target.path)?.methods.find((m) => m.name === name) ?? null;
  }

  /** Class path of the value produced by calling `qualified` ("m.f" or "m.C"). */
  instanceTypeProducedBy(qualified) {
    const parts = qualified.split(".");
    if (parts.length !== 2) return null;
    const mod = this.module(parts[0]);
    if (!mod) return null;
    const cls = mod.classes.find((c) => c.name === parts[1]);
    if (cls) return cls.kind === "class" ? qualified : null;
    const returns = mod.functions.find((f) => f.name === parts[1])?.returns;
    if (!returns) return null;
    const bare = returns.replace(" | None", "").trim();
    return mod.classes.some((c) => c.name === bare && c.kind === "class") ? `${parts[0]}.${bare}` : null;
  }
}
