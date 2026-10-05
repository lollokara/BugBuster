// Interaction tests for the CodeMirror script editor, on a real Chromium page served from a local
// static server (no network). Run: node tests/ui-harness/editor_e2e.mjs  [--shots]
// Playwright is resolved from BB_PLAYWRIGHT_DIR, this harness, tests/e2e, or a sibling worktree.
import assert from "node:assert/strict";
import { existsSync, mkdirSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import { dirname, extname, join, normalize, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const appRoot = resolve(here, "../..");
const shotDir = process.env.BB_SHOT_DIR ?? resolve(appRoot, "../../../../scratch/desktop-scripting");
const wantShots = process.argv.includes("--shots");

async function loadPlaywright() {
  const roots = [
    process.env.BB_PLAYWRIGHT_DIR,
    join(here, "node_modules/playwright"),
    join(appRoot, "tests/e2e/node_modules/playwright"),
    resolve(appRoot, "../../../../DesktopApp/BugBuster/tests/e2e/node_modules/playwright"),
    resolve(appRoot, "../../../battsim-ui/DesktopApp/BugBuster/tests/ui-harness/node_modules/playwright"),
    resolve(appRoot, "../../../../.worktrees/battsim-ui/DesktopApp/BugBuster/tests/ui-harness/node_modules/playwright"),
  ].filter(Boolean);
  for (const r of roots) {
    if (existsSync(join(r, "package.json"))) return import(pathToFileURL(join(r, "index.mjs")).href);
  }
  throw new Error(`playwright not found; set BB_PLAYWRIGHT_DIR (tried ${roots.join(", ")})`);
}

const TYPES = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".json": "application/json", ".woff2": "font/woff2" };
function serve() {
  const server = createServer((req, res) => {
    let path = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (path.startsWith("/styles/public/")) path = path.replace("/styles/public/", "/public/"); // tokens.css font urls
    const file = normalize(join(appRoot, path));
    if (!file.startsWith(appRoot) || !existsSync(file) || file.endsWith("\\") ) {
      res.writeHead(404).end("not found");
      return;
    }
    res.writeHead(200, { "content-type": TYPES[extname(file)] ?? "application/octet-stream", "cache-control": "no-store" }).end(readFileSync(file));
  });
  return new Promise((ok) => server.listen(0, "127.0.0.1", () => ok(server)));
}

const results = [];
async function step(name, fn) {
  try {
    await fn();
    results.push({ name, ok: true });
    console.log(`PASS ${name}`);
  } catch (e) {
    results.push({ name, ok: false, error: e });
    console.log(`FAIL ${name}\n  ${String(e.stack ?? e).split("\n").slice(0, 6).join("\n  ")}`);
  }
}

const { chromium } = await loadPlaywright();
const server = await serve();
const base = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1000, height: 640 }, deviceScaleFactor: wantShots ? 2 : 1 });
const problems = [];
const external = [];
page.on("pageerror", (e) => problems.push(`pageerror: ${e.message}`));
page.on("console", (m) => m.type() === "error" && problems.push(`console.error: ${m.text()}`));
page.on("requestfailed", (r) => problems.push(`requestfailed: ${r.url()}`));
page.on("request", (r) => {
  if (!r.url().startsWith(base)) external.push(r.url());
});
page.on("response", (r) => r.status() >= 400 && r.url().includes("/firmware-api.json") === false && problems.push(`HTTP ${r.status()} ${r.url()}`));

await page.goto(`${base}/tests/ui-harness/editor_test.html`);
const canonical = JSON.parse(readFileSync(resolve(appRoot, "../../python/firmware_modules/stubs/firmware_api.json"), "utf8"));

const fresh = (source = "") =>
  page.evaluate((src) => {
    window.changes = [];
    window.h?.destroy();
    window.h = window.bbScriptEditor.mount(document.getElementById("host"), (s) => window.changes.push(s), { source: src });
    window.h.focus();
    const v = window.h._view;
    v.dispatch({ selection: { anchor: v.state.doc.length } });
  }, source);
