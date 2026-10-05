// Port of iOSApp/Tests/ScriptCompletionTests.swift plus multi-line string/comment, signature help
// and range/Unicode cases. Runs on the canonical firmware API catalogue.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { BUILTINS } from "../src/builtins.js";
import { decodeCatalogue, FirmwareAPIIndex } from "../src/catalogue.js";
import { callItem, score, ScriptBindings, ScriptCompletionEngine } from "../src/completion.js";

const catalogue = decodeCatalogue(
  JSON.parse(readFileSync(fileURLToPath(new URL("../../../../python/firmware_modules/stubs/firmware_api.json", import.meta.url)), "utf8")),
);
const engine = new ScriptCompletionEngine(new FirmwareAPIIndex(catalogue));

/** `|` marks the caret. */
function complete(marked, opts) {
  const caret = marked.indexOf("|");
  const text = marked.slice(0, caret) + marked.slice(caret + 1);
  return engine.complete(text, caret, opts);
}
const labels = (marked) => complete(marked).items.map((i) => i.label);
function item(marked, label) {
  const found = complete(marked).items.find((i) => i.label === label);
  assert.ok(found, `no ${label} item in ${JSON.stringify(marked)}`);
  return found;
}
const isEmpty = (marked) => complete(marked).items.length === 0;

// members after "."

test("module members after dot", () => {
  const r = complete("import daq\ndaq.|");
  const have = new Set(r.items.map((i) => i.label));
  for (const n of ["run", "present", "vdut", "read", "samples"]) assert.ok(have.has(n), n);
  assert.equal(r.items[0].label, "run");
  assert.equal(r.items[0].kind, "namespace");
});

test("namespace methods are prefix ranked alphabetically", () => {
  assert.deepEqual(labels("import daq\ndaq.run.st|"), ["start", "status", "stop"]);
});

test("insert text and caret offsets", () => {
  const items = complete("import daq\ndaq.|").items;
  const vdut = items.find((i) => i.label === "vdut");
  assert.equal(vdut.insertText, "vdut()");
  assert.equal(vdut.caretOffset, 5);
  assert.equal(items.find((i) => i.label === "present").caretOffset, 9);
  assert.equal(items.find((i) => i.label === "run").insertText, "run");
});

test("replace range covers the partial", () => {
  assert.deepEqual(complete("import daq\ndaq.vd|").replaceRange, { location: 15, length: 2 });
});

test("unicode before the caret keeps UTF-16 offsets", () => {
  const text = "# café ☕️ µA\nimport daq\ndaq.v";
  const r = engine.complete(text, text.length);
  assert.deepEqual(r.replaceRange, { location: text.length - 1, length: 1 });
  assert.equal(r.items[0].label, "vdut");
});

test("instance methods from constructor assignment", () => {
  const found = labels("import bugbuster\nch = bugbuster.Channel(0)\nch.|");
  for (const n of ["set_voltage", "read_voltage", "set_function", "set_do"]) assert.ok(found.includes(n), n);
  assert.ok(!found.includes("__init__"));
});

test("aliased import", () => {
  const first = complete("import bugbuster as bb\nbb.Ch|").items[0];
  assert.equal(first.label, "Channel");
  assert.equal(first.kind, "type");
  assert.equal(first.detail, "Channel(channel: int)");
});

test("from-imported class", () => {
  assert.deepEqual(labels("from bugbuster import Channel\nc = Channel(1)\nc.read|"), ["read_voltage"]);
});

test("return annotation gives instance type", () => {
  assert.deepEqual(labels("import bugbuster\nc = bugbuster.claim([0])\nc.|"), []);
  assert.deepEqual(labels("import bugbuster\nc = bugbuster.claim([0])\nc.__|"), ["__enter__", "__exit__"]);
});

test("unknown receiver gives nothing", () => assert.ok(isEmpty("foo.|")));

// kwargs inside calls

