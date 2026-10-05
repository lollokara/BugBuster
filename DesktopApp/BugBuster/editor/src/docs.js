// Docs helpers over the shared catalogue: flatten, search and example/API insertion planning.
// Everything returned is plain JSON-serialisable data (safe to pass across wasm-bindgen).

import { ScriptBindings } from "./completion.js";

const KIND_ORDER = { module: 0, namespace: 1, class: 1, function: 2, method: 3, constant: 4, example: 5 };

function paramView(p) {
  return {
    name: p.name,
    kind: p.kind,
    annotation: p.annotation,
    default: p.defaultValue,
    required: p.isRequired,
    doc: p.doc,
  };
}

function fnFields(fn) {
  const d = fn.detail;
  return {
    signature: fn.signature,
    summary: fn.summary,
    description: d.description,
    doc: fn.doc,
    params: fn.params.filter((p) => p.name !== "self").map(paramView),
    returns: fn.returns,
    returnsDoc: d.returnsDoc,
    returnKeys: d.returnKeys.map((k) => ({ ...k })),
    raises: d.raises.map((r) => ({ ...r })),
    notes: d.notes,
    examples: [...d.examples],
  };
}

function containerFields(item) {
  const d = item.detail;
  return {
    summary: item.summary,
    description: d.description,
    doc: item.doc,
    params: [],
    returns: null,
    returnsDoc: "",
    returnKeys: [],
    raises: d.raises.map((r) => ({ ...r })),
    notes: d.notes,
    examples: [...d.examples],
  };
}

/** Call text with required arguments as plain names: `sleep(ms)`, `new(name, chem)`. */
function callText(name, fn) {
  const required = (fn?.params ?? []).filter((p) => p.name !== "self" && p.isRequired);
  return `${name}(${required.map((p) => (p.kind === "keyword_only" ? `${p.name}=${p.name}` : p.name)).join(", ")})`;
}

/** Flatten the catalogue into searchable doc entries. */
export function flatten(catalogue) {
  if (!catalogue) return [];
  const out = [];
  for (const mod of catalogue.modules) {
    const imp = `import ${mod.name}`;
    out.push({
      id: mod.name,
      kind: "module",
      path: mod.name,
      name: mod.name,
      module: mod.name,
      owner: null,
      ...containerFields(mod),
      signature: imp,
      imports: [imp],
      insertText: imp,
    });
    for (const c of mod.constants) {
      out.push(constantEntry(c, mod.name, `${mod.name}.${c.name}`, mod.name, imp));
    }
    for (const fn of mod.functions) {
      out.push({
        id: `${mod.name}.${fn.name}`,
        kind: "function",
        path: `${mod.name}.${fn.name}`,
        name: fn.name,
        module: mod.name,
        owner: mod.name,
        ...fnFields(fn),
        imports: [imp],
        insertText: `${mod.name}.${callText(fn.name, fn)}`,
      });
    }
    for (const cls of mod.classes) {
      const path = `${mod.name}.${cls.name}`;
      const isClass = cls.kind === "class";
      out.push({
        id: path,
        kind: cls.kind,
        path,
        name: cls.name,
        module: mod.name,
        owner: mod.name,
        ...(isClass && cls.constructor_ ? { ...fnFields(cls.constructor_), ...classOverrides(cls) } : containerFields(cls)),
        signature: isClass ? cls.constructorSignature : `${path}`,
        imports: [imp],
        insertText: isClass ? `${mod.name}.${callText(cls.name, cls.constructor_)}` : path,
      });
      for (const c of cls.constants) out.push(constantEntry(c, mod.name, `${path}.${c.name}`, path, imp));
      for (const fn of cls.methods) {
        if (fn.name === "__init__") continue;
        const receiver = isClass ? cls.name.toLowerCase() : path;
        out.push({
          id: `${path}.${fn.name}`,
          kind: "method",
          path: `${path}.${fn.name}`,
          name: fn.name,
          module: mod.name,
          owner: path,
          ...fnFields(fn),
          imports: [imp],
          insertText: `${receiver}.${callText(fn.name, fn)}`,
        });
      }
    }
  }
  for (const ex of catalogue.examples) {
    out.push({
      id: `example:${ex.name}`,
      kind: "example",
      path: `example:${ex.name}`,
      name: ex.name,
      module: null,
      owner: null,
      signature: ex.name,
      summary: ex.title,
      description: "",
      doc: ex.doc,
      params: [],
      returns: null,
      returnsDoc: "",
      returnKeys: [],
      raises: [],
      notes: "",
      examples: [],
      title: ex.title,
      source: ex.source,
      imports: sourceImports(ex.source),
      insertText: ex.source,
    });
  }
  return out;
}