const src = () => page.evaluate(() => window.h.getSource());
const sel = () => page.evaluate(() => window.h.getSelection());
const typeText = (t) => page.keyboard.type(t, { delay: 12 });
// CodeMirror ignores accept keys for 75 ms after the popup changes (interactionDelay).
const openPopup = async () => {
  await page.locator(".cm-tooltip-autocomplete").waitFor();
  await page.waitForTimeout(180);
};
const popup = () => page.locator(".cm-tooltip-autocomplete");
async function expectNoPopup() {
  await page.waitForTimeout(350);
  assert.equal(await popup().count(), 0, "completion popup must not appear");
}

await step("catalogue loads over fetch and matches canonical JSON", async () => {
  const status = await page.evaluate(() => window.bbScriptCatalogue.load());
  assert.equal(status.state, "ready");
  assert.equal(status.moduleCount, canonical.modules.length);
  assert.equal(status.exampleCount, canonical.examples.length);
  const data = await page.evaluate(() => window.bbScriptCatalogue.data);
  assert.deepEqual(data, canonical);
});

await step("completion popup: keyboard (Tab) accepts and puts caret inside parens", async () => {
  await fresh("import daq\ndaq.");
  await typeText("vd");
  await openPopup();
  assert.ok((await page.locator(".cm-tooltip-autocomplete li").first().innerText()).includes("vdut"));
  await page.keyboard.press("Tab");
  assert.equal(await src(), "import daq\ndaq.vdut()");
  const s = await sel();
  assert.equal(s.head, "import daq\ndaq.vdut(".length);
  assert.equal(await popup().count(), 0);
});

await step("required placeholder is selected, typing replaces it, Tab leaves the call", async () => {
  await fresh("import bugbuster\nbugbuster.");
  await typeText("sle");
  await openPopup();
  await page.keyboard.press("Tab");
  assert.equal(await src(), "import bugbuster\nbugbuster.sleep(ms)");
  assert.equal((await sel()).text, "ms");
  await typeText("250");
  assert.equal(await src(), "import bugbuster\nbugbuster.sleep(250)");
  await page.keyboard.press("Tab");
  assert.equal((await sel()).head, (await src()).length);
});

await step("Tab walks several placeholders in order", async () => {
  await fresh("import daq\ndaq.run.");
  await typeText("ne");
  await openPopup();
  await page.keyboard.press("Tab");
  assert.equal(await src(), "import daq\ndaq.run.new(name, chem, cells, capacity_mah)");
  const seen = [(await sel()).text];
  for (let i = 0; i < 3; i++) {
    await page.keyboard.press("Tab");
    seen.push((await sel()).text);
  }
  assert.deepEqual(seen, ["name", "chem", "cells", "capacity_mah"]);
  await page.keyboard.press("Tab");
  const end = await sel();
  assert.equal(end.head, (await src()).length);
  assert.equal(end.text, "");
  assert.equal(await src(), "import daq\ndaq.run.new(name, chem, cells, capacity_mah)");
});

await step("mouse click accepts a completion; Enter also accepts", async () => {
  await fresh("import daq\ndaq.run.");
  await typeText("st");
  await openPopup();
  await page.locator(".cm-tooltip-autocomplete li", { hasText: "status" }).click();
  assert.equal(await src(), "import daq\ndaq.run.status()");
  await fresh("import daq\ndaq.run.");
  await typeText("sto");
  await openPopup();
  await page.keyboard.press("Enter");
  assert.ok((await src()).endsWith("daq.run.stop()"));
});

await step("arrow keys move the selection; Escape closes the popup", async () => {
  await fresh("import daq\ndaq.run.");
  await typeText("st");
  await openPopup();
  await page.keyboard.press("ArrowDown");
  const selected = await page.locator('.cm-tooltip-autocomplete li[aria-selected="true"]').innerText();
  assert.ok(selected.includes("status"), selected);
  await page.keyboard.press("Escape");
  assert.equal(await popup().count(), 0);
});