test("kwargs inside call", () => assert.deepEqual(labels("import daq\ndaq.vdut(|"), ["enable=", "volts=", "amps_limit="]));
test("kwargs skip used and positionally passed", () => {
  assert.deepEqual(labels("import daq\ndaq.vdut(True, volts=1.0, |"), ["amps_limit="]);
});
test("kwarg word-start match", () => assert.deepEqual(labels("import daq\ndaq.vdut(lim|"), ["amps_limit="]));
test("required params rank after optional", () => {
  assert.deepEqual(labels("import bugbuster\nbugbuster.I2C(|"), [
    "freq=",
    "pullups=",
    "supply=",
    "vlogic=",
    "allow_split_supplies=",
    "sda_io=",
    "scl_io=",
  ]);
});
test("multi-line call still finds callee", () => {
  assert.deepEqual(labels('import daq\ndaq.run.new(\n    "bench",\n    "lipo",\n    |'), [
    "soc=",
    "self_discharge=",
    "ext_load=",
    "cells=",
    "capacity_mah=",
  ]);
});
test("identifier fallback inside call", () => assert.equal(labels("import daq\ndaq.vdut(da|")[0], "daq"));

// ranking

test("prefix before word-start then groups", () => {
  assert.deepEqual(labels("import bugbuster\nbugbuster.rail|").slice(0, 5), [
    "rail_power_up",
    "hat_rails",
    "hat_set_rail_enable",
    "hat_set_rail_voltage",
    "HAT_RAIL_3V3_ADJ",
  ]);
});
test("case-insensitive prefix", () => assert.deepEqual(labels("import bugbuster\nbugbuster.func_v|"), ["FUNC_VIN", "FUNC_VOUT"]));
test("scores", () => {
  assert.equal(score("set_voltage", ""), 0);
  assert.equal(score("set_voltage", "set"), 0);
  assert.equal(score("FUNC_VIN", "func"), 1);
  assert.equal(score("set_voltage", "volt"), 2);
  assert.equal(score("set_voltage", "oltage"), null);
});
test("exact namespace match is suppressed", () => assert.ok(isEmpty("import daq\ndaq.run|")));

// imports, strings, comments, numbers

test("import module names", () => {
  assert.deepEqual(labels("import d|"), ["daq", "bb_devices"]);
  assert.deepEqual(labels("import bugbuster, b|"), ["bb_devices", "bb_helpers", "bb_logging", "bugbuster"]);
});
test("from-import lists module members", () => {
  const have = new Set(labels("from daq import |"));
  assert.ok(have.has("vdut") && have.has("run"));
});
test("nothing inside strings or comments", () => {
  assert.ok(isEmpty('import daq\nprint("daq.|'));
  assert.ok(isEmpty("import daq\n# daq.|"));
});
test("nothing inside multi-line strings or comment blocks", () => {
  assert.ok(isEmpty('import daq\nx = """\nnotes\ndaq.|'));
  assert.ok(isEmpty("import daq\nx = '''\nprint(\nda|"));
  assert.ok(isEmpty('import daq\nx = """a\\"""\ndaq.|\n"""'));
  assert.ok(!isEmpty('import daq\nx = """a"""\ndaq.|'), "completes again after the closing triple quote");
  assert.ok(!isEmpty("import daq\n# daq.\ndaq.|"), "a comment ends at the newline");
  assert.ok(isEmpty("import daq\nx = 'it\\'s daq.|"), "escaped quote keeps the string open");
});
test("numbers do not complete", () => {
  assert.ok(isEmpty("x = 1.|"));
  assert.ok(isEmpty("x = 12|"));
});

// bindings

test("bindings scan", () => {
  const text = "import bugbuster as bb, daq\nfrom machine import Pin as P\nch = bb.Channel(0)\nwith bb.claim([1]) as cl:\n    pass\n";
  const b = ScriptBindings.scan(text, engine.index);
  assert.equal(b.aliases.get("bb"), "bugbuster");
  assert.equal(b.aliases.get("daq"), "daq");
  assert.equal(b.aliases.get("P"), "machine.Pin");
  assert.equal(b.instances.get("ch"), "bugbuster.Channel");
  assert.equal(b.instances.get("cl"), "bugbuster.Claim");
});
test("star import aliases every member", () => {
  const b = ScriptBindings.scan("from daq import *\n", engine.index);
  assert.equal(b.aliases.get("vdut"), "daq.vdut");
  assert.equal(b.aliases.get("run"), "daq.run");
});
test("trailing comments and CRLF do not hide bindings", () => {
  const b = ScriptBindings.scan("import daq  # power\r\nfrom bugbuster import Channel # ch\r\nc = Channel(0)\r\n", engine.index);
  assert.equal(b.aliases.get("daq"), "daq");
  assert.equal(b.instances.get("c"), "bugbuster.Channel");
});

