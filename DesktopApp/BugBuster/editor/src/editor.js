// CodeMirror 6 script editor. State lives in the editor document; the host is told through onChange.
import { acceptCompletion, autocompletion, closeBrackets, closeBracketsKeymap, completionKeymap, completionStatus, snippet } from "@codemirror/autocomplete";
import { defaultKeymap, history, historyKeymap, indentWithTab, redo, undo } from "@codemirror/commands";
import { bracketMatching, HighlightStyle, indentOnInput, indentUnit, syntaxHighlighting } from "@codemirror/language";
import { python } from "@codemirror/lang-python";
import { lintGutter, lintKeymap, setDiagnostics } from "@codemirror/lint";
import { highlightSelectionMatches, openSearchPanel, search, searchKeymap } from "@codemirror/search";
import { Annotation, Compartment, EditorState, Prec, StateEffect, StateField } from "@codemirror/state";
import { drawSelection, EditorView, highlightActiveLine, highlightActiveLineGutter, highlightSpecialChars, keymap, lineNumbers, showTooltip, tooltips } from "@codemirror/view";
import { tags as t } from "@lezer/highlight";
import { planInsert, flatten } from "./docs.js";
import { store } from "./store.js";

const External = Annotation.define();
const dismissSignature = StateEffect.define();
const catalogueChanged = StateEffect.define();
const instances = new WeakMap();

// ---------------------------------------------------------------------------------------------
// Theme: colours come only from the desktop CSS tokens (styles/tokens.css).