await step("typed parenthesis is not doubled; kwargs inside a call", async () => {
  await fresh("import bugbuster\nbugbuster.sle(10)");
  await page.evaluate(() => window.h._view.dispatch({ selection: { anchor: "import bugbuster\nbugbuster.sle".length } }));
  await page.keyboard.press("Control+Space");
  await openPopup();
  await page.keyboard.press("Tab");
  assert.equal(await src(), "import bugbuster\nbugbuster.sleep(10)");
  await fresh("import daq\ndaq.vdut(");
  await page.keyboard.press("Control+Space");
  await openPopup();
  const text = await popup().innerText();
  assert.ok(text.includes("enable=") && text.includes("volts=") && text.includes("amps_limit="), text);
});

await step("no completion in comments, strings or multi-line strings", async () => {
  await fresh("import daq\n# daq.");
  await typeText("vd");
  await expectNoPopup();
  await fresh('import daq\nx = "daq.');
  await typeText("vd");
  await expectNoPopup();
  await fresh('import daq\nx = """\nnotes\ndaq.');
  await typeText("vd");
  await expectNoPopup();
  await fresh("import daq\nx = '''\nprint(\nda");
  await typeText("q");
  await expectNoPopup();
});

await step("signature help shows the active parameter", async () => {
  await fresh("import bugbuster\nch = bugbuster.Channel(0)\nch.set_voltage(1.5, ");
  const tip = page.locator(".cm-tooltip-signature");
  await tip.waitFor();
  const text = await tip.innerText();
  assert.ok(text.includes("set_voltage(voltage: float, bipolar: bool = False)"), text);
  assert.equal(await tip.locator(".bb-sig-param.is-active").innerText(), " bipolar: bool = False");
  await page.keyboard.press("Escape");
  assert.equal(await tip.count(), 0);
});

await step("onChange fires for user edits only; setSource is silent and idempotent", async () => {
  await fresh("");
  await typeText("x = 1");
  const afterTyping = await page.evaluate(() => window.changes.at(-1));
  assert.equal(afterTyping, "x = 1");
  const n = await page.evaluate(() => window.changes.length);
  await page.evaluate(() => window.h.setSource("y = 2"));
  assert.equal(await src(), "y = 2");
  await page.evaluate(() => window.h.setSource("y = 2"));
  assert.equal(await page.evaluate(() => window.changes.length), n);
  await page.evaluate(() => window.h.insert("z"));
  assert.equal(await page.evaluate(() => window.changes.at(-1)), (await src()));
});

await step("undo/redo through keyboard and handle; setSource resets history", async () => {
  await fresh("");
  await typeText("abc");
  await page.keyboard.press("Control+z");
  assert.equal(await src(), "");
  await page.keyboard.press("Control+y");
  assert.equal(await src(), "abc");
  await page.evaluate(() => window.h.undo());
  assert.equal(await src(), "");
  await page.evaluate(() => window.h.redo());
  assert.equal(await src(), "abc");
  await page.evaluate(() => window.h.setSource("fresh"));
  await page.evaluate(() => window.h.undo());
  assert.equal(await src(), "fresh");
});

await step("auto-indent after colon and Tab indents selection", async () => {
  await fresh("");
  await typeText("def f():");
  await page.keyboard.press("Enter");
  await typeText("pass");
  assert.equal(await src(), "def f():\n    pass");
  await fresh("a\nb");
  await page.keyboard.press("Control+a");
  await page.keyboard.press("Tab");
  assert.equal(await src(), "    a\n    b");
});

await step("search and replace panel", async () => {
  await fresh("foo bar foo\nfoo");
  await page.evaluate(() => window.h.search());
  const panel = page.locator(".cm-search");
  await panel.waitFor();
  await panel.locator('input[name="search"]').fill("foo");
  await panel.locator('input[name="replace"]').fill("baz");
  await panel.locator('button[name="replaceAll"]').click();
  assert.equal(await src(), "baz bar baz\nbaz");
});

