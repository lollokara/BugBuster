// Pure completion engine for the script editor. A line-for-line port of the iOS
// ScriptCompletion.swift (same contexts, ranking, placeholders), so the two clients agree.
// Offsets are UTF-16 code units, which is what JS strings and CodeMirror use natively.

import { FirmwareAPIIndex } from "./catalogue.js";
import { BUILTINS, builtinNamed } from "./builtins.js";

export const EMPTY_RESULT = Object.freeze({ replaceRange: { location: 0, length: 0 }, items: [] });

// ---------------------------------------------------------------------------------------------
// Lexing: which characters are code (not inside a comment or string, including triple-quoted)

/** mask[i] = 1 for code characters; state = what the end of `text` is inside of. */
export function lexScan(text) {
  const n = text.length;
  const mask = new Uint8Array(n);
  let state = "code";
  let quote = "";
  let triple = false;
  let i = 0;
  while (i < n) {
    const ch = text[i];
    if (state === "code") {
      if (ch === "#") {
        state = "comment";
        i++;
      } else if (ch === '"' || ch === "'") {
        quote = ch;
        triple = text.startsWith(ch.repeat(3), i);
        state = "string";
        i += triple ? 3 : 1;
      } else {
        mask[i++] = 1;
      }
    } else if (state === "comment") {
      if (ch === "\n") {
        state = "code";
        mask[i] = 1;
      }
      i++;
    } else if (ch === "\\") {
      i += 2;
    } else if (triple) {
      if (text.startsWith(quote.repeat(3), i)) {
        state = "code";
        i += 3;
      } else {
        i++;
      }
    } else if (ch === quote) {
      state = "code";
      i++;
    } else if (ch === "\n") {
      state = "code";
      mask[i++] = 1;
    } else {
      i++;
    }
  }
  return { mask, state };
}

const isIdentChar = (c) => /[A-Za-z0-9_]/.test(c);

/** Trailing `[A-Za-z0-9_]` run (plus `.` when allowDots). ASCII only. */
export function trailing(s, allowDots) {
  let i = s.length;
  while (i > 0 && (isIdentChar(s[i - 1]) || (allowDots && s[i - 1] === "."))) i--;
  return s.slice(i);
}

// ---------------------------------------------------------------------------------------------
// Bindings: names a script binds to catalogue objects, found with line regexes (no parser)