const editorTheme = EditorView.theme({
  "&": {
    height: "100%",
    minWidth: "0",
    color: "var(--label-1)",
    backgroundColor: "var(--surface-1)",
    fontFamily: "var(--font-mono)",
    fontSize: "var(--fs-body)",
  },
  "&.cm-focused": { outline: "none" },
  ".cm-scroller": { fontFamily: "var(--font-mono)", lineHeight: "1.55", overflow: "auto" },
  ".cm-content": { caretColor: "var(--label-1)", padding: "var(--s-2) 0" },
  ".cm-line": { padding: "0 var(--s-3)" },
  ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--label-1)" },
  "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection": {
    backgroundColor: "var(--accent-tint-strong)",
  },
  ".cm-activeLine": { backgroundColor: "var(--fill-1)" },
  ".cm-selectionMatch": { backgroundColor: "var(--tint-yellow)" },
  "&.cm-focused .cm-matchingBracket": { backgroundColor: "var(--fill-3)", outline: "none" },
  "&.cm-focused .cm-nonmatchingBracket": { backgroundColor: "var(--tint-red)" },
  ".cm-gutters": {
    backgroundColor: "var(--surface-2)",
    color: "var(--label-3)",
    border: "none",
    borderRight: "1px solid var(--sep)",
  },
  ".cm-lineNumbers .cm-gutterElement": { padding: "0 var(--s-2) 0 var(--s-3)", minWidth: "3ch" },
  ".cm-activeLineGutter": { backgroundColor: "var(--fill-2)", color: "var(--label-1)" },
  ".cm-panels": { backgroundColor: "var(--surface-2)", color: "var(--label-1)", borderColor: "var(--sep)", fontFamily: "var(--font-ui)", fontSize: "var(--fs-footnote)" },
  ".cm-panels-top": { borderBottom: "1px solid var(--sep)" },
  ".cm-search": { display: "flex", flexWrap: "wrap", alignItems: "center", gap: "var(--s-1) var(--s-2)", padding: "var(--s-2) var(--s-8) var(--s-2) var(--s-3)" },
  ".cm-search br": { display: "none" },
  ".cm-search label": { display: "inline-flex", alignItems: "center", gap: "var(--s-1)", color: "var(--label-2)" },
  ".cm-search input[type=checkbox]": { margin: "0" },
  ".cm-textfield": {
    height: "var(--ctl-h-sm)",
    padding: "0 var(--s-2)",
    color: "var(--label-1)",
    backgroundColor: "var(--surface-1)",
    border: "1px solid var(--sep-strong)",
    borderRadius: "var(--r-sm)",
    fontFamily: "var(--font-ui)",
    fontSize: "var(--fs-footnote)",
    margin: "0",
  },
  ".cm-textfield:focus": { outline: "2px solid var(--accent)", outlineOffset: "-1px" },
  ".cm-button": {
    height: "var(--ctl-h-sm)",
    padding: "0 var(--s-2)",
    color: "var(--label-1)",
    backgroundImage: "none",
    backgroundColor: "var(--fill-2)",
    border: "none",
    borderRadius: "var(--r-sm)",
    fontFamily: "var(--font-ui)",
    fontSize: "var(--fs-footnote)",
    margin: "0",
  },
  ".cm-button:hover": { backgroundColor: "var(--fill-3)" },
  ".cm-button:active": { backgroundImage: "none", backgroundColor: "var(--fill-3)" },
  ".cm-search [name=close]": { color: "var(--label-2)", background: "none", top: "var(--s-2)", right: "var(--s-2)" },
  ".cm-searchMatch": { backgroundColor: "var(--tint-yellow)", outline: "none" },
  ".cm-searchMatch.cm-searchMatch-selected": { backgroundColor: "var(--tint-orange)" },
  ".cm-tooltip": {
    color: "var(--label-1)",
    backgroundColor: "var(--surface-1)",
    border: "1px solid var(--sep-strong)",
    borderRadius: "var(--r-md)",
    boxShadow: "var(--shadow-pop)",
    fontFamily: "var(--font-ui)",
    fontSize: "var(--fs-footnote)",
    overflow: "hidden",
  },
  ".cm-tooltip-autocomplete > ul": { fontFamily: "var(--font-mono)", fontSize: "var(--fs-footnote)", maxHeight: "16em", minWidth: "22em" },
  ".cm-tooltip-autocomplete > ul > li": { padding: "var(--s-1) var(--s-2)", display: "flex", alignItems: "center", lineHeight: "1.4" },
  ".cm-tooltip-autocomplete > ul > li[aria-selected]": { backgroundColor: "var(--accent-tint-strong)", color: "var(--label-1)" },
  ".cm-completionLabel": { color: "var(--label-1)" },
  ".cm-completionMatchedText": { textDecoration: "none", fontWeight: "var(--fw-semibold)", color: "var(--accent-text)" },
  ".cm-completionDetail": { marginLeft: "var(--s-3)", color: "var(--label-3)", fontStyle: "normal", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", maxWidth: "26em", flex: "1 1 auto", textAlign: "right" },
  ".cm-completionIcon": { color: "var(--label-3)", opacity: "1", width: "1.2em", marginRight: "var(--s-1)" },
  ".cm-completionIcon-function, .cm-completionIcon-method": { color: "var(--c-blue-text)" },
  ".cm-completionIcon-class, .cm-completionIcon-type": { color: "var(--c-teal-text)" },
  ".cm-completionIcon-namespace": { color: "var(--c-purple-text)" },
  ".cm-completionIcon-constant": { color: "var(--c-orange-text)" },
  ".cm-completionIcon-property": { color: "var(--c-green-text)" },
  ".cm-completionIcon-keyword": { color: "var(--c-pink-text)" },
  ".cm-tooltip.cm-completionInfo": { padding: "var(--s-2) var(--s-3)", maxWidth: "36em", whiteSpace: "normal", fontFamily: "var(--font-ui)" },
  ".cm-snippetField": { backgroundColor: "var(--accent-tint)" },
  ".bb-sig": { padding: "var(--s-1) var(--s-3)", maxWidth: "46em", whiteSpace: "normal" },
  ".bb-sig-line": { fontFamily: "var(--font-mono)", color: "var(--label-2)", whiteSpace: "pre-wrap", overflowWrap: "anywhere" },
  ".bb-sig-param.is-active": { color: "var(--accent-text)", fontWeight: "var(--fw-semibold)" },
  ".bb-sig-doc": { color: "var(--label-2)", marginTop: "var(--s-1)" },
  ".bb-sig-param-doc": { color: "var(--label-3)", marginTop: "2px" },
  ".bb-info-sig": { fontFamily: "var(--font-mono)", color: "var(--label-1)", whiteSpace: "pre-wrap", overflowWrap: "anywhere" },
  ".bb-info-doc": { color: "var(--label-2)", marginTop: "var(--s-1)" },
  ".cm-diagnostic": { fontFamily: "var(--font-ui)", fontSize: "var(--fs-footnote)", padding: "var(--s-1) var(--s-2)" },
  ".cm-diagnostic-error": { borderLeft: "3px solid var(--c-red)" },
  ".cm-diagnostic-warning": { borderLeft: "3px solid var(--c-orange)" },
  ".cm-diagnostic-info": { borderLeft: "3px solid var(--c-blue)" },
  ".cm-diagnosticSource": { color: "var(--label-3)" },
  ".cm-lintRange-error": { backgroundImage: "none", textDecoration: "underline wavy var(--c-red)", textUnderlineOffset: "3px" },
  ".cm-lintRange-warning": { backgroundImage: "none", textDecoration: "underline wavy var(--c-orange)", textUnderlineOffset: "3px" },
  ".cm-lintRange-info": { backgroundImage: "none", textDecoration: "underline dotted var(--c-blue)", textUnderlineOffset: "3px" },
  ".cm-gutter-lint": { width: "1.1em" },
  ".cm-lint-marker": { width: "0.7em", height: "0.7em" },
});