function classOverrides(cls) {
  const d = cls.detail;
  return {
    summary: cls.summary,
    description: d.description || cls.constructor_?.detail.description || "",
    doc: cls.doc || cls.constructor_?.doc || "",
    raises: [...d.raises, ...(cls.constructor_?.detail.raises ?? [])].map((r) => ({ ...r })),
    notes: d.notes || cls.constructor_?.detail.notes || "",
    examples: d.examples.length ? [...d.examples] : [...(cls.constructor_?.detail.examples ?? [])],
  };
}

function constantEntry(c, moduleName, path, owner, imp) {
  return {
    id: path,
    kind: "constant",
    path,
    name: c.name,
    module: moduleName,
    owner,
    signature: `${c.name}${c.annotation ? `: ${c.annotation}` : ""}${c.value !== null ? ` = ${c.value}` : ""}`,
    summary: c.doc.split("\n")[0] ?? "",
    description: "",
    doc: c.doc,
    params: [],
    returns: null,
    returnsDoc: "",
    returnKeys: [],
    raises: [],
    notes: "",
    examples: [],
    value: c.value,
    annotation: c.annotation,
    imports: [imp],
    insertText: path,
  };
}

function searchScore(entry, tokens) {
  const name = entry.name.toLowerCase();
  const path = entry.path.toLowerCase();
  const summary = entry.summary.toLowerCase();
  const doc = entry.doc.toLowerCase();
  const sig = entry.signature.toLowerCase();
  let total = 0;
  for (const t of tokens) {
    let s = 0;
    if (name === t) s = 100;
    else if (name.startsWith(t)) s = 80;
    else if (path.startsWith(t)) s = 70;
    else if (name.split("_").some((seg) => seg.startsWith(t))) s = 60;
    else if (name.includes(t)) s = 50;
    else if (path.includes(t)) s = 40;
    else if (sig.includes(t)) s = 30;
    else if (summary.includes(t)) s = 20;
    else if (doc.includes(t)) s = 10;
    else if (entry.params.some((p) => p.name.toLowerCase().includes(t))) s = 15;
    if (s === 0) return 0; // every token must match somewhere
    total += s;
  }
  return total;
}

/** Search entries; an empty query lists everything in catalogue order. kinds limits the result. */
export function search(entries, query, { kinds = null, limit = 200 } = {}) {
  const tokens = String(query ?? "").toLowerCase().split(/\s+/).filter(Boolean);
  const pool = kinds ? entries.filter((e) => kinds.includes(e.kind)) : entries;
  if (tokens.length === 0) return pool.slice(0, limit);
  const scored = [];
  pool.forEach((e, i) => {
    const s = searchScore(e, tokens);
    if (s > 0) scored.push({ e, s, i });
  });
  scored.sort((a, b) => b.s - a.s || KIND_ORDER[a.e.kind] - KIND_ORDER[b.e.kind] || a.i - b.i);
  return scored.slice(0, limit).map((x) => x.e);
}

// ---------------------------------------------------------------------------------------------
// Insertion planning