await step("diagnostics render and clear", async () => {
  await fresh("import daq\nx = (\ndaq.vdut()\n");
  await page.evaluate(() => window.h.setDiagnostics([{ line: 2, column: 5, severity: "error", message: "'(' was never closed", source: "device syntax" }, { line: 3, severity: "warning", message: "advisory", source: "local" }, { line: 99, message: "out of range is ignored" }, null]));
  await page.locator(".cm-lintRange-error").waitFor();
  assert.equal(await page.locator(".cm-lintRange-warning").count(), 1);
  await page.locator(".cm-lintRange-error").hover();
  await page.locator(".cm-tooltip-lint").waitFor();
  assert.ok((await page.locator(".cm-tooltip-lint").innerText()).includes("was never closed"));
  await page.evaluate(() => window.h.setDiagnostics([]));
  assert.equal(await page.locator(".cm-lintRange-error").count(), 0);
});

await step("long lines scroll inside the editor without widening the page", async () => {
  await fresh(`x = "${"a".repeat(6000)}"\ny = 1`);
  const m = await page.evaluate(() => {
    const host = document.getElementById("host");
    const sc = host.querySelector(".cm-scroller");
    return { hostW: host.clientWidth, hostScroll: host.scrollWidth, scW: sc.clientWidth, scScroll: sc.scrollWidth, doc: document.documentElement.scrollWidth, win: window.innerWidth };
  });
  assert.ok(m.scScroll > m.scW * 3, JSON.stringify(m));
  assert.ok(m.hostScroll <= m.hostW, JSON.stringify(m));
  assert.ok(m.doc <= m.win, JSON.stringify(m));
});

await step("selection, line numbers and syntax highlighting", async () => {
  await fresh("import daq\nx = 'hi'  # note\nif x:\n    pass");
  assert.equal(await page.locator(".cm-lineNumbers .cm-gutterElement").count() >= 4, true);
  const colours = await page.evaluate(() => {
    const spans = [...document.querySelectorAll(".cm-line span")];
    return new Set(spans.map((s) => getComputedStyle(s).color)).size;
  });
  assert.ok(colours >= 3, `expected several highlight colours, got ${colours}`);
  await page.evaluate(() => window.h._view.dispatch({ selection: { anchor: 0, head: 6 } }));
  assert.equal((await sel()).text, "import");
});

await step("docs: flatten/search/insert helpers add only missing imports", async () => {
  const info = await page.evaluate(() => {
    const api = window.bbScriptCatalogue;
    const flat = api.flatten();
    const hits = api.search("set voltage");
    return { n: flat.length, kinds: [...new Set(flat.map((e) => e.kind))].sort(), top: hits.slice(0, 3).map((e) => e.path), rt: JSON.stringify(flat).length > 1000 };
  });
  assert.ok(info.n > 100 && info.rt);
  for (const k of ["module", "function", "method", "class", "namespace", "constant", "example"]) assert.ok(info.kinds.includes(k), k);
  assert.ok(info.top.some((p) => p.endsWith("set_voltage")), info.top.join(","));
  await fresh("x = 1\n");
  const first = await page.evaluate(() => window.h.insertDoc("daq.vdut"));
  assert.deepEqual(first.missingImports, ["import daq"]);
  assert.equal(await src(), "import daq\n\nx = 1\ndaq.vdut()");
  const second = await page.evaluate(() => window.h.insertDoc("daq.present"));
  assert.deepEqual(second.missingImports, []);
  assert.equal((await src()).split("import daq").length, 2);
  await fresh("");
  const exName = await page.evaluate(() => window.bbScriptCatalogue.examples()[0].name);
  await page.evaluate((n) => window.h.insertDoc(`example:${n}`), exName);
  const example = await page.evaluate(() => window.bbScriptCatalogue.examples()[0].source);
  assert.equal((await src()).trimEnd(), example.trimEnd());
});