// call insertion with placeholders

test("required argument becomes a placeholder", () => {
  const sleep = item("import bugbuster\nbugbuster.sle|", "sleep");
  assert.equal(sleep.insertText, "sleep(ms)");
  assert.deepEqual(sleep.placeholders, [{ location: 6, length: 2 }]);
  assert.equal(sleep.caretOffset, 6);
  assert.equal(sleep.isCall, true);
});
test("constructor placeholders", () => {
  const channel = item("import bugbuster\nbugbuster.Chan|", "Channel");
  assert.equal(channel.insertText, "Channel(channel)");
  assert.deepEqual(channel.placeholders, [{ location: 8, length: 7 }]);
});
test("several required arguments get one placeholder each", () => {
  const n = item("import daq\ndaq.run.ne|", "new");
  assert.equal(n.insertText, "new(name, chem, cells, capacity_mah)");
  assert.deepEqual(n.placeholders.map((p) => p.location), [4, 10, 16, 23]);
  assert.deepEqual(n.placeholders.map((p) => p.length), [4, 4, 5, 12]);
});
test("optional and keyword arguments are not inserted", () => {
  const vdut = item("import daq\ndaq.vd|", "vdut");
  assert.equal(vdut.insertText, "vdut()");
  assert.equal(vdut.caretOffset, 5);
  assert.equal(vdut.placeholders.length, 0);
});
test("no-argument function puts the caret after the parens", () => {
  const t = item("import bugbuster\nbugbuster.tick|", "ticks_ms");
  assert.equal(t.insertText, "ticks_ms()");
  assert.equal(t.caretOffset, 10);
  assert.equal(t.placeholders.length, 0);
});
test("required keyword-only argument selects its value", () => {
  const p = (name, kind, def) => ({ name, kind, annotation: null, defaultValue: def, doc: "", acceptsKeyword: true, isRequired: def === null });
  const fn = { name: "f", signature: "f(x, *, y, z=1)", params: [p("x", "positional", null), p("y", "keyword_only", null), p("z", "keyword_only", "1")] };
  const call = callItem("f", fn, "", "", "function");
  assert.equal(call.insertText, "f(x, y=y)");
  assert.deepEqual(call.placeholders, [
    { location: 2, length: 1 },
    { location: 7, length: 1 },
  ]);
});
test("typed parenthesis is not doubled", () => {
  const sleep = item("import bugbuster\nbugbuster.sle|(10)", "sleep");
  assert.equal(sleep.insertText, "sleep");
  assert.equal(sleep.caretOffset, 5);
  assert.equal(sleep.placeholders.length, 0);
  assert.equal(item("pri|(1)", "print").insertText, "print");
});

// built-ins and keywords