const highlight = HighlightStyle.define([
  { tag: [t.keyword, t.controlKeyword, t.definitionKeyword, t.moduleKeyword, t.operatorKeyword], color: "var(--c-purple-text)" },
  { tag: [t.string, t.special(t.string)], color: "var(--c-green-text)" },
  { tag: [t.number, t.bool, t.null, t.atom], color: "var(--c-orange-text)" },
  { tag: [t.comment, t.lineComment, t.blockComment], color: "var(--label-3)", fontStyle: "italic" },
  { tag: [t.function(t.variableName), t.function(t.propertyName), t.definition(t.variableName)], color: "var(--c-blue-text)" },
  { tag: [t.className, t.typeName], color: "var(--c-teal-text)" },
  { tag: [t.self, t.standard(t.variableName)], color: "var(--c-indigo-text)" },
  { tag: [t.meta, t.annotation], color: "var(--c-pink-text)" },
  { tag: [t.operator, t.punctuation, t.bracket], color: "var(--label-2)" },
  { tag: t.propertyName, color: "var(--label-1)" },
  { tag: t.invalid, color: "var(--c-red-text)" },
]);

// ---------------------------------------------------------------------------------------------
// Completion UI over the pure engine

const TYPE_FOR_KIND = {
  module: "namespace",
  namespace: "namespace",
  function: "function",
  builtin: "function",
  method: "method",
  type: "class",
  exception: "class",
  constant: "constant",
  keyword: "property",
  reserved: "keyword",
};

function el(tag, cls, text) {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
}