await step("theme follows tokens and switches without raw colours", async () => {
  await fresh("import daq\nx = 'a'");
  const read = () =>
    page.evaluate(() => {
      const ed = document.querySelector(".cm-editor");
      const g = document.querySelector(".cm-gutters");
      return { bg: getComputedStyle(ed).backgroundColor, fg: getComputedStyle(ed).color, gutter: getComputedStyle(g).backgroundColor, tokenBg: getComputedStyle(document.documentElement).getPropertyValue("--surface-1") };
    });
  const light = await read();
  await page.evaluate(() => {
    document.documentElement.setAttribute("data-theme", "dark");
    window.h.setTheme("dark");
  });
  const dark = await read();
  assert.notEqual(light.bg, dark.bg);
  assert.notEqual(light.fg, dark.fg);
  await page.evaluate(() => {
    window.h.setTheme("inherit");
    document.documentElement.setAttribute("data-theme", "light");
  });
  assert.equal((await read()).bg, light.bg);
});

await step("invalid catalogue: labelled failure, editor still works, no page errors", async () => {
  const before = problems.length;
  const bad = await page.evaluate(() => [
    window.bbScriptCatalogue.set("{not json"),
    window.bbScriptCatalogue.set({ schema: "bugbuster.firmware-api/9", modules: [] }),
    window.bbScriptCatalogue.set({ schema: "bugbuster.firmware-api/1" }),
  ]);
  assert.deepEqual(bad.map((s) => [s.state, s.code]), [["failed", "invalid"], ["failed", "unsupported-schema"], ["failed", "invalid"]]);
  assert.ok(bad[1].message.includes("bugbuster.firmware-api/9"));
  await fresh("import daq\ndaq.");
  await typeText("vd");
  await expectNoPopup();
  assert.equal(await src(), "import daq\ndaq.vd");
  assert.deepEqual(await page.evaluate(() => window.bbScriptCatalogue.flatten()), []);
  const missing = await page.evaluate(() => window.bbScriptCatalogue.load("/nope/firmware-api.json"));
  assert.equal(missing.state, "failed");
  assert.equal(missing.code, "http");
  assert.equal((await page.evaluate(() => window.bbScriptCatalogue.load())).state, "ready");
  assert.equal(problems.slice(before).filter((p) => !p.includes("404")).length, 0, problems.slice(before).join("; "));
});

await step("destroy removes the editor and listeners; remount works", async () => {
  await page.evaluate(() => window.h.destroy());
  assert.equal(await page.locator(".cm-editor").count(), 0);
  await fresh("print('again')");
  assert.equal(await page.locator(".cm-editor").count(), 1);
});

if (wantShots) {
  mkdirSync(shotDir, { recursive: true });
  for (const theme of ["light", "dark"]) {
    await page.evaluate((th) => document.documentElement.setAttribute("data-theme", th), theme);
    await fresh('"""Demo"""\nimport daq\nimport bugbuster\n\nch = bugbuster.Channel(0)\n# comment\nch.set_voltage(1.5, \ndaq.run.');
    await page.evaluate(() => window.h.setDiagnostics([{ line: 7, column: 1, severity: "error", message: "device syntax: unexpected token", source: "device" }]));
    await typeText("st");
    await openPopup();
    await page.locator(".cm-tooltip-autocomplete li").first().waitFor();
    await page.screenshot({ path: join(shotDir, `editor-completion-${theme}.png`) });
    await page.keyboard.press("Escape");
  }
  await page.evaluate(() => document.documentElement.setAttribute("data-theme", "light"));
  await fresh("import bugbuster\nch = bugbuster.Channel(0)\nch.set_voltage(1.5, ");
  await page.locator(".cm-tooltip-signature").waitFor();
  await page.screenshot({ path: join(shotDir, "editor-signature-light.png") });
  await fresh("foo bar foo\nfoo");
  await page.evaluate(() => window.h.search());
  await page.locator(".cm-search").waitFor();
  await page.locator('.cm-search input[name="search"]').pressSequentially("foo");
  await page.locator(".cm-searchMatch").first().waitFor();
  await page.screenshot({ path: join(shotDir, "editor-search-light.png") });
}

await step("no external requests, no page/console errors", async () => {
  assert.deepEqual(external, []);
  assert.deepEqual(problems.filter((p) => !p.includes("404")), []);
});

await browser.close();
server.close();
const failed = results.filter((r) => !r.ok);
console.log(`\n${results.length - failed.length}/${results.length} passed`);
process.exit(failed.length ? 1 : 0);