test("builtin functions insert a call", () => {
  const print = item("pri|", "print");
  assert.equal(print.insertText, "print()");
  assert.equal(print.caretOffset, 6);
  assert.equal(print.kind, "builtin");
  const len = item("x = le|", "len");
  assert.equal(len.insertText, "len(obj)");
  assert.deepEqual(len.placeholders, [{ location: 4, length: 3 }]);
  assert.equal(item("for i in ran|", "range").insertText, "range(stop)");
});
test("builtin types, exceptions and keywords complete", () => {
  assert.equal(item("n = flo|", "float").insertText, "float(x)");
  assert.equal(item("s = byt|", "bytes").insertText, "bytes()");
  assert.equal(item("raise Val|", "ValueError").kind, "exception");
  assert.equal(item("whi|", "while").kind, "reserved");
  assert.equal(item("imp|", "import").insertText, "import");
  assert.equal(item("de|", "def").kind, "reserved");
  assert.equal(item("tr|", "try").kind, "reserved");
});
test("builtin list covers the common names", () => {
  const names = new Set(BUILTINS.map((b) => b.name));
  for (const n of ["print", "len", "range", "int", "float", "str", "list", "dict", "enumerate", "zip", "min", "max", "abs", "round", "sorted", "isinstance", "hex", "bytes", "bytearray", "Exception", "ValueError", "OSError", "KeyboardInterrupt", "for", "while", "import", "def", "try", "except", "with", "return"]) {
    assert.ok(names.has(n), n);
  }
  assert.equal(names.size, BUILTINS.length, "duplicate built-in names");
  assert.ok(BUILTINS.every((b) => b.doc !== ""));
});
test("class positions take the bare name", () => {
  assert.equal(item("isinstance(x, in|", "int").insertText, "int");
  assert.equal(item("try:\n    pass\nexcept Val|", "ValueError").insertText, "ValueError");
  assert.equal(item("try:\n    pass\nexcept (OSE|", "OSError").insertText, "OSError");
});
test("catalogue matches rank above builtins", () => {
  const r = complete("from bugbuster import log\nlo|");
  assert.equal(r.items[0].label, "log");
  assert.ok(r.items.some((i) => i.label === "locals"));
  assert.equal(r.items[0].kind, "function");
});
test("builtins stay quiet in strings and comments", () => {
  assert.ok(isEmpty("x = 'pri|"));
  assert.ok(isEmpty("# pri|"));
});
test("builtin parameter spec parsing", async () => {
  const { parseParams } = await import("../src/builtins.js");
  const params = parseParams("iterable, *, key=None, reverse=False");
  assert.deepEqual(params.map((p) => p.name), ["iterable", "key", "reverse"]);
  assert.deepEqual(params.map((p) => p.kind), ["positional", "keyword_only", "keyword_only"]);
  assert.deepEqual(params.map((p) => p.isRequired), [true, false, false]);
  assert.deepEqual(parseParams("*objects, sep=' '").map((p) => p.kind), ["var_positional", "keyword_only"]);
});

// beyond the iOS suite

test("explicit completion relaxes the two-character minimum", () => {
  assert.ok(isEmpty("p|"), "implicit needs 2 characters");
  assert.ok(complete("p|", { explicit: true }).items.some((i) => i.label === "print"));
});

test("signature help marks the active parameter and survives nesting", () => {
  const sig = (marked) => {
    const caret = marked.indexOf("|");
    return engine.signatureAt(marked.slice(0, caret) + marked.slice(caret + 1), caret);
  };
  let s = sig("import bugbuster\nbugbuster.Channel(|");
  assert.equal(s.signature, "Channel(channel: int)");
  assert.equal(s.activeParam, "channel");
  s = sig("import bugbuster\nch = bugbuster.Channel(0)\nch.set_voltage(1.5, bi|");
  assert.equal(s.fn.name, "set_voltage");
  assert.equal(s.argIndex, 1);
  assert.equal(s.activeParam, "bipolar");
  s = sig("import bugbuster\nch = bugbuster.Channel(0)\nch.set_voltage(max(1, 2), bipolar=T|");
  assert.equal(s.activeParam, "bipolar");
  s = sig("import bugbuster\nbugbuster.sleep(max(1, |");
  assert.equal(s.fn.name, "max");
  assert.equal(sig('import bugbuster\nbugbuster.sleep("(|'), null);
  assert.equal(sig("import bugbuster\nbugbuster.sleep(5)|"), null);
});

test("catalogue failures are labelled, not thrown", async () => {
  const { tryDecode } = await import("../src/catalogue.js");
  assert.equal(tryDecode("{not json").error.code, "invalid");
  assert.equal(tryDecode({ schema: "bugbuster.firmware-api/2", modules: [] }).error.code, "unsupported-schema");
  assert.equal(tryDecode({ schema: "bugbuster.firmware-api/1" }).error.code, "invalid");
  assert.equal(tryDecode({ schema: "bugbuster.firmware-api/1", modules: [{ name: "m", functions: [{ name: "f" }] }] }).ok, false);
  assert.equal(tryDecode(null).ok, false);
  assert.equal(tryDecode({ schema: "bugbuster.firmware-api/1", modules: [], examples: [] }).ok, true);
});