const IMPORT_LINE = /^\s*(?:import\s+[A-Za-z_][\w.]*(?:\s+as\s+\w+)?(?:\s*,\s*[A-Za-z_][\w.]*(?:\s+as\s+\w+)?)*|from\s+[A-Za-z_][\w.]*\s+import\s+[^#]+)\s*(?:#.*)?$/;

/** Leading module docstring + import lines of an example source. */
export function splitExample(source) {
  const lines = source.replace(/\r\n/g, "\n").split("\n");
  let i = 0;
  while (i < lines.length && (lines[i].trim() === "" || lines[i].startsWith("#"))) i++;
  const q = /^\s*[rRbBuU]?("""|''')/.exec(lines[i] ?? "");
  let docstring = "";
  if (q) {
    const start = i;
    const rest = lines[i].slice(lines[i].indexOf(q[1]) + 3);
    if (!rest.includes(q[1])) {
      i++;
      while (i < lines.length && !lines[i].includes(q[1])) i++;
    }
    i++;
    docstring = lines.slice(start, i).join("\n");
  }
  const imports = [];
  let j = i;
  while (j < lines.length) {
    const l = lines[j];
    if (l.trim() === "") {
      j++;
      continue;
    }
    if (IMPORT_LINE.test(l)) {
      imports.push(l.trim());
      j++;
    } else {
      break;
    }
  }
  return { docstring, imports, body: lines.slice(j).join("\n").replace(/^\n+/, "").replace(/\s+$/, "") };
}

function sourceImports(source) {
  return splitExample(source).imports;
}

/** Offset where a new import line belongs: after the last header import, else after the docstring. */
export function importPosition(source) {
  const lines = source.split("\n");
  const lineEnd = (idx) => lines.slice(0, idx + 1).reduce((n, l) => n + l.length + 1, 0);
  let i = 0;
  let headerEnd = 0;
  while (i < lines.length && (lines[i].trim() === "" || lines[i].trim().startsWith("#"))) i++;
  const q = /^\s*[rRbBuU]?("""|''')/.exec(lines[i] ?? "");
  if (q) {
    const rest = lines[i].slice(lines[i].indexOf(q[1]) + 3);
    if (!rest.includes(q[1])) {
      i++;
      while (i < lines.length && !lines[i].includes(q[1])) i++;
    }
    headerEnd = lineEnd(Math.min(i, lines.length - 1));
    i++;
  }
  let last = -1;
  while (i < lines.length) {
    const l = lines[i];
    if (/^\s*(import\s|from\s)/.test(l)) {
      let depth = (l.match(/\(/g) ?? []).length - (l.match(/\)/g) ?? []).length;
      while (depth > 0 && i + 1 < lines.length) {
        i++;
        depth += (lines[i].match(/\(/g) ?? []).length - (lines[i].match(/\)/g) ?? []).length;
      }
      last = i;
      i++;
    } else if (l.trim() === "" || l.trim().startsWith("#")) {
      i++;
    } else {
      break;
    }
  }
  return { offset: Math.min(last >= 0 ? lineEnd(last) : headerEnd, source.length), hasImports: last >= 0 };
}

function satisfied(line, bindings) {
  const t = line.trim();
  let m = /^import\s+([A-Za-z_][\w.]*)(?:\s+as\s+(\w+))?$/.exec(t);
  if (m) {
    const local = m[2] ?? m[1].split(".")[0];
    return bindings.aliases.has(local) && (m[2] ? bindings.aliases.get(local) === m[1] : true);
  }
  m = /^from\s+([A-Za-z_][\w.]*)\s+import\s+(.+)$/.exec(t);
  if (m) {
    const names = m[2].replace(/[()]/g, "").split(",").map((s) => s.trim()).filter(Boolean);
    return names.every((n) => {
      const [name, , alias] = n.split(/\s+/);
      return bindings.aliases.get(alias ?? name) === `${m[1]}.${name}`;
    });
  }
  return false;
}

/**
 * Plan inserting a doc entry (or example) into `source`. Returns changes in original-document
 * coordinates: missing import lines at the import block, the body at the selection. Imports already
 * satisfied by the script are skipped.
 *
 * @param {string} source current script
 * @param {object} entry an entry from flatten()
 * @param {{from:number,to:number}} selection where the body goes (default: end of source)
 * @param {import('./catalogue.js').FirmwareAPIIndex|null} index used to resolve existing imports
 */
export function planInsert(source, entry, selection, index) {
  const sel = selection ?? { from: source.length, to: source.length };
  const bindings = index ? ScriptBindings.scan(source, index) : new ScriptBindings();
  let imports;
  let body;
  const emptyDoc = source.trim() === "";
  if (entry.kind === "example") {
    const parts = splitExample(entry.source);
    if (emptyDoc) {
      const text = entry.source.endsWith("\n") ? entry.source : `${entry.source}\n`;
      return { missingImports: parts.imports, changes: [{ from: 0, to: source.length, insert: text }], body: text };
    }
    imports = parts.imports;
    body = parts.body;
  } else {
    imports = entry.imports ?? [];
    body = entry.insertText;
  }
  const missing = imports.filter((l) => !satisfied(l, bindings));
  const changes = [];
  if (missing.length) {
    const { offset, hasImports } = importPosition(source);
    const lead = offset > 0 && source[offset - 1] !== "\n" ? "\n" : "";
    const gap = !hasImports && source.slice(offset).trim() !== "" ? "\n" : "";
    changes.push({ from: offset, to: offset, insert: `${lead}${missing.join("\n")}\n${gap}` });
  }
  const lineStart = source.lastIndexOf("\n", sel.from - 1) + 1;
  const before = source.slice(lineStart, sel.from);
  const prefix = before.trim() !== "" && body.includes("\n") ? "\n" : "";
  const suffix = entry.kind === "example" || (body.includes("\n") && !body.endsWith("\n")) ? "\n" : "";
  changes.push({ from: sel.from, to: sel.to, insert: prefix + body + suffix });
  return { missingImports: missing, changes, body };
}