const ASSIGNMENT = /^([A-Za-z_]\w*)\s*=\s*([A-Za-z_][\w.]*)\s*\(/;
const WITH_AS = /^with\s+([A-Za-z_][\w.]*)\s*\(.*\)\s+as\s+([A-Za-z_]\w*)\s*:/;

export class ScriptBindings {
  constructor() {
    this.aliases = new Map(); // local name -> catalogue path
    this.instances = new Map(); // local variable -> class path
  }

  /** `bb.Channel` -> `bugbuster.Channel`; from-imported `Channel` -> `bugbuster.Channel`. */
  qualify(dotted) {
    const parts = dotted.split(".");
    const alias = this.aliases.get(parts[0]);
    if (alias === undefined) return dotted;
    parts[0] = alias;
    return parts.join(".");
  }

  static scan(text, index) {
    const b = new ScriptBindings();
    for (const raw of text.split(/\r?\n/)) {
      const line = raw.trim();
      if (line === "") continue;
      if (line.startsWith("import ")) {
        for (const item of stripComment(line).slice("import ".length).split(",")) {
          const words = item.split(" ").filter(Boolean);
          if (words.length === 0) continue;
          if (words.length === 3 && words[1] === "as") b.aliases.set(words[2], words[0]);
          else if (words.length === 1) b.aliases.set(words[0].split(".")[0], words[0].split(".")[0]);
        }
      } else if (line.startsWith("from ") && line.includes(" import ")) {
        const code = stripComment(line);
        const at = code.indexOf(" import ");
        if (at < 0) continue;
        const module = code.slice(5, at).trim();
        const names = code.slice(at + " import ".length).replaceAll("(", "").replaceAll(")", "");
        for (const item of names.split(",")) {
          const words = item.split(" ").filter(Boolean);
          if (words.length === 0) continue;
          const name = words[0];
          const mod = index.module(module);
          if (name === "*" && mod) {
            for (const member of [...mod.functions, ...mod.classes, ...mod.constants]) {
              b.aliases.set(member.name, `${module}.${member.name}`);
            }
          } else if (words.length === 3 && words[1] === "as") {
            b.aliases.set(words[2], `${module}.${name}`);
          } else if (words.length === 1) {
            b.aliases.set(name, `${module}.${name}`);
          }
        }
      } else {
        const m = ASSIGNMENT.exec(line);
        const w = m ? null : WITH_AS.exec(line);
        const bound = m ? [m[1], m[2]] : w ? [w[2], w[1]] : null;
        if (bound) {
          const cls = index.instanceTypeProducedBy(b.qualify(bound[1]));
          if (cls) b.instances.set(bound[0], cls);
        }
      }
    }
    return b;
  }
}

function stripComment(line) {
  const at = line.indexOf("#");
  return at < 0 ? line : line.slice(0, at).trim();
}

// ---------------------------------------------------------------------------------------------
// Where the caret is

const FROM_IMPORT = /^\s*from\s+([A-Za-z_][\w.]*)\s+import\s+\(?\s*(?:[A-Za-z_]\w*(?:\s+as\s+\w+)?\s*,\s*)*$/;
const IMPORT_LIST = /^\s*(?:import\s+(?:[A-Za-z_][\w.]*(?:\s+as\s+\w+)?\s*,\s*)*|from\s+)$/;
const KEYWORD_ARGUMENT = /^\s*([A-Za-z_]\w*)\s*=(?!=)/;

/** The innermost unclosed `(` in text[start..], honouring strings and comments via mask. */
function openParenIndex(text, mask, start) {
  const stack = [];
  for (let i = start; i < text.length; i++) {
    if (!mask[i]) continue;
    const ch = text[i];
    if (ch === "(" || ch === "[" || ch === "{") stack.push([ch, i]);
    else if ((ch === ")" || ch === "]" || ch === "}") && stack.length) stack.pop();
  }
  const last = stack[stack.length - 1];
  return last && last[0] === "(" ? last[1] : -1;
}

function splitTopLevel(text, mask, from) {
  const parts = [];
  let current = "";
  let depth = 0;
  for (let i = from; i < text.length; i++) {
    const ch = text[i];
    if (mask[i]) {
      if (ch === "(" || ch === "[" || ch === "{") depth++;
      else if (ch === ")" || ch === "]" || ch === "}") depth--;
      else if (ch === "," && depth === 0) {
        parts.push(current);
        current = "";
        continue;
      }
    }
    current += ch;
  }
  parts.push(current);
  return parts;
}

/** The innermost unclosed call before the caret: callee text, and its argument slices so far. */
function openCall(before, lex) {
  const start = Math.max(0, before.length - 2000);
  const open = openParenIndex(before, lex.mask, start);
  if (open < 0) return null;
  const callee = trailing(before.slice(start, open), true);
  if (!/^[A-Za-z_]/.test(callee)) return null;
  return { callee, args: splitTopLevel(before, lex.mask, open + 1) };
}

export function enclosingCall(before, lex = lexScan(before)) {
  const call = openCall(before, lex);
  if (!call) return null;
  const current = call.args[call.args.length - 1];
  if (current.trim() !== "") return null;
  let positional = 0;
  const keywords = new Set();
  for (const arg of call.args.slice(0, -1)) {
    const k = KEYWORD_ARGUMENT.exec(arg);
    if (k) keywords.add(k[1]);
    else if (arg.trim() !== "") positional++;
  }
  return { callee: call.callee, positional, keywords };
}

/** The completion context at `caret` and the range of the typed partial. */
export function findContext(text, caret, { explicit = false } = {}) {
  const c = Math.min(Math.max(0, caret), text.length);
  const prefix = text.slice(0, c);
  const partial = trailing(prefix, false);
  const range = { location: c - partial.length, length: partial.length };
  const before = prefix.slice(0, prefix.length - partial.length);
  const line = before.slice(before.lastIndexOf("\n") + 1);
  const lex = lexScan(before);
  const nothing = [{ kind: "nothing" }, range];

  if (lex.state !== "code" || /^[0-9]/.test(partial)) return nothing;

  if (before.endsWith(".")) {
    const receiver = trailing(before.slice(0, -1), true);
    if (!/^[A-Za-z_]/.test(receiver)) return nothing;
    return [{ kind: "member", receiver, partial }, range];
  }
  const from = FROM_IMPORT.exec(line);
  if (from) return [{ kind: "member", receiver: from[1], partial }, range];
  if (IMPORT_LIST.test(line)) return [{ kind: "importModule", partial }, range];
  const call = enclosingCall(before, lex);
  if (call) {
    return [{ kind: "argument", callee: call.callee, partial, positional: call.positional, used: call.keywords }, range];
  }
  if (partial.length >= (explicit ? 1 : 2)) return [{ kind: "identifier", partial }, range];
  return nothing;
}

// ---------------------------------------------------------------------------------------------
// Items

function item(label, insertText, caretOffset, detail, doc, kind, extra = {}) {
  return { label, insertText, caretOffset, detail, doc, kind, placeholders: [], isCall: false, id: `${kind}:${label}`, ...extra };
}

/** The same item inserting only its name (parentheses already typed, or used as a class). */
export function nameOnly(it) {
  if (!it.isCall) return it;
  return item(it.label, it.label, it.label.length, it.detail, it.doc, it.kind, { fn: it.fn, ref: it.ref });
}

/**
 * The call to insert. Required arguments become placeholders (`sleep(ms)`, keyword-only ones as
 * `name=name`); optional ones are left to keyword completion. With no required argument it is
 * `name()`: caret inside the parens when the callable takes optional arguments, after them when none.
 */
export function callItem(label, fn, detail, doc, kind, ref) {
  const params = (fn?.params ?? []).filter((p) => p.name !== "self");
  const required = params.filter((p) => p.isRequired);
  let text = `${label}(`;
  const placeholders = [];
  required.forEach((param, i) => {
    if (i > 0) text += ", ";
    if (param.kind === "keyword_only") text += `${param.name}=`;
    placeholders.push({ location: text.length, length: param.name.length });
    text += param.name;
  });
  text += ")";
  const caret = placeholders.length ? placeholders[0].location : label.length + (params.length === 0 ? 2 : 1);
  return item(label, text, caret, detail, doc, kind, { placeholders, isCall: true, fn: fn ?? null, ref });
}

function constantItem(constant, ref) {
  return item(constant.name, constant.name, constant.name.length, constant.annotation ?? "constant", constant.doc, "constant", { ref });
}

/** Candidates for the members of a resolved target. */
export function membersOf(index, target) {
  const out = [];
  if (target.kind === "module") {
    const mod = index.module(target.path);
    if (!mod) return out;
    for (const cls of mod.classes) {
      const ref = `${mod.name}.${cls.name}`;
      if (cls.kind === "namespace") {
        out.push({ item: item(cls.name, cls.name, cls.name.length, "namespace", cls.doc, "namespace", { ref }), order: 0 });
      } else {
        out.push({ item: callItem(cls.name, cls.constructor_, cls.constructorSignature, cls.doc, "type", ref), order: 0 });
      }
    }
    for (const fn of mod.functions) {
      out.push({ item: callItem(fn.name, fn, fn.signature, fn.doc, "function", `${mod.name}.${fn.name}`), order: 1000 });
    }
    for (const c of mod.constants) out.push({ item: constantItem(c, `${mod.name}.${c.name}`), order: 2000 });
  } else {
    const cls = index.type(target.path);
    if (!cls) return out;
    for (const fn of cls.methods) {
      if (fn.name === "__init__") continue;
      out.push({ item: callItem(fn.name, fn, fn.signature, fn.doc, "method", `${target.path}.${fn.name}`), order: 1000 });
    }
    for (const c of cls.constants) out.push({ item: constantItem(c, `${target.path}.${c.name}`), order: 2000 });
  }
  return out;
}

/** `name=` items for the parameters still open in a call. */
export function keywordItems(fn, positionalCount, used) {
  const out = [];
  let positionalSeen = 0;
  fn.params.forEach((param, i) => {
    if (param.name === "self") return;
    if (param.kind === "positional") {
      positionalSeen++;
      if (positionalSeen <= positionalCount) return;
    }
    if (!param.acceptsKeyword || used.has(param.name)) return;
    const label = `${param.name}=`;
    let detail = param.annotation ?? "";
    if (param.defaultValue !== null) detail += detail === "" ? `= ${param.defaultValue}` : ` = ${param.defaultValue}`;
    out.push({
      item: item(label, label, label.length, detail, param.doc || fn.doc, "keyword", { fn }),
      order: (param.isRequired ? 1000 : 0) + i,
    });
  });
  return out;
}

// ---------------------------------------------------------------------------------------------
// Ranking: lower is better. 0 case-sensitive prefix, 1 case-insensitive prefix, 2 prefix of a later
// `_` segment. Then `order`, then lower-cased label. `_`-prefixed names show only after a `_`.

export function score(label, partial) {
  if (partial === "" || label.startsWith(partial)) return 0;
  const l = label.toLowerCase();
  const p = partial.toLowerCase();
  if (l.startsWith(p)) return 1;
  if (l.split("_").slice(1).some((seg) => seg !== "" && seg.startsWith(p))) return 2;
  return null;
}

export function rank(candidates, partial, limit) {
  const showPrivate = partial.startsWith("_");
  const scored = [];
  for (const c of candidates) {
    if (c.item.label.startsWith("_") && !showPrivate) continue;
    const s = score(c.item.label, partial);
    if (s !== null) scored.push({ item: c.item, score: s, order: c.order });
  }
  scored.sort((a, b) => {
    if (a.score !== b.score) return a.score - b.score;
    if (a.order !== b.order) return a.order - b.order;
    const la = a.item.label.toLowerCase();
    const lb = b.item.label.toLowerCase();
    return la < lb ? -1 : la > lb ? 1 : 0;
  });
  return scored.slice(0, limit).map((s) => s.item);
}

// ---------------------------------------------------------------------------------------------
// Engine

const builtinOrder = (kind) => (kind === "function" || kind === "type" ? 4000 : kind === "exception" ? 4500 : 5000);

function builtinItem(b) {
  if (b.kind === "function" || b.kind === "type") {
    return callItem(b.name, b.function, b.signature, b.doc, "builtin", b.name);
  }
  const kind = b.kind === "exception" ? "exception" : "reserved";
  return item(b.name, b.name, b.name.length, b.kind === "exception" ? "exception" : "keyword", b.doc, kind);
}

export class ScriptCompletionEngine {
  /** @param {import('./catalogue.js').FirmwareAPIIndex} index */
  constructor(index) {
    this.index = index;
  }

  static fromCatalogue(catalogue) {
    return new ScriptCompletionEngine(new FirmwareAPIIndex(catalogue));
  }

  bindings(text) {
    return ScriptBindings.scan(text, this.index);
  }

  complete(text, caret, { limit = 40, explicit = false } = {}) {
    const [context, range] = findContext(text, caret, { explicit });
    if (context.kind === "nothing") return EMPTY_RESULT;
    const bindings = this.bindings(text);
    const index = this.index;
    let partial;
    let candidates;
    switch (context.kind) {
      case "member": {
        partial = context.partial;
        const target = index.resolve(context.receiver, bindings);
        if (!target) return EMPTY_RESULT;
        candidates = membersOf(index, target);
        break;
      }
      case "importModule":
        partial = context.partial;
        candidates = index.moduleNames.map((m) => ({ item: this.moduleItem(m, m), order: 0 }));
        break;
      case "argument": {
        partial = context.partial;
        const fn = index.callable(context.callee, bindings) ?? builtinNamed(context.callee)?.function ?? null;
        candidates = fn ? keywordItems(fn, context.positional, context.used) : [];
        if (rank(candidates, partial, 1).length === 0) {
          if (partial.length < (explicit ? 1 : 2)) return EMPTY_RESULT;
          candidates = this.identifierCandidates(bindings); // a positional name, not a kwarg
        }
        break;
      }
      default:
        partial = context.partial;
        candidates = this.identifierCandidates(bindings);
    }
    let items = rank(candidates, partial, limit);
    if (keepsBareName(text, range, context) || nextIsOpenParen(text, range.location + range.length)) {
      items = items.map(nameOnly);
    }
    if (items.length === 0 || (items.length === 1 && items[0].insertText === partial)) return EMPTY_RESULT;
    return { replaceRange: range, items };
  }

  moduleItem(module, label) {
    return item(label, label, label.length, label === module ? "module" : `module ${module}`, this.index.module(module)?.doc ?? "", "module", {
      ref: module,
    });
  }

  /** Names bound by imports, then modules not imported yet, then Python built-ins. */
  identifierCandidates(bindings) {
    const index = this.index;
    const out = [];
    for (const [local, qualified] of bindings.aliases) {
      const target = index.targetForQualified(qualified);
      if (target?.kind === "module") {
        out.push({ item: this.moduleItem(qualified, local), order: 0 });
      } else if (target?.kind === "type") {
        const cls = index.type(target.path);
        if (cls && cls.kind === "class") {
          out.push({ item: callItem(local, cls.constructor_, cls.constructorSignature, cls.doc, "type", target.path), order: 0 });
        } else {
          out.push({ item: item(local, local, local.length, "namespace", cls?.doc ?? "", "namespace", { ref: target.path }), order: 0 });
        }
      } else {
        const fn = index.callableQualified(qualified);
        if (fn) out.push({ item: callItem(local, fn, fn.signature, fn.doc, "function", qualified), order: 1000 });
        else out.push({ item: item(local, local, local.length, qualified, "", "constant", { ref: qualified }), order: 2000 });
      }
    }
    for (const m of index.moduleNames) {
      if (!bindings.aliases.has(m)) out.push({ item: this.moduleItem(m, m), order: 3000 });
    }
    for (const b of BUILTINS) {
      if (!bindings.aliases.has(b.name)) out.push({ item: builtinItem(b), order: builtinOrder(b.kind) });
    }
    return out;
  }

  /** Signature help for the innermost open call at the caret, or null. */
  signatureAt(text, caret) {
    const c = Math.min(Math.max(0, caret), text.length);
    const before = text.slice(0, c);
    const lex = lexScan(before);
    if (lex.state !== "code") return null;
    const call = openCall(before, lex);
    if (!call) return null;
    const bindings = this.bindings(text);
    const fn = this.index.callable(call.callee, bindings) ?? builtinNamed(call.callee)?.function ?? null;
    if (!fn) return null;
    const params = fn.params.filter((p) => p.name !== "self");
    const argIndex = call.args.length - 1;
    const kw = KEYWORD_ARGUMENT.exec(call.args[argIndex]);
    let active = null;
    if (kw && params.some((p) => p.acceptsKeyword && p.name === kw[1])) {
      active = kw[1];
    } else {
      const positional = params.filter((p) => p.kind === "positional" || p.kind === "var_positional");
      const p = positional[argIndex] ?? positional.find((q) => q.kind === "var_positional");
      active = p && !call.args.slice(0, argIndex).some((a) => KEYWORD_ARGUMENT.test(a)) ? p.name : null;
    }
    return { callee: call.callee, fn, signature: this.displaySignature(fn, call.callee, bindings), params, activeParam: active, argIndex };
  }

  displaySignature(fn, callee, bindings) {
    if (fn.name !== "__init__") return fn.signature;
    const cls = this.constructorOwner(fn, callee, bindings);
    return cls ? cls.constructorSignature : fn.signature;
  }

  constructorOwner(fn, callee, bindings) {
    const q = bindings.qualify(callee);
    const parts = q.split(".");
    return parts.length === 2 ? this.index.type(q) : null;
  }
}

function nextIsOpenParen(text, offset) {
  return offset < text.length && text[offset] === "(";
}

/** Class positions take the bare name: `isinstance(x, int)`, `except OSError:`. */
function keepsBareName(text, range, context) {
  if (context.kind === "argument" && (context.callee === "isinstance" || context.callee === "issubclass")) return true;
  const before = text.slice(0, range.location);
  const line = before.slice(before.lastIndexOf("\n") + 1);
  return /^\s*except\s+[\w.,\s(]*$/.test(line);
}
