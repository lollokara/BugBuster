import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { decodeCatalogue, FirmwareAPIIndex } from "../src/catalogue.js";
import { flatten, importPosition, planInsert, search, splitExample } from "../src/docs.js";

const read = (rel) => readFileSync(fileURLToPath(new URL(rel, import.meta.url)), "utf8");
const raw = JSON.parse(read("../../../../python/firmware_modules/stubs/firmware_api.json"));
const catalogue = decodeCatalogue(raw);
const index = new FirmwareAPIIndex(catalogue);
const entries = flatten(catalogue);
const byPath = new Map(entries.map((e) => [e.path, e]));

test("public copy and canonical catalogue are byte-identical", () => {
  assert.equal(read("../../public/firmware-api.json"), read("../../../../python/firmware_modules/stubs/firmware_api.json"));
});

test("every function/method keeps signature, params, defaults, returns, keys, raises, notes and examples", () => {
  let checked = 0;
  const verify = (path, f) => {
    const e = byPath.get(path);
    assert.ok(e, path);
    assert.equal(e.signature, f.signature, path);
    assert.equal(e.returns ?? null, f.returns ?? null, path);
    assert.equal(e.returnsDoc, f.returns_doc ?? "", path);
    assert.deepEqual(e.returnKeys, f.return_keys ?? [], path);
    assert.deepEqual(e.raises, f.raises ?? [], path);
    assert.equal(e.notes, f.notes ?? "", path);
    assert.deepEqual(e.examples, f.examples ?? [], path);
    assert.deepEqual(
      e.params.map((p) => [p.name, p.kind, p.annotation, p.default, p.doc]),
      (f.params ?? []).filter((p) => p.name !== "self").map((p) => [p.name, p.kind, p.annotation ?? null, p.default ?? null, p.doc ?? ""]),
      path,
    );
    checked++;
  };
  for (const m of raw.modules) {
    for (const f of m.functions) verify(`${m.name}.${f.name}`, f);
    for (const c of m.classes) for (const f of c.methods) if (f.name !== "__init__") verify(`${m.name}.${c.name}.${f.name}`, f);
  }
  const expected = raw.modules.reduce((n, m) => n + m.functions.length + m.classes.reduce((k, c) => k + c.methods.filter((f) => f.name !== "__init__").length, 0), 0);
  assert.equal(checked, expected);
  assert.ok(checked > 0);
  const withKeys = entries.filter((e) => e.returnKeys.length).length;
  const withRaises = entries.filter((e) => e.raises.length).length;
  const withNotes = entries.filter((e) => e.notes).length;
  const withExamples = entries.filter((e) => e.kind !== "example" && e.examples.length).length;
  assert.ok(withKeys > 0 && withRaises > 0 && withNotes > 0 && withExamples > 0, JSON.stringify({ withKeys, withRaises, withNotes, withExamples }));
});

test("constants, classes, namespaces and examples are all present", () => {
  const kinds = new Set(entries.map((e) => e.kind));
  for (const k of ["module", "class", "namespace", "function", "method", "constant", "example"]) assert.ok(kinds.has(k), k);
  assert.equal(entries.filter((e) => e.kind === "example").length, raw.examples.length);
  assert.equal(entries.filter((e) => e.kind === "module").length, raw.modules.length);
  const ch = byPath.get("bugbuster.Channel");
  assert.equal(ch.signature, "Channel(channel: int)");
  assert.equal(byPath.get("daq.run").kind, "namespace");
  assert.equal(byPath.get("bugbuster.FUNC_VOUT").kind, "constant");
});

test("flatten output is plain JSON data", () => {
  assert.deepEqual(JSON.parse(JSON.stringify(entries)), entries);
});