function snippetTemplate(it) {
  const esc = (s) => s.replace(/[\\$#{}]/g, (c) => `\\${c}`);
  let out = "";
  let pos = 0;
  it.placeholders.forEach((p, i) => {
    out += esc(it.insertText.slice(pos, p.location)) + `\${${i + 1}:${esc(it.insertText.slice(p.location, p.location + p.length))}}`;
    pos = p.location + p.length;
  });
  return `${out}${esc(it.insertText.slice(pos))}\${0}`; // ${0}: Tab from the last argument leaves the call
}

function applyFor(it) {
  if (it.placeholders.length) {
    const run = snippet(snippetTemplate(it));
    return (view, completion, from, to) => run(view, completion, from, to);
  }
  return (view, completion, from, to) => {
    view.dispatch({
      changes: { from, to, insert: it.insertText },
      selection: { anchor: from + it.caretOffset },
      userEvent: "input.complete",
    });
  };
}

function infoFor(it) {
  return () => {
    const root = el("div", "bb-info");
    const fn = it.fn;
    root.append(el("div", "bb-info-sig", it.detail || it.label));
    const text = fn?.summary || (it.doc ? it.doc.split("\n")[0] : "");
    if (text) root.append(el("div", "bb-info-doc", text));
    if (fn?.detail?.description) root.append(el("div", "bb-info-doc", fn.detail.description));
    return root;
  };
}

function toOption(it) {
  return { label: it.label, detail: it.detail, type: TYPE_FOR_KIND[it.kind] ?? "text", info: infoFor(it), apply: applyFor(it) };
}

function completionSource(ctx) {
  const engine = store.engine;
  if (!engine) return null;
  const result = engine.complete(ctx.state.doc.toString(), ctx.pos, { explicit: ctx.explicit });
  if (result.items.length === 0) return null;
  return { from: result.replaceRange.location, options: result.items.map(toOption), filter: false };
}

// ---------------------------------------------------------------------------------------------
// Signature help

function splitSignature(sig) {
  const open = sig.indexOf("(");
  if (open < 0) return { head: sig, params: [], tail: "" };
  let depth = 0;
  let close = -1;
  const params = [];
  let start = open + 1;
  for (let i = open; i < sig.length; i++) {
    const ch = sig[i];
    if (ch === "(" || ch === "[" || ch === "{") depth++;
    else if (ch === ")" || ch === "]" || ch === "}") {
      depth--;
      if (depth === 0) {
        close = i;
        break;
      }
    } else if (ch === "," && depth === 1) {
      params.push(sig.slice(start, i));
      start = i + 1;
    }
  }
  if (close < 0) return { head: sig, params: [], tail: "" };
  const last = sig.slice(start, close);
  if (last.trim() !== "") params.push(last);
  return { head: sig.slice(0, open + 1), params, tail: sig.slice(close) };
}

function renderSignature(dom, sig) {
  dom.textContent = "";
  const root = el("div", "bb-sig");
  const line = el("div", "bb-sig-line");
  const { head, params, tail } = splitSignature(sig.signature);
  line.append(document.createTextNode(head));
  params.forEach((seg, i) => {
    if (i > 0) line.append(document.createTextNode(","));
    const name = /^\s*\**\s*([A-Za-z_]\w*)/.exec(seg)?.[1];
    const span = el("span", "bb-sig-param", seg);
    if (name && name === sig.activeParam) span.classList.add("is-active");
    line.append(span);
  });
  line.append(document.createTextNode(tail));
  root.append(line);
  if (sig.fn.summary) root.append(el("div", "bb-sig-doc", sig.fn.summary));
  const active = sig.params.find((p) => p.name === sig.activeParam);
  if (active?.doc) root.append(el("div", "bb-sig-param-doc", `${active.name}: ${active.doc}`));
  dom.append(root);
}

const signatureField = StateField.define({
  create: () => ({ sig: null, dismissed: false }),
  update(value, tr) {
    const engine = store.engine;
    const dismissed = tr.effects.some((e) => e.is(dismissSignature)) ? true : tr.docChanged ? false : value.dismissed;
    if (!engine || dismissed || (!tr.docChanged && !tr.selection && !tr.effects.some((e) => e.is(catalogueChanged)))) {
      return { sig: !engine || dismissed ? null : value.sig, dismissed };
    }
    const head = tr.state.selection.main.head;
    return { sig: engine.signatureAt(tr.state.doc.toString(), head), dismissed };
  },
  provide: (f) =>
    showTooltip.compute([f, "selection"], (state) => {
      const { sig } = state.field(f);
      if (!sig) return null;
      return { pos: state.selection.main.head, above: true, strictSide: false, arrow: false, create: createSignatureTooltip(f) };
    }),
});

const tooltipCreators = new WeakMap();
function createSignatureTooltip(field) {
  let create = tooltipCreators.get(field);
  if (!create) {
    create = (view) => {
      const dom = el("div", "cm-tooltip-signature");
      const paint = (state) => {
        const { sig } = state.field(field);
        if (sig) renderSignature(dom, sig);
        // The completion menu owns the space under the caret; the hint returns when it closes.
        dom.style.visibility = completionStatus(state) === "active" ? "hidden" : "";
      };
      paint(view.state);
      return { dom, update: (u) => paint(u.state) };
    };
    tooltipCreators.set(field, create);
  }
  return create;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics

function toCmDiagnostics(state, items) {
  const doc = state.doc;
  const clamp = (n) => Math.max(0, Math.min(doc.length, n));
  const out = [];
  for (const d of Array.isArray(items) ? items : []) {
    if (!d || typeof d !== "object") continue;
    let from;
    let to;
    if (Number.isFinite(d.from)) {
      from = clamp(d.from);
      to = clamp(Number.isFinite(d.to) ? d.to : from + 1);
    } else if (Number.isFinite(d.line) && d.line >= 1 && d.line <= doc.lines) {
      const line = doc.line(d.line);
      const col = Number.isFinite(d.column) ? Math.max(1, d.column) - 1 : null;
      if (col === null) {
        const lead = line.text.length - line.text.trimStart().length;
        from = line.from + lead;
        to = Math.max(from, line.to);
      } else {
        from = Math.min(line.from + col, line.to);
        if (Number.isFinite(d.endLine) && d.endLine >= 1 && d.endLine <= doc.lines) {
          const endLine = doc.line(d.endLine);
          to = Math.min(endLine.from + (Number.isFinite(d.endColumn) ? Math.max(1, d.endColumn) - 1 : endLine.length), endLine.to);
        } else {
          const word = /^[A-Za-z0-9_]+/.exec(line.text.slice(from - line.from));
          to = Math.min(line.to, from + (word ? word[0].length : 1));
        }
      }
    } else {
      continue;
    }
    if (to < from) [from, to] = [to, from];
    if (to === from) to = Math.min(doc.length, from + 1);
    const severity = d.severity === "warning" ? "warning" : d.severity === "error" ? "error" : "info";
    out.push({ from, to, severity, message: String(d.message ?? ""), source: d.source ? String(d.source) : undefined });
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Mount

function currentRootDark() {
  return document.documentElement.getAttribute("data-theme") === "dark";
}

function resolveDark(theme) {
  if (theme === "dark") return true;
  if (theme === "light") return false;
  if (theme === "system") return window.matchMedia?.("(prefers-color-scheme: dark)").matches ?? false;
  return currentRootDark();
}

export function mount(element, onChange, options = {}) {
  if (!(element instanceof Element)) throw new TypeError("bbScriptEditor.mount: element must be a DOM element");
  instances.get(element)?.destroy();

  const themeCompartment = new Compartment();
  let theme = options.theme ?? "inherit";
  let destroyed = false;
  element.classList.add("bb-script-editor");
  element.style.minWidth = "0";
  element.style.minHeight = "0";
  element.style.overflow = "hidden";

  const applyHostTheme = () => {
    if (theme === "light" || theme === "dark") element.setAttribute("data-theme", theme);
    else element.removeAttribute("data-theme");
  };
  applyHostTheme();

  const keys = Prec.highest(
    keymap.of([
      { key: "Tab", run: (v) => acceptCompletion(v) },
      { key: "Escape", run: (v) => (v.state.field(signatureField).sig ? (v.dispatch({ effects: dismissSignature.of(null) }), true) : false) },
    ]),
  );

  const extensions = [
    lineNumbers(),
    highlightActiveLineGutter(),
    highlightSpecialChars(),
    history(),
    drawSelection(),
    indentUnit.of("    "),
    EditorState.tabSize.of(4),
    indentOnInput(),
    bracketMatching(),
    closeBrackets(),
    highlightActiveLine(),
    highlightSelectionMatches(),
    python(),
    syntaxHighlighting(highlight),
    search({ top: true }),
    lintGutter(),
    autocompletion({ override: [completionSource], activateOnTyping: true, maxRenderedOptions: 80 }),
    tooltips({ tooltipSpace: (view) => view.dom.getBoundingClientRect() }),
    signatureField,
    keys,
    keymap.of([...closeBracketsKeymap, ...defaultKeymap, ...searchKeymap, ...historyKeymap, ...completionKeymap, ...lintKeymap, indentWithTab]),
    EditorView.contentAttributes.of({ spellcheck: "false", autocorrect: "off", autocapitalize: "off", "aria-label": "Script source" }),
    editorTheme,
    themeCompartment.of(EditorView.darkTheme.of(resolveDark(theme))),
    EditorView.updateListener.of((u) => {
      if (!u.docChanged || destroyed || u.transactions.some((tr) => tr.annotation(External))) return;
      try {
        onChange?.(u.state.doc.toString());
      } catch (e) {
        console.error("bbScriptEditor onChange failed", e);
      }
    }),
  ];

  const makeState = (doc, selection) => EditorState.create({ doc, selection, extensions });
  const view = new EditorView({ state: makeState(options.source ?? ""), parent: element });

  const syncDark = () => view.dispatch({ effects: themeCompartment.reconfigure(EditorView.darkTheme.of(resolveDark(theme))) });
  const observer = new MutationObserver(() => {
    if (theme === "inherit" && !destroyed) syncDark();
  });
  observer.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
  const unsubscribe = store.onChange(() => {
    if (!destroyed) view.dispatch({ effects: catalogueChanged.of(null) });
  });

  const handle = {
    setSource(source, opts = {}) {
      const next = String(source ?? "");
      if (destroyed || next === view.state.doc.toString()) return;
      if (opts.keepHistory) {
        const head = Math.min(view.state.selection.main.head, next.length);
        view.dispatch({
          changes: { from: 0, to: view.state.doc.length, insert: next },
          selection: { anchor: head },
          annotations: [External.of(true)],
        });
      } else {
        const head = opts.keepSelection ? Math.min(view.state.selection.main.head, next.length) : 0;
        view.setState(makeState(next, { anchor: head }));
      }
    },
    getSource: () => view.state.doc.toString(),
    focus() {
      view.focus();
    },
    insert(text) {
      if (destroyed) return;
      const tr = view.state.replaceSelection(String(text));
      view.dispatch(tr, { scrollIntoView: true, userEvent: "input.paste" });
      view.focus();
    },
    /** Insert a docs entry (flatten() entry or its id/path); adds missing imports. Returns the plan. */
    insertDoc(ref) {
      if (destroyed) return null;
      const entries = flatten(store.catalogue);
      const entry = typeof ref === "string" ? entries.find((e) => e.id === ref || e.path === ref) : ref;
      if (!entry) return null;
      const sel = view.state.selection.main;
      const plan = planInsert(view.state.doc.toString(), entry, { from: sel.from, to: sel.to }, store.index);
      view.dispatch({ changes: plan.changes, scrollIntoView: true, userEvent: "input.paste" });
      view.focus();
      return { missingImports: plan.missingImports, body: plan.body };
    },
    destroy() {
      if (destroyed) return;
      destroyed = true;
      observer.disconnect();
      unsubscribe();
      view.destroy();
      element.classList.remove("bb-script-editor");
      element.removeAttribute("data-theme");
      if (instances.get(element) === handle) instances.delete(element);
    },
    setTheme(next) {
      if (destroyed) return;
      theme = ["light", "dark", "system"].includes(next) ? next : "inherit";
      applyHostTheme();
      syncDark();
    },
    setDiagnostics(items) {
      if (destroyed) return;
      view.dispatch(setDiagnostics(view.state, toCmDiagnostics(view.state, items)));
    },
    undo() {
      return undo(view);
    },
    redo() {
      return redo(view);
    },
    search() {
      if (destroyed) return false;
      view.focus();
      return openSearchPanel(view);
    },
    getSelection() {
      const s = view.state.selection.main;
      const line = view.state.doc.lineAt(s.head);
      return {
        from: s.from,
        to: s.to,
        anchor: s.anchor,
        head: s.head,
        text: view.state.sliceDoc(s.from, s.to),
        line: line.number,
        column: s.head - line.from + 1,
      };
    },
    _view: view,
  };
  instances.set(element, handle);
  return handle;
}