test("search ranks name hits, requires every token, and filters by kind", () => {
  assert.equal(search(entries, "sleep")[0].path, "bugbuster.sleep");
  assert.ok(search(entries, "set voltage").some((e) => e.path === "bugbuster.Channel.set_voltage"));
  assert.deepEqual(search(entries, "zzzz-nothing"), []);
  assert.ok(search(entries, "", { kinds: ["module"] }).every((e) => e.kind === "module"));
  assert.ok(search(entries, "background", { kinds: ["example"] }).length >= 1);
  assert.ok(search(entries, "vdut", { limit: 1 }).length === 1);
});

test("API insert adds a missing import once and keeps existing ones", () => {
  const vdut = byPath.get("daq.vdut");
  const plan = planInsert("x = 1\n", vdut, { from: 6, to: 6 }, index);
  assert.deepEqual(plan.missingImports, ["import daq"]);
  assert.deepEqual(plan.changes, [
    { from: 0, to: 0, insert: "import daq\n\n" },
    { from: 6, to: 6, insert: "daq.vdut()" },
  ]);
  const again = planInsert("import daq\nx = 1\n", vdut, { from: 17, to: 17 }, index);
  assert.deepEqual(again.missingImports, []);
  assert.equal(again.changes.length, 1);
  const aliased = planInsert("import daq as d\n", byPath.get("daq.present"), undefined, index);
  assert.deepEqual(aliased.missingImports, ["import daq"], "an alias does not satisfy `import daq`");
});

test("required arguments appear as placeholders text in API inserts", () => {
  assert.equal(byPath.get("bugbuster.sleep").insertText, "bugbuster.sleep(ms)");
  assert.equal(byPath.get("daq.run.new").insertText, "daq.run.new(name, chem, cells, capacity_mah)");
  assert.equal(byPath.get("bugbuster.Channel").insertText, "bugbuster.Channel(channel)");
});

test("import lands after the docstring and existing imports", () => {
  const src = '"""Doc."""\nimport bugbuster\nfrom machine import Pin\n\nx = 1\n';
  const { offset, hasImports } = importPosition(src);
  assert.equal(src.slice(0, offset), '"""Doc."""\nimport bugbuster\nfrom machine import Pin\n');
  assert.equal(hasImports, true);
  const doc = importPosition('"""Doc."""\nx = 1\n');
  assert.equal(doc.offset, '"""Doc."""\n'.length);
  assert.equal(doc.hasImports, false);
  assert.equal(importPosition("").offset, 0);
});

test("examples: insert into an empty script verbatim, otherwise body plus missing imports", () => {
  const ex = byPath.get(`example:${raw.examples[0].name}`);
  const empty = planInsert("", ex, { from: 0, to: 0 }, index);
  assert.equal(empty.changes[0].insert.trimEnd(), raw.examples[0].source.trimEnd());
  const parts = splitExample(raw.examples[0].source);
  assert.ok(parts.imports.length > 0 && parts.docstring.startsWith('"""'));
  const into = planInsert("print('hi')\n", ex, { from: 12, to: 12 }, index);
  assert.deepEqual(into.missingImports, parts.imports);
  assert.equal(into.changes.at(-1).insert.trim(), parts.body.trim());
  const withAll = `${parts.imports.join("\n")}\nprint('hi')\n`;
  assert.deepEqual(planInsert(withAll, ex, { from: withAll.length, to: withAll.length }, index).missingImports, []);
});

test("every catalogue example splits into docstring, imports and a non-empty body", () => {
  for (const e of raw.examples) {
    const parts = splitExample(e.source);
    assert.ok(parts.body.length > 0, e.name);
  }
});

test("editor theme and highlight style use CSS tokens only, never raw colours", () => {
  const src = read("../src/editor.js");
  const themeBlock = src.slice(src.indexOf("const editorTheme"), src.indexOf("// Completion UI over the pure engine"));
  assert.doesNotMatch(themeBlock, /#[0-9a-fA-F]{3,8}\b/);
  assert.doesNotMatch(themeBlock, /\brgba?\(|\bhsla?\(/);
  assert.doesNotMatch(themeBlock, /:\s*"(white|black|red|green|blue|gray|grey|yellow|orange)"/);
  assert.match(themeBlock, /var\(--surface-1\)/);
});
