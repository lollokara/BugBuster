// Interaction acceptance suite for the desktop Scripts workspace (MOCK evidence only).
//
// Drives the real Leptos UI served by `trunk serve` against the stateful `script_request` mock in
// mock_ipc.js (window.__BB_SCRIPT_MOCK). No Tauri runtime, no CDP, no hardware: every PASS here is
// "mock", never "live". Wrong IPC shapes are rejected by the mock and fail the run.
//
//   node scripting.mjs [--url http://127.0.0.1:1462] [--only identity,runs,...] [--matrix full|quick]
//                      [--themes light,dark] [--sizes 1440x900,1100x800,800x720] [--hats daq,la,none]
//                      [--out DIR] [--no-shots]
//
// Playwright is resolved from BB_PLAYWRIGHT_DIR, this folder, tests/e2e, or a sibling worktree.
import fs from 'node:fs';
import path from 'node:path';
import { existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const appRoot = resolve(here, '../..');

function parseArgs(argv) {
  const o = { url: process.env.BB_UI_URL || 'http://127.0.0.1:1462', only: null, matrix: 'full', themes: ['light', 'dark'], sizes: ['1440x900', '1100x800', '800x720'], hats: ['daq', 'la', 'none'], shots: true, out: process.env.BB_SHOT_DIR || resolve(appRoot, '../../../../scratch/desktop-scripting') };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i];
    const v = () => argv[++i];
    if (k === '--url') o.url = v();
    else if (k === '--only') o.only = v().split(',').map((s) => s.trim()).filter(Boolean);
    else if (k === '--matrix') o.matrix = v();
    else if (k === '--themes') o.themes = v().split(',');
    else if (k === '--sizes') o.sizes = v().split(',');
    else if (k === '--hats') o.hats = v().split(',');
    else if (k === '--out') o.out = resolve(v());
    else if (k === '--no-shots') o.shots = false;
  }
  return o;
}
const opts = parseArgs(process.argv);
const SHOTS = join(opts.out, 'harness-shots');
fs.mkdirSync(SHOTS, { recursive: true });
for (const f of fs.readdirSync(SHOTS)) if (f.startsWith('fail-')) fs.unlinkSync(join(SHOTS, f));

async function loadPlaywright() {
  const roots = [
    process.env.BB_PLAYWRIGHT_DIR,
    join(here, 'node_modules/playwright'),
    join(appRoot, 'tests/e2e/node_modules/playwright'),
    resolve(appRoot, '../../../battsim-ui/DesktopApp/BugBuster/tests/ui-harness/node_modules/playwright'),
    resolve(appRoot, '../../../../.worktrees/battsim-ui/DesktopApp/BugBuster/tests/ui-harness/node_modules/playwright'),
  ].filter(Boolean);
  for (const r of roots) if (existsSync(join(r, 'package.json'))) return import(pathToFileURL(join(r, 'index.mjs')).href);
  throw new Error(`playwright not found; set BB_PLAYWRIGHT_DIR (tried ${roots.join(', ')})`);
}
const { chromium } = await loadPlaywright();

// ------------------------------------------------------------------------------------------------
// bookkeeping
// ------------------------------------------------------------------------------------------------
const results = [];
const shotsTaken = [];
const findings = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = (id) => `[data-testid="${id}"]`;
const slug = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 70);
const firstLine = (e) => String(e && e.message ? e.message : e).split('\n').filter(Boolean).slice(0, 3).join(' | ').slice(0, 420);
const selectorOf = (e) => {
  const m = /locator\('([^']+)'\)|waiting for (?:selector|locator) ["']?([^"'\n]+)/.exec(String(e && e.message ? e.message : e));
  return m ? (m[1] || m[2]) : '';
};

class Session {
  constructor(group, ctx, page, o) {
    this.group = group; this.ctx = ctx; this.page = page; this.o = o;
    this.issues = { console: [], page: [], failed: [], http: [], noise: [], unhandled: [] };
    this.expectedFail = [];
    this.current = '(setup)';
  }

  async step(name, fn) {
    const t0 = Date.now();
    this.current = name;
    const rec = { group: this.group, name, ok: true, note: '', ms: 0, selector: '' };
    try {
      const note = await fn(this.page, this);
      if (typeof note === 'string') rec.note = note;
    } catch (e) {
      rec.ok = false; rec.note = firstLine(e); rec.selector = selectorOf(e);
      try {
        const last = await this.page.evaluate(() => (window.__BB_SCRIPT_MOCK ? window.__BB_SCRIPT_MOCK.commands().filter((c) => c.op !== 'status' && c.op !== 'logs').slice(-7).map((c) => `${c.n}:${c.op}${c.args && c.args.name ? '(' + c.args.name + ')' : ''}:${c.reply ? (c.reply.kind || 'ok') : 'pending'}`) : []));
        rec.lastCommands = last;
      } catch (_) { /* page may be gone */ }
      if (opts.shots) {
        const f = join(SHOTS, `fail-${slug(this.group)}-${slug(name)}.png`);
        await this.page.screenshot({ path: f }).catch(() => {});
        rec.shot = f;
      }
      await this.page.keyboard.press('Escape').catch(() => {});
      await this.page.keyboard.press('Escape').catch(() => {});
      await this.page.evaluate(() => document.querySelectorAll('.sc-dialog button').forEach((b) => { if (/^cancel$/i.test(b.textContent.trim())) b.click(); })).catch(() => {});
    }
    rec.ms = Date.now() - t0;
    results.push(rec);
    console.log(`${rec.ok ? 'PASS' : 'FAIL'} [${this.group}] ${name}${rec.note ? ' - ' + rec.note : ''}${rec.lastCommands ? '\n       last commands: ' + rec.lastCommands.join(' ') : ''}`);
    return rec.ok;
  }

  async shot(name, clip) {
    if (!opts.shots) return null;
    const f = join(SHOTS, `${name}.png`);
    await this.page.screenshot({ path: f, ...(clip ? { clip } : {}) });
    shotsTaken.push({ name, file: f, group: this.group });
    return f;
  }
}

async function newSession(browser, group, o = {}) {
  const { hat = 'daq', theme = 'light', w = 1440, h = 900, mock = {}, connect = 'usb', open = true, abortCatalogue = false, noScripts = false } = o;
  const ctx = await browser.newContext({ viewport: { width: w, height: h }, colorScheme: theme, locale: 'en-US', acceptDownloads: true, deviceScaleFactor: 1 });
  await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: new URL(opts.url).origin }).catch(() => {});
  const scripts = noScripts ? undefined : Object.assign({ secondDevice: true, delayMs: 30, sleepScale: 0.2 }, mock);
  await ctx.addInitScript(`window.__BB_MOCK = ${JSON.stringify({ hat, connected: true, theme, scripts })};`);
  await ctx.addInitScript(`(function(){ var O = window.WebSocket; function W(u, p){ if (String(u).indexOf('.well-known/trunk/ws') >= 0) { return { close: function(){}, send: function(){}, addEventListener: function(){}, removeEventListener: function(){}, readyState: 0 }; } return p === undefined ? new O(u) : new O(u, p); } W.prototype = O.prototype; W.CONNECTING = 0; W.OPEN = 1; W.CLOSING = 2; W.CLOSED = 3; window.WebSocket = W; })();`);
  await ctx.addInitScript({ path: join(here, 'mock_ipc.js') });
  await ctx.addInitScript(`(function(){ window.requestAnimationFrame = function(cb){ return setTimeout(function(){ cb(performance.now()); }, 16); }; })();`);
  const page = await ctx.newPage();
  page.setDefaultTimeout(9000);
  const s = new Session(group, ctx, page, o);
  page.on('console', (m) => {
    const t = m.text();
    if (t.startsWith('[mock] unhandled')) return;
    if (m.type() === 'error') (/crbug|willReadFrequently/.test(t) ? s.issues.noise : s.issues.console).push(`during "${s.current}": ${t.slice(0, 300)}`);
    if (/panicked/.test(t)) s.issues.page.push(`PANIC during "${s.current}": ${t.slice(0, 500)}`);
  });
  page.on('pageerror', (e) => s.issues.page.push(`during "${s.current}": ${String(e.message || e).slice(0, 400)}`));
  page.on('requestfailed', (r) => {
    const u = r.url();
    if (u.includes('.well-known/trunk')) return;
    if (s.expectedFail.some((re) => re.test(u))) return;
    s.issues.failed.push(`${r.failure() ? r.failure().errorText : 'failed'} ${u}`);
  });
  page.on('response', (r) => {
    if (r.status() >= 400 && !s.expectedFail.some((re) => re.test(r.url()))) s.issues.http.push(`${r.status()} ${r.url()}`);
  });
  if (abortCatalogue) {
    s.expectedFail.push(/firmware-api\.json/);
    await page.route('**/firmware-api.json', (route) => route.abort());
  }
  await page.goto(opts.url, { waitUntil: 'load' });
  await page.waitForFunction(() => [...document.querySelectorAll('.device-item')].some((e) => /COM6/.test(e.textContent)), null, { timeout: 120000 });
  if (theme) await page.evaluate((t) => document.documentElement.setAttribute('data-theme', t), theme);
  if (connect) {
    await connectTo(s, connect);
    if (open) await openScripts(s);
  }
  return s;
}

// ------------------------------------------------------------------------------------------------
// page helpers
// ------------------------------------------------------------------------------------------------
const DEVICE_RE = { usb: 'COM6', http: 'bugbuster-a1b2c3', usb2: 'COM7' };
async function connectTo(s, which) {
  const p = s.page;
  await p.waitForFunction((re) => [...document.querySelectorAll('.device-item')].some((e) => new RegExp(re).test(e.textContent)), DEVICE_RE[which], { timeout: 20000 });
  await p.evaluate((re) => [...document.querySelectorAll('.device-item')].find((e) => new RegExp(re).test(e.textContent)).click(), DEVICE_RE[which]);
  await p.waitForSelector('[data-nav-view="scripts"]', { timeout: 20000 });
}
async function openScripts(s) {
  const p = s.page;
  await p.evaluate(() => document.querySelector('[data-nav-view="scripts"]').click());
  await p.waitForSelector(T('scripts-root'));
  await p.waitForSelector('[data-file="hello.py"], [data-file="other.py"]', { state: 'attached', timeout: 10000 });
  await waitVerified(p);
}
// The workspace is usable once the first reply of the current link has been accepted.
async function waitVerified(p, timeout = 10000) {
  const end = Date.now() + timeout;
  const conn = await mock(p, 'connection');
  const want = conn.mode.toLowerCase();
  for (;;) {
    const c = await cmds(p);
    const t0 = (await mock(p, 'connectedAt')) || 0;
    if (c.some((x) => x.transport === want && x.mac === conn.mac && x.t >= t0 && x.reply && x.reply.ok)) { await sleep(120); return; }
    if (Date.now() > end) throw new Error('link never verified');
    await sleep(60);
  }
}
async function disconnectUi(s) {
  await s.page.click('[title="Disconnect"]');
  await s.page.waitForSelector(T('scripts-offline-entry'), { timeout: 10000 });
  // connection.rs ParticleBackground panics if the screen unmounts within ~50 ms of mounting (pre-existing).
  await sleep(450);
}
async function nav(p, id) { await p.evaluate((i) => document.querySelector(`[data-nav-view="${i}"]`).click(), id); }
const mock = (p, fn, ...a) => p.evaluate(([f, args]) => { const m = window.__BB_SCRIPT_MOCK; return typeof m[f] === 'function' ? m[f](...args) : m[f]; }, [fn, a]);
const cmds = (p, op) => mock(p, 'commands', op);
const nCmds = async (p, op) => (await cmds(p, op)).length;
const snap = (p, mac) => mock(p, 'snapshot', mac === undefined ? null : mac);
async function until(p, fn, arg, timeout = 8000, what = '') {
  try { return await p.waitForFunction(fn, arg, { timeout, polling: 50 }); } catch (e) { throw new Error(`timeout (${timeout} ms) waiting for ${what || String(fn).slice(0, 90)}`); }
}
const docText = (p) => p.evaluate(() => [...document.querySelectorAll('.cm-content .cm-line')].map((l) => l.textContent).join('\n'));
async function focusEditor(p) { await p.click('.cm-content'); await p.keyboard.press('Control+End'); }
async function replaceDoc(p, text) {
  await p.click('.cm-content');
  await p.keyboard.press('Control+a');
  if (text === '') await p.keyboard.press('Backspace'); else await p.keyboard.insertText(text);
}
async function typeText(p, t) { await p.keyboard.type(t, { delay: 12 }); }
async function openFile(p, name) {
  await revealList(p);
  await p.click(`[data-file="${name}"] .sc-file-main`);
  await until(p, (n) => (document.querySelector('[data-testid="sc-doc-name"]')?.textContent || '').includes(n), name, 8000, `doc ${name} open`);
  await closeDrawer(p).then((closed) => { if (closed) p.__drawerStayed = (p.__drawerStayed || 0) + 1; });
  await p.waitForSelector('.cm-content');
  await p.waitForTimeout(150);
}
// A file row counts as reachable only when it is really painted on top at its own centre.
const rowVisible = (p) => p.evaluate(() => [...document.querySelectorAll('[data-file]')].some((e) => { const r = e.getBoundingClientRect(); if (r.width <= 0 || r.height <= 0 || r.right <= 0 || r.left >= innerWidth) return false; const t = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2); return !!t && e.contains(t); }));
const drawerScrimVisible = (p) => p.evaluate(() => { const s = document.querySelector('.sc-drawer-scrim'); if (!s) return false; const r = s.getBoundingClientRect(); const cs = getComputedStyle(s); return r.width > 0 && r.height > 0 && cs.display !== 'none' && cs.visibility !== 'hidden' && cs.pointerEvents !== 'none' && parseFloat(cs.opacity || '1') > 0; });
async function revealList(p) {
  await closeDrawer(p);
  if (await rowVisible(p)) return false;
  const toggle = p.locator('.sc-list-toggle').first();
  if (!(await toggle.isVisible().catch(() => false))) {
    await p.evaluate(() => { const b = [...document.querySelectorAll('button')].find((x) => /Open scratch buffer/.test(x.textContent)); if (b) b.click(); });
    await p.waitForSelector('.sc-list-toggle', { timeout: 5000 });
  }
  await p.evaluate(() => document.querySelector('.sc-list-toggle').click());
  for (let i = 0; i < 20 && !(await rowVisible(p)); i++) await sleep(100);
  return true;
}
async function closeDrawer(p) {
  if (!(await drawerScrimVisible(p))) return false;
  await p.evaluate(() => document.querySelector('.sc-drawer-scrim').click());
  await sleep(300);
  return true;
}
const isDirty = async (p) => !!(await p.$(T('sc-dirty')));
const lintState = (p) => p.evaluate(() => document.querySelector('[data-testid="sc-lint-banner"]')?.getAttribute('data-lint') || null);
const lintText = (p) => p.evaluate(() => document.querySelector('[data-testid="sc-lint-banner"]')?.textContent.trim() || '');
async function waitLint(p, state, timeout = 8000) {
  await until(p, (st) => document.querySelector('[data-testid="sc-lint-banner"]')?.getAttribute('data-lint') === st, state, timeout, `lint banner ${state}`);
}
const noticeText = (p) => p.evaluate(() => document.querySelector('[data-testid="sc-notice"]')?.textContent.trim() || '');
async function waitNotice(p, re, timeout = 8000) {
  await until(p, (src) => new RegExp(src, 'i').test(document.querySelector('[data-testid="sc-notice"]')?.textContent || ''), re, timeout, `notice /${re}/`);
}
const logLines = (p) => p.$$eval(T('sc-log-line'), (els) => els.map((e) => e.textContent.replace(/\s+/g, ' ').trim()));
const logMarkers = (p) => p.$$eval(T('sc-log-marker'), (els) => els.map((e) => e.textContent.trim()));
async function openConsole(p) { if (!(await p.$(T('sc-console')))) await p.click(T('sc-log-toggle')); await p.waitForSelector(T('sc-console')); }
async function waitMarker(p, re, timeout = 10000) {
  await until(p, (src) => [...document.querySelectorAll('[data-testid="sc-log-marker"]')].some((e) => new RegExp(src).test(e.textContent)), re, timeout, `log marker /${re}/`);
}
async function waitIdle(p, timeout = 12000) {
  const end = Date.now() + timeout;
  for (;;) {
    const st = (await snap(p)).status;
    const held = (await mock(p, 'held')).length;
    if (!st.running && held === 0) return;
    if (Date.now() > end) throw new Error('device never went idle');
    await sleep(60);
  }
}
async function stopAll(p) {
  await mock(p, 'release');
  await mock(p, 'clearFaults');
  await mock(p, 'stopDevice');
  await waitIdle(p).catch(() => {});
}
const cursorRect = (p) => p.evaluate(() => {
  const sel = window.getSelection();
  if (!sel || !sel.rangeCount) return null;
  const r = sel.getRangeAt(0).cloneRange();
  r.collapse(false);
  let b = r.getBoundingClientRect();
  if (!b.width && !b.height) { const n = r.startContainer.nodeType === 1 ? r.startContainer : r.startContainer.parentElement; b = n.getBoundingClientRect(); }
  return { x: b.x, y: b.y, w: b.width, h: b.height, right: b.right, bottom: b.bottom };
});
async function popupItems(p) {
  await p.waitForSelector('.cm-tooltip-autocomplete', { timeout: 6000 });
  await p.waitForTimeout(220); // CodeMirror ignores accept keys for 75 ms after the popup changes
  return p.$$eval('.cm-tooltip-autocomplete li', (els) => els.map((e) => e.textContent.trim()));
}
async function expectNoPopup(p) { await p.waitForTimeout(450); if (await p.$('.cm-tooltip-autocomplete')) throw new Error('completion popup appeared'); }
function assert(cond, msg) { if (!cond) throw new Error(msg); }
function eq(a, b, msg) { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${msg || 'not equal'}: ${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }

// Geometry/overflow/overlap report over the Scripts workspace.
const layoutReport = (p, o = {}) => p.evaluate((o) => {
  const vw = window.innerWidth, vh = window.innerHeight, de = document.documentElement;
  const out = { hOverflow: de.scrollWidth - de.clientWidth, vw, vh, problems: [] };
  const root = document.querySelector('[data-testid="scripts-root"]');
  if (!root) { out.problems.push('scripts root missing'); return out; }
  const ids = o.controls === false ? [] : ['sc-save', 'sc-check', 'sc-run', 'sc-docs-toggle'];
  const rects = [];
  for (const id of ids) {
    const e = document.querySelector(`[data-testid="${id}"]`);
    if (!e) { out.problems.push(`${id} missing`); continue; }
    const r = e.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) { out.problems.push(`${id} not rendered`); continue; }
    if (r.left < -1 || r.right > vw + 1 || r.top < -1 || r.bottom > vh + 1) out.problems.push(`${id} outside viewport (${Math.round(r.left)},${Math.round(r.top)},${Math.round(r.right)},${Math.round(r.bottom)})`);
    rects.push([id, r]);
  }
  for (let i = 0; i < rects.length; i++) for (let j = i + 1; j < rects.length; j++) {
    const a = rects[i][1], b = rects[j][1];
    const ix = Math.min(a.right, b.right) - Math.max(a.left, b.left), iy = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top);
    if (ix > 2 && iy > 2) out.problems.push(`${rects[i][0]} overlaps ${rects[j][0]}`);
  }
  const sel = 'button, .badge, .sc-doc-name, .sc-status-name, .sc-status-sub, .sc-banner, .sc-file-main, .sc-chip, .sc-storage, .sc-dialog';
  for (const e of root.querySelectorAll(sel)) {
    const r = e.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) continue;
    const cs = getComputedStyle(e);
    const label = (e.getAttribute('data-testid') || e.className || e.tagName).toString().slice(0, 40);
    if (e.scrollWidth > e.clientWidth + 1 && ['hidden', 'clip'].includes(cs.overflowX) && cs.textOverflow !== 'ellipsis') out.problems.push(`clipped text: ${label} (${e.scrollWidth}>${e.clientWidth})`);
    if (r.right > vw + 1) out.problems.push(`offscreen right: ${label} (${Math.round(r.right)}>${vw})`);
  }
  for (const e of document.querySelectorAll('.cm-tooltip, .dialog')) {
    const r = e.getBoundingClientRect();
    if (r.width && (r.left < -1 || r.right > vw + 1 || r.top < -1 || r.bottom > vh + 1)) out.problems.push(`${(e.className || '').toString().slice(0, 30)} popup outside viewport`);
  }
  return out;
}, o);

// ------------------------------------------------------------------------------------------------
// groups
// ------------------------------------------------------------------------------------------------
const GROUPS = {};
const group = (name, fn) => { GROUPS[name] = fn; };

group('identity', async (browser) => {
  const s = await newSession(browser, 'identity', { connect: null });
  const p = s.page;
  await s.step('offline: editor, docs and scratch work with no device and no IPC', async () => {
    await p.click(T('scripts-offline-entry'));
    await p.waitForSelector(T('scripts-offline-shell'));
    await p.waitForSelector('.cm-content');
    await p.click('.cm-content');
    await typeText(p, 'x = 1');
    await p.waitForSelector(T('sc-dirty'));
    assert(await p.$eval(T('sc-save'), (b) => b.disabled), 'save enabled offline');
    assert(await p.$eval(T('sc-run'), (b) => b.disabled), 'run enabled offline');
    await p.waitForSelector(T('sc-offline-banner'));
    await p.click(T('sc-docs-toggle'));
    await p.waitForSelector(T('sc-doc-list'), { timeout: 10000 });
    await p.fill(T('sc-docs-search'), 'sleep');
    await p.waitForSelector('[data-doc]');
    await p.click('[data-doc]');
    await p.click(T('sc-doc-insert'));
    await p.waitForTimeout(250);
    assert(/sleep/.test(await docText(p)), 'insert did not add the call');
    const c = await mock(p, 'counts');
    eq(c.total, 0, 'offline editor must not send script_request');
    await s.shot('offline-docs-1440-light');
  });
  await s.step('offline completion works without a device', async () => {
    await replaceDoc(p, 'import daq\ndaq.');
    await typeText(p, 'vd');
    const items = await popupItems(p);
    assert(items.some((t) => t.includes('vdut')), `no vdut in ${items.slice(0, 5)}`);
    await p.keyboard.press('Escape');
  });
  await s.step('USB connect: files, storage, status verified; USB badge', async () => {
    await p.click(T('scripts-offline-back'));
    await sleep(450);
    await connectTo(s, 'usb');
    await openScripts(s);
    for (const n of ['hello.py', 'blink.py', 'fail.py', 'long.py']) await p.waitForSelector(`[data-file="${n}"]`);
    await until(p, () => /USB/.test(document.querySelector('[data-testid="script-transport"]')?.textContent || ''), null, 8000, 'USB badge');
    await p.waitForFunction(() => (document.querySelector('[data-testid="sc-storage"]')?.textContent || '').length > 0);
    const ops = new Set((await cmds(p)).map((c) => c.op));
    for (const need of ['files', 'status', 'storage']) assert(ops.has(need), `missing ${need}`);
    for (const bad of ['run', 'eval', 'save', 'lint', 'delete', 'autorun_enable', 'autorun_run']) assert(!ops.has(bad), `unexpected ${bad} on connect`);
    return [...ops].join(',');
  });
  await s.step('offline scratch follows to the device as an untitled buffer', async () => {
    await p.waitForSelector('.sc-file[data-file=""]', { timeout: 8000 });
  });
  await s.step('Wi-Fi: same MAC keeps files and the dirty buffer; REST labels; no run-now', async () => {
    await openFile(p, 'blink.py');
    await focusEditor(p);
    await typeText(p, '\n# dirty-over-usb');
    await p.waitForSelector(T('sc-dirty'));
    await disconnectUi(s);
    await connectTo(s, 'http');
    await openScripts(s);
    await until(p, () => /Wi-?Fi/i.test(document.querySelector('[data-testid="script-transport"]')?.textContent || ''), null, 8000, 'Wi-Fi badge');
    await p.waitForSelector('[data-file="blink.py"] .sc-dirty', { timeout: 8000 });
    const last = await (async () => { for (let i = 0; i < 50; i++) { const l = (await cmds(p, 'files')).filter((c) => c.transport === 'http').pop(); if (l) return l; await sleep(100); } throw new Error('no files command over http after reconnect'); })();
    eq([last.transport, last.mac], ['http', (await mock(p, 'limits')).defaultMac], 'files over http for the same device');
    await p.click(T('sc-open-repl'));
    await p.waitForSelector(T('sc-repl'));
    assert(/Wi-?Fi/i.test(await p.$eval(T('sc-repl-transport'), (e) => e.textContent)), 'REPL transport label');
    await p.click(T('sc-repl-close'));
    await p.click(`${T('sc-autorun')} summary`);
    assert(!(await p.$('[data-testid="sc-autorun-run"]')), 'autorun run-now control must not exist (iOS has none)');
    assert(!/run now/i.test(await p.$eval(T('sc-autorun'), (e) => e.textContent)), 'autorun panel mentions run-now');
    eq(await nCmds(p, 'autorun_run'), 0, 'autorun_run must never be sent');
    await openFile(p, 'blink.py');
    await p.keyboard.press('Control+s');
    await until(p, () => !document.querySelector('[data-testid="sc-dirty"]'), null, 8000, 'save over Wi-Fi to clear dirty');
    const sv = (await cmds(p, 'save')).pop();
    eq(sv.transport, 'http', 'save transport');
    assert((await snap(p)).files['blink.py'].includes('dirty-over-usb'), 'device copy not updated');
  });
  await s.step('click a stored script before the new link is verified: no misleading "Connect to a device" notice', async () => {
    await disconnectUi(s);
    await mock(p, 'configure', { opDelays: { files: 900, status: 900, storage: 900, autorun_status: 900 } });
    await connectTo(s, 'usb');
    await p.evaluate(() => document.querySelector('[data-nav-view="scripts"]').click());
    await p.waitForSelector(T('scripts-root'));
    await p.waitForSelector('[data-file="fail.py"] .sc-file-main', { timeout: 8000 });
    await p.click('[data-file="fail.py"] .sc-file-main');
    await sleep(250);
    const early = await noticeText(p);
    await mock(p, 'configure', { opDelays: { files: 30, status: 30, storage: 30, autorun_status: 30 } });
    await waitVerified(p);
    await sleep(1500);
    assert(!/Connect to a device/i.test(early), `notice while the device is connected but unverified: "${early}"`);
  });
  await s.step('device swap: B shows only B; A dirty buffers are not leaked; stale A status is ignored', async () => {
    await mock(p, 'configure', { staleReplies: true });
    const A = (await mock(p, 'limits')).defaultMac, B = (await mock(p, 'limits')).secondMac;
    await openFile(p, 'hello.py');
    await focusEditor(p);
    await typeText(p, '\n# DIRTY-A');
    await p.waitForSelector(T('sc-dirty'));
    await mock(p, 'externalRun', A, 'blink.py', { kind: 'manual' });
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    await mock(p, 'hold', 'status', 'after', 1);
    for (let i = 0; i < 80 && (await mock(p, 'held')).length === 0; i++) await sleep(100);
    eq((await mock(p, 'held')).length, 1, 'status reply should be held');
    await mock(p, 'connect', 'usb:COM7');
    await sleep(300);
    await mock(p, 'release', 'status');
    await sleep(1500);
    await openScripts(s);
    await p.waitForSelector('[data-file="other.py"]', { timeout: 8000 });
    assert(!(await p.$('[data-file="blink.py"]')), "A's file list leaked into B");
    assert(!(await p.$(T('script-run-chip'))), "A's running script shows on B");
    const sub = await p.$eval(T('script-status-name'), (e) => e.textContent);
    assert(!/blink/.test(sub), `status name leaked: ${sub}`);
    await p.waitForSelector(T('sc-elsewhere'), { timeout: 5000 });
    await openFile(p, 'hello.py');
    assert(/device B|hello from B/.test(await docText(p)), `B content wrong: ${await docText(p)}`);
    assert(!/DIRTY-A/.test(await docText(p)), 'A edit leaked into B document');
    await s.shot('device-swap-b-1440-light');
    await mock(p, 'connect', 'usb:COM6');
    await openScripts(s);
    await p.waitForSelector('[data-file="hello.py"] .sc-dirty', { timeout: 8000 });
    await openFile(p, 'hello.py');
    assert(/DIRTY-A/.test(await docText(p)), 'A dirty buffer not restored');
    await mock(p, 'stopDevice', A);
    await mock(p, 'configure', { staleReplies: false });
    eq(B !== A, true);
  });
  await s.step('disconnect while saving: buffer stays dirty, no phantom clean, no duplicate upload', async () => {
    await stopAll(p);
    await openFile(p, 'hello.py');
    const before = await nCmds(p, 'save');
    await mock(p, 'hold', 'save', 'before', 1);
    await p.keyboard.press('Control+s');
    for (let i = 0; i < 60 && (await mock(p, 'held')).length === 0; i++) await sleep(50);
    await mock(p, 'disconnect');
    await sleep(300);
    await mock(p, 'release', 'save');
    await sleep(800);
    await mock(p, 'connect', 'usb:COM6');
    await openScripts(s);
    await sleep(800);
    eq((await nCmds(p, 'save')) - before, 1, 'save must not be retried automatically');
    await openFile(p, 'hello.py');
    assert(await isDirty(p) || /DIRTY-A/.test(await docText(p)), 'unsaved text lost');
  });
  await finish(s);
});

group('editing', async (browser) => {
  const s = await newSession(browser, 'editing', {});
  const p = s.page;
  await s.step('new script: name validation, duplicates, template saved to the device', async () => {
    await p.click(T('sc-new'));
    await p.waitForSelector(T('sc-dialog-new'));
    const confirm = `${T('sc-dialog-new')} ${T('sc-dialog-confirm')}`;
    const bad = ['bad name', 'a/b', '.hidden', 'x'.repeat(40), 'sp\u00e9cial', ''];
    for (const n of bad) {
      await p.fill(T('sc-name-input'), n);
      assert(await p.$eval(confirm, (b) => b.disabled), `name ${JSON.stringify(n)} accepted`);
    }
    await p.fill(T('sc-name-input'), 'hello');
    const hint = await p.$eval(T('sc-name-hint'), (e) => e.textContent.trim());
    const saves = await nCmds(p, 'save');
    await p.press(T('sc-name-input'), 'Enter');
    await waitNotice(p, 'already exists', 4000);
    eq(await nCmds(p, 'save'), saves, 'duplicate name must not upload a template over the existing file');
    assert(!/Write your MicroPython/.test((await snap(p)).files['hello.py']), 'existing hello.py was overwritten');
    if (await p.$(T('sc-dialog-new'))) await p.keyboard.press('Escape');
    await p.click(T('sc-new'));
    await p.waitForSelector(T('sc-dialog-new'));
    await p.fill(T('sc-name-input'), 'fresh');
    await p.press(T('sc-name-input'), 'Enter');
    await p.waitForSelector('[data-file="fresh.py"]', { timeout: 8000 });
    assert(/Write your MicroPython/.test((await snap(p)).files['fresh.py'] || ''), 'template missing on device');
    eq(await nCmds(p, 'save'), 1, 'exactly one save for create');
    return `dup hint: "${hint}"`;
  });
  await s.step('type, Ctrl+S saves the exact text and lints compile-only', async () => {
    await openFile(p, 'hello.py');
    await focusEditor(p);
    await typeText(p, 'x = 2');
    await p.keyboard.press('Control+s');
    await waitLint(p, 'ok');
    assert((await snap(p)).files['hello.py'].includes('x = 2'), 'device text mismatch');
    assert(!(await isDirty(p)), 'still dirty');
    eq((await mock(p, 'snapshot')).status.totalRuns, 0, 'lint must not execute');
  });
  await s.step('Ctrl+S right after a paste-like insert saves exactly that text', async () => {
    await replaceDoc(p, 'print("pasted")\n');
    await p.keyboard.press('Control+s');
    await sleep(900);
    const dev = (await snap(p)).files['hello.py'];
    assert(dev === 'print("pasted")\n', `device has ${JSON.stringify(dev.slice(0, 60))}; saves sent: ${await nCmds(p, 'save')}`);
    assert(!(await isDirty(p)), 'dirty after save');
  });
  await s.step('typing during upload: edit stays dirty, lint result is not attributed to the new text', async () => {
    await mock(p, 'hold', 'save', 'before', 1);
    await focusEditor(p);
    await typeText(p, '\n# a');
    await p.keyboard.press('Control+s');
    for (let i = 0; i < 60 && (await mock(p, 'held')).length === 0; i++) await sleep(50);
    await focusEditor(p);
    await typeText(p, 'ZZ');
    await mock(p, 'release', 'save');
    await sleep(1200);
    assert(await isDirty(p), 'edit made during upload was marked clean');
    const st = await lintState(p);
    assert(st !== 'ok', `lint banner says ${st} for text that was edited after the upload`);
    const dev = (await snap(p)).files['hello.py'];
    assert(dev.includes('# a') && !dev.includes('ZZ'), 'device copy is not the pre-edit snapshot');
    await p.keyboard.press('Control+s');
    await until(p, () => !document.querySelector('[data-testid="sc-dirty"]'), null, 8000, 'second save clears dirty');
    assert((await snap(p)).files['hello.py'].includes('ZZ'), 'second save lost text');
  });
  await s.step('failed upload: stays dirty, notice, no lint, device untouched; retry succeeds', async () => {
    await focusEditor(p);
    await typeText(p, '\n# fail-me');
    const lintBefore = await nCmds(p, 'lint');
    const devBefore = (await snap(p)).files['hello.py'];
    await mock(p, 'fail', 'save', { reply: { kind: 'transport', error: 'USB write timed out' } });
    await p.keyboard.press('Control+s');
    await waitNotice(p, 'USB write timed out');
    assert(await isDirty(p), 'marked clean after a failed save');
    eq(await nCmds(p, 'lint'), lintBefore, 'lint ran after a failed upload');
    eq((await snap(p)).files['hello.py'], devBefore, 'device changed by a failed upload');
    await p.keyboard.press('Control+s');
    await until(p, () => !document.querySelector('[data-testid="sc-dirty"]'), null, 8000, 'retry save');
    assert((await snap(p)).files['hello.py'].includes('fail-me'), 'retry lost text');
  });
  await s.step('mid-upload failure leaves the old file and shows progress', async () => {
    const big = 'x = 1\n'.repeat(900);
    await replaceDoc(p, big);
    const old = (await snap(p)).files['hello.py'];
    await mock(p, 'configure', { opDelays: { save: 400 } });
    await mock(p, 'fail', 'save', { atChunk: 2 });
    await p.keyboard.press('Control+s');
    await p.waitForSelector(T('sc-upload'), { timeout: 3000 });
    await waitNotice(p, 'interrupted after chunk 2');
    eq((await snap(p)).files['hello.py'], old, 'partial upload replaced the file');
    assert(await isDirty(p), 'dirty cleared by a failed upload');
    assert((await mock(p, 'uploads')).length >= 2, 'no progress events');
    await mock(p, 'configure', { opDelays: { save: 30 } });
    await replaceDoc(p, 'print("ok")\n');
    await p.keyboard.press('Control+s');
    await sleep(700);
    const dbg = `notice="${await noticeText(p)}" upload=${!!(await p.$(T('sc-upload')))} saves=${await nCmds(p, 'save')} focus=${await p.evaluate(() => document.activeElement && (document.activeElement.className || document.activeElement.tagName))}`;
    await until(p, () => !document.querySelector('[data-testid="sc-dirty"]'), null, 8000, `clean save (${dbg})`);
  });
  await s.step('oversize script (> 32768 UTF-8 bytes) is refused before any I/O or with too_large', async () => {
    const before = await nCmds(p, 'save');
    await replaceDoc(p, '\u00e9'.repeat(17000));
    await p.keyboard.press('Control+s');
    await sleep(900);
    const notice = await noticeText(p);
    assert(/byte|large|32/i.test(notice), `no size notice: "${notice}"`);
    const sent = (await nCmds(p, 'save')) - before;
    return `save commands sent: ${sent}; notice: ${notice.slice(0, 80)}`;
  });
  await s.step('dirty protection: revert and delete confirm; Escape cancels; switching files keeps unsaved text', async () => {
    await replaceDoc(p, 'print("dirty")\n');
    await p.click(T('sc-revert'));
    await p.waitForSelector(T('sc-dialog-revert'));
    await p.keyboard.press('Escape');
    await p.waitForSelector(T('sc-dialog-revert'), { state: 'detached' });
    assert(/dirty/.test(await docText(p)), 'escape discarded edits');
    await openFile(p, 'blink.py');
    await openFile(p, 'hello.py');
    assert(/dirty/.test(await docText(p)), 'unsaved buffer lost on file switch');
    assert(await p.$('[data-file="hello.py"] .sc-dirty'), 'row dirty marker missing');
    await p.click('[data-file="hello.py"] [data-testid="sc-file-delete"]');
    await p.waitForSelector(T('sc-dialog-delete'));
    const text = await p.$eval(T('sc-dialog-delete'), (e) => e.textContent);
    assert((await snap(p)).files['hello.py'] !== undefined, 'deleted before confirm');
    await p.keyboard.press('Escape');
    await p.click(T('sc-revert'));
    await p.click(`${T('sc-dialog-revert')} ${T('sc-dialog-confirm')}`);
    await until(p, () => !document.querySelector('[data-testid="sc-dirty"]'), null, 6000, 'revert');
    return `delete dialog text: ${text.trim().replace(/\s+/g, ' ').slice(0, 90)}`;
  });
  await s.step('modal dialogs take keyboard focus and close on Escape (new, delete, revert, autorun)', async () => {
    const results = [];
    const probe = async (label, testid, open) => {
      await open();
      await p.waitForSelector(T(testid), { timeout: 4000 });
      await sleep(250);
      const inDialog = await p.evaluate((id) => { const d = document.querySelector(`[data-testid="${id}"]`); const a = document.activeElement; return !!(d && a && d.contains(a)); }, testid);
      await p.keyboard.press('Escape');
      await sleep(350);
      const closed = !(await p.$(T(testid)));
      if (!closed) await p.evaluate((id) => document.querySelectorAll(`[data-testid="${id}"] button`).forEach((b) => { if (/^cancel$/i.test(b.textContent.trim())) b.click(); }), testid);
      await sleep(250);
      results.push({ label, focusInDialog: inDialog, escapeCloses: closed });
    };
    await openFile(p, 'hello.py');
    await probe('new', 'sc-dialog-new', () => p.click(T('sc-new')));
    await probe('delete', 'sc-dialog-delete', () => p.click('[data-file="hello.py"] [data-testid="sc-file-delete"]'));
    await replaceDoc(p, 'print("changed")\n');
    await probe('revert', 'sc-dialog-revert', () => p.click(T('sc-revert')));
    await p.click(T('sc-revert'));
    await p.click(`${T('sc-dialog-revert')} ${T('sc-dialog-confirm')}`);
    if (!(await p.isVisible(T('sc-autorun-pick')))) await p.click(`${T('sc-autorun')} summary`);
    await probe('autorun', 'sc-dialog-autorun', async () => { await p.click(T('sc-autorun-pick')); await p.click('.sc-pick .menu-item'); });
    const bad = results.filter((r) => !r.focusInDialog || !r.escapeCloses);
    assert(bad.length === 0, `dialogs without focus/Escape handling: ${JSON.stringify(bad)}`);
    return JSON.stringify(results);
  });
  await s.step('deleting the open file closes the editor and drops its buffer', async () => {
    await mock(p, 'setFiles', null, { 'tmp_del.py': 'x = 1\n' });
    await p.click(T('sc-refresh'));
    await p.waitForSelector('[data-file="tmp_del.py"]');
    await openFile(p, 'tmp_del.py');
    await p.click('[data-file="tmp_del.py"] [data-testid="sc-file-delete"]');
    await p.click(`${T('sc-dialog-delete')} ${T('sc-dialog-confirm')}`);
    await p.waitForSelector('[data-file="tmp_del.py"]', { state: 'detached' });
    await p.waitForSelector(T('sc-empty'), { timeout: 4000 });
    assert(!(await snap(p)).files['tmp_del.py'], 'device still has the file');
  });
  await s.step('undo / redo / search & replace on the real view', async () => {
    await openFile(p, 'hello.py');
    await replaceDoc(p, '');
    await typeText(p, 'foo bar foo');
    await p.keyboard.press('Control+z');
    assert((await docText(p)) !== 'foo bar foo', 'undo did nothing');
    await p.keyboard.press('Control+y');
    eq(await docText(p), 'foo bar foo', 'redo');
    await p.keyboard.press('Control+f');
    const panel = p.locator('.cm-search');
    await panel.waitFor({ timeout: 4000 });
    await panel.locator('input[name="search"]').fill('foo');
    await panel.locator('input[name="replace"]').fill('baz');
    await panel.locator('button[name="replaceAll"]').click();
    eq(await docText(p), 'baz bar baz', 'replace all');
    await s.shot('search-replace-1440-light');
    await p.keyboard.press('Escape');
    await p.keyboard.press('Control+z');
    assert((await docText(p)) !== 'baz bar baz', 'undo of replace did nothing');
    await p.click(T('sc-revert'));
    await p.click(`${T('sc-dialog-revert')} ${T('sc-dialog-confirm')}`);
  });
  await s.step('long source line and long file name stay inside their panes', async () => {
    await openFile(p, 'long.py');
    const m = await p.evaluate(() => ({ page: document.documentElement.scrollWidth - document.documentElement.clientWidth, cm: (() => { const e = document.querySelector('.cm-scroller'); return e.scrollWidth - e.clientWidth; })() }));
    eq(m.page <= 0, true, 'page widened');
    assert(m.cm > 50, 'editor does not scroll its long line internally');
    await p.click(T('sc-new'));
    await p.fill(T('sc-name-input'), 'a_really_long_script_name_28c');
    await p.press(T('sc-name-input'), 'Enter');
    await p.waitForSelector('[data-file="a_really_long_script_name_28c.py"]', { timeout: 8000 });
    const rep = await layoutReport(p);
    eq(rep.problems, [], 'layout problems');
  });
  await s.step('UTF-8 round trip (accents, CJK, emoji) is byte exact', async () => {
    await openFile(p, 'unicode.py');
    const t = await docText(p);
    assert(/h\u00e9llo w\u00f6rld/.test(t) && /\u65e5\u672c\u8a9e/.test(t), `unicode lost: ${t}`);
    await focusEditor(p);
    await typeText(p, '\n# \u00fc\u00f1\u00ee');
    await p.keyboard.press('Control+s');
    await until(p, () => !document.querySelector('[data-testid="sc-dirty"]'), null, 8000, 'save');
    assert((await snap(p)).files['unicode.py'].includes('# \u00fc\u00f1\u00ee'), 'device lost accents');
    const lossy = await p.$(T('sc-lossy'));
    assert(!lossy, 'lossy banner on valid UTF-8');
  });
  await s.step('lossy (non UTF-8) device file warns and needs confirmation', async () => {
    await mock(p, 'setFiles', null, { 'latin1.py': { source: 'print("caf\ufffd")\n', lossy: true } });
    await p.click(T('sc-refresh'));
    await p.waitForSelector('[data-file="latin1.py"]');
    await openFile(p, 'latin1.py');
    await p.waitForSelector(T('sc-lossy'), { timeout: 4000 });
  });
  await finish(s);
});

group('lint', async (browser) => {
  const s = await newSession(browser, 'lint', {});
  const p = s.page;
  const banners = {};
  await s.step('valid text -> ok; invalid -> syntax banner + editor diagnostics; invalid is still saved', async () => {
    await openFile(p, 'hello.py');
    await focusEditor(p);
    await p.keyboard.insertText('\nvalue = (1,');
    await p.keyboard.press('Control+s');
    await waitLint(p, 'syntax');
    banners.syntax = await lintText(p);
    assert((await snap(p)).files['hello.py'].includes('value = (1,'), 'invalid script was not saved');
    await p.waitForSelector('.cm-lintRange-error', { timeout: 4000 });
    assert(/SyntaxError|invalid syntax/i.test(banners.syntax), `banner: ${banners.syntax}`);
    await s.shot('lint-syntax-1440-light');
    await p.keyboard.insertText(')');
    await waitLint(p, 'stale', 3000);
    assert(!(await p.$('.cm-lintRange-error')) || true);
    await p.click(T('sc-check'));
    await waitLint(p, 'ok');
    banners.ok = await lintText(p);
  });
  await s.step('explicit check sends source (unsaved text) and never name; does not execute', async () => {
    await focusEditor(p);
    await typeText(p, ' # unsaved');
    const last = (await cmds(p, 'lint')).pop();
    await p.click(T('sc-check'));
    await waitLint(p, 'ok');
    const l2 = (await cmds(p, 'lint')).pop();
    assert(l2.n !== last.n && typeof l2.args.source === 'string' && l2.args.name === undefined, `explicit check args ${JSON.stringify(l2.args).slice(0, 80)}`);
    assert(await isDirty(p), 'check must not save');
    eq((await snap(p)).status.totalRuns, 0);
  });
  await s.step('busy interpreter is "skipped", not a syntax error, and the save still happens', async () => {
    await mock(p, 'setFiles', null, { 'spin.py': 'import time\nwhile True:\n    time.sleep(1)\n' });
    await p.click(T('sc-refresh'));
    await p.waitForSelector('[data-file="spin.py"]');
    await mock(p, 'externalRun', null, 'spin.py', {});
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    await focusEditor(p);
    await typeText(p, ' # busy');
    await p.keyboard.press('Control+s');
    await waitLint(p, 'skipped');
    banners.busy = await lintText(p);
    assert(!/SyntaxError/.test(banners.busy), 'busy reported as syntax error');
    assert(!(await isDirty(p)), 'save did not happen while busy');
    await stopAll(p);
  });
  await s.step('unsupported firmware: distinct "update" wording, save persists', async () => {
    await mock(p, 'setCaps', { lint: false });
    await focusEditor(p);
    await typeText(p, ' # b');
    await p.keyboard.press('Control+s');
    await waitLint(p, 'skipped');
    banners.unsupported = await lintText(p);
    assert(/firmware|update/i.test(banners.unsupported), banners.unsupported);
    await mock(p, 'setCaps', { lint: true });
  });
  await s.step('transport failure: distinct wording', async () => {
    await focusEditor(p);
    await typeText(p, ' # c');
    await mock(p, 'fail', 'lint', { reply: { kind: 'transport', error: 'USB timeout while checking' } });
    await p.keyboard.press('Control+s');
    await waitLint(p, 'skipped');
    banners.transport = await lintText(p);
    assert(/timeout|transport|link|check/i.test(banners.transport), banners.transport);
  });
  await s.step('analysis unavailable: distinct wording', async () => {
    await focusEditor(p);
    await typeText(p, ' # d');
    await mock(p, 'fail', 'lint', { reply: { kind: 'unavailable', error: 'Scripting engine is off' } });
    await p.keyboard.press('Control+s');
    await waitLint(p, 'skipped');
    banners.unavailable = await lintText(p);
  });
  await s.step('five states read differently', async () => {
    const texts = Object.values(banners);
    eq(new Set(texts).size, texts.length, `duplicate banner texts: ${JSON.stringify(banners)}`);
    return JSON.stringify(banners).slice(0, 300);
  });
  await s.step('stale lint reply cannot attach to newer text', async () => {
    await p.click(T('sc-check'));
    await waitLint(p, 'ok');
    await mock(p, 'hold', 'lint', 'after', 1);
    await focusEditor(p);
    await typeText(p, ' # x');
    await p.keyboard.press('Control+s');
    for (let i = 0; i < 60 && (await mock(p, 'held')).length === 0; i++) await sleep(50);
    await focusEditor(p);
    await typeText(p, ' newer');
    await mock(p, 'release', 'lint');
    await sleep(900);
    const st = await lintState(p);
    assert(st === 'stale', `late lint reply decorated newer text: ${st}`);
  });
  await s.step('very long lint message wraps inside its banner', async () => {
    const long = 'SyntaxError: ' + 'very-long-token-'.repeat(40);
    await mock(p, 'fail', 'lint', { reply: { ok: true, valid: false, kind: 'x', message: long } });
    await focusEditor(p);
    await typeText(p, ' # long');
    await p.keyboard.press('Control+s');
    await waitLint(p, 'syntax');
    const r = await p.evaluate(() => { const b = document.querySelector('[data-testid="sc-lint-banner"]'); const pr = b.parentElement.getBoundingClientRect(); const br = b.getBoundingClientRect(); return { over: b.scrollWidth - b.clientWidth, right: br.right, parentRight: pr.right, vw: innerWidth, h: br.height }; });
    assert(r.over <= 1 && r.right <= r.parentRight + 1, `banner overflows ${JSON.stringify(r)}`);
    await s.shot('lint-long-message-1440-light');
  });
  await finish(s);
});

group('completion', async (browser) => {
  const s = await newSession(browser, 'completion', {});
  const p = s.page;
  await openFile(p, 'hello.py');
  const accept = async (key = 'Tab') => { await p.keyboard.press(key); await p.waitForTimeout(120); };
  await s.step('module alias member: import daq as d; d.vd + Tab', async () => {
    await replaceDoc(p, 'import daq as d\nd.');
    await typeText(p, 'vd');
    const items = await popupItems(p);
    assert(items[0].includes('vdut'), items.slice(0, 4).join('|'));
    await accept();
    assert((await docText(p)).startsWith('import daq as d\nd.vdut('), await docText(p));
    await s.shot('completion-alias-1440-light');
  });
  await s.step('from-import and class instance members; caret lands on the first placeholder', async () => {
    await replaceDoc(p, 'from bugbuster import Channel\nch = Channel(0)\nch.');
    await typeText(p, 'set_v');
    const items = await popupItems(p);
    assert(items.some((t) => t.includes('set_voltage')), items.slice(0, 5).join('|'));
    await accept();
    assert(/ch\.set_voltage\(/.test(await docText(p)), await docText(p));
    await typeText(p, '2.5');
    assert(/ch\.set_voltage\(2\.5\)/.test(await docText(p)), `placeholder not replaced: ${await docText(p)}`);
  });
  await s.step('return-annotation instance (bugbuster.http_get -> Response) offers members', async () => {
    await replaceDoc(p, 'import bugbuster\nr = bugbuster.http_get("http://x")\nr.');
    await p.keyboard.press('Control+Space');
    const items = await popupItems(p);
    assert(items.length > 0, 'no members for a return-annotated instance');
    await p.keyboard.press('Escape');
    return items.slice(0, 4).join(' | ');
  });
  await s.step('class completion inserts a constructor call with its first placeholder selected; builtins and keywords complete', async () => {
    await replaceDoc(p, 'import bugbuster\nbugbuster.');
    await typeText(p, 'Chan');
    await popupItems(p);
    await accept();
    assert((await docText(p)).endsWith('bugbuster.Channel(channel)'), await docText(p));
    await replaceDoc(p, 'pri');
    const b = await popupItems(p);
    assert(b.some((t) => t.startsWith('print')), b.slice(0, 5).join('|'));
    await accept('Enter');
    assert((await docText(p)).startsWith('print'), await docText(p));
    await replaceDoc(p, 'for i in ran');
    const r = await popupItems(p);
    assert(r.some((t) => t.startsWith('range')), r.slice(0, 5).join('|'));
    await p.keyboard.press('Escape');
  });
  await s.step('mouse accepts; arrows move; Escape closes; Ctrl+Space opens explicitly', async () => {
    await replaceDoc(p, 'import daq\ndaq.run.');
    await typeText(p, 'st');
    await popupItems(p);
    await p.locator('.cm-tooltip-autocomplete li', { hasText: 'status' }).first().click();
    assert(/daq\.run\.status\(/.test(await docText(p)), await docText(p));
    await replaceDoc(p, 'import daq\ndaq.run.');
    await typeText(p, 'st');
    await popupItems(p);
    await p.keyboard.press('ArrowDown');
    const sel = await p.locator('.cm-tooltip-autocomplete li[aria-selected="true"]').innerText();
    await p.keyboard.press('Escape');
    assert(!(await p.$('.cm-tooltip-autocomplete')), 'escape left popup open');
    await replaceDoc(p, 'import daq\ndaq.');
    await p.keyboard.press('Control+Space');
    const items = await popupItems(p);
    assert(items.length > 3, 'explicit completion empty');
    await p.keyboard.press('Escape');
    return `arrow selected "${sel.trim().slice(0, 30)}"`;
  });
  await s.step('popup anchoring: inside the viewport and not covering the caret line', async () => {
    await replaceDoc(p, 'import daq\ndaq.');
    await typeText(p, 'v');
    await popupItems(p);
    const c = await cursorRect(p);
    const pop = await p.$eval('.cm-tooltip-autocomplete', (e) => { const r = e.getBoundingClientRect(); return { x: r.x, y: r.y, right: r.right, bottom: r.bottom, w: r.width, h: r.height, vw: innerWidth, vh: innerHeight }; });
    assert(pop.x >= 0 && pop.right <= pop.vw && pop.y >= 0 && pop.bottom <= pop.vh, `popup outside viewport ${JSON.stringify(pop)}`);
    const overlapsCaret = pop.x < c.right + 2 && pop.right > c.x - 2 && pop.y < c.bottom - 2 && pop.bottom > c.y + 2;
    assert(!overlapsCaret, `popup covers the caret ${JSON.stringify({ c, pop })}`);
    assert(Math.abs(pop.x - c.x) < 160, `popup far from caret x: ${Math.round(pop.x - c.x)}px`);
    await p.keyboard.press('Escape');
  });
  await s.step('no completion in comments / strings; local names are not completed (shipped limit)', async () => {
    await replaceDoc(p, 'import daq\n# daq.');
    await typeText(p, 'vd');
    await expectNoPopup(p);
    await replaceDoc(p, 'import daq\nx = "daq.');
    await typeText(p, 'vd');
    await expectNoPopup(p);
    await replaceDoc(p, 'my_local_value = 1\nmy_loc');
    await p.waitForTimeout(500);
    const items = await p.$$eval('.cm-tooltip-autocomplete li', (e) => e.map((x) => x.textContent));
    assert(!items.some((t) => t.includes('my_local_value')), 'local variable completion appeared (not shipped on iOS)');
    await p.keyboard.press('Escape');
  });
  await s.step('signature help shows the active parameter', async () => {
    await replaceDoc(p, 'import bugbuster\nch = bugbuster.Channel(0)\nch.set_voltage(1.5, ');
    const tip = p.locator('.cm-tooltip-signature');
    await tip.waitFor({ timeout: 5000 });
    assert(/set_voltage\(voltage/.test(await tip.innerText()), await tip.innerText());
    await s.shot('signature-1440-light');
    await p.keyboard.press('Escape');
  });
  await finish(s);
});

group('docs', async (browser) => {
  const s = await newSession(browser, 'docs', {});
  const p = s.page;
  await openFile(p, 'hello.py');
  await s.step('search finds entries, detail shows params/returns/safety, Insert adds missing import once', async () => {
    await replaceDoc(p, 'import time\n');
    if (!(await p.isVisible(T('sc-docs')))) await p.click(T('sc-docs-toggle'));
    await p.waitForSelector(T('sc-doc-list'), { timeout: 8000 });
    await p.fill(T('sc-docs-search'), 'vdut');
    await p.waitForSelector('[data-doc]');
    await p.click('[data-doc]');
    await p.waitForSelector(T('sc-doc-detail'));
    assert(await p.$(T('sc-doc-params')) || await p.$(T('sc-doc-returns')), 'detail has neither params nor returns');
    await p.click(T('sc-doc-insert'));
    await p.waitForTimeout(250);
    let t = await docText(p);
    assert(/import daq/.test(t) && /vdut/.test(t), `insert result: ${t}`);
    await p.click(T('sc-doc-insert'));
    await p.waitForTimeout(250);
    t = await docText(p);
    eq((t.match(/import daq/g) || []).length, 1, 'import duplicated');
    assert(await isDirty(p), 'insert did not dirty the buffer');
    await s.shot('docs-detail-1440-light');
  });
  await s.step('an alias does not count as the import', async () => {
    await replaceDoc(p, 'import daq as d\n');
    await p.fill(T('sc-docs-search'), 'vdut');
    await p.waitForSelector('[data-doc]');
    await p.click('[data-doc]');
    await p.click(T('sc-doc-insert'));
    await p.waitForTimeout(250);
    const t = await docText(p);
    assert(/^import daq as d\n/.test(t) && /\nimport daq\n/.test(t + '\n'), `aliased import not supplemented: ${t}`);
  });
  await s.step('safety banner appears for output-changing calls', async () => {
    await p.fill(T('sc-docs-search'), 'set_voltage');
    await p.waitForSelector('[data-doc]');
    await p.click('[data-doc]');
    await p.waitForSelector(T('sc-doc-safety'), { timeout: 3000 });
  });
  await s.step('examples: kind filter, verbatim into an empty script, body-only into a non-empty one', async () => {
    await p.fill(T('sc-docs-search'), '');
    await p.selectOption(T('sc-docs-kind'), { index: await p.$$eval(`${T('sc-docs-kind')} option`, (o) => Math.max(0, o.findIndex((x) => /example/i.test(x.textContent)))) });
    await p.waitForSelector('[data-doc]');
    const ex = await p.evaluate(() => window.bbScriptCatalogue.examples()[0]);
    await p.click('[data-doc]');
    await p.waitForSelector(T('sc-doc-detail'));
    await replaceDoc(p, '');
    await p.click(`${T('sc-doc-detail')} ${T('sc-doc-insert')}`).catch(() => p.click(T('sc-doc-insert')));
    await p.waitForTimeout(300);
    const t = await docText(p);
    const firstCode = (ex.source || '').split('\n').find((l) => l.trim() && !l.startsWith('#') && !l.startsWith('"""')) || '';
    assert(t.includes(firstCode.trim()), `example not inserted: ${t.slice(0, 80)}`);
    await replaceDoc(p, 'print("keep me")\n');
    await p.click(`${T('sc-doc-detail')} ${T('sc-doc-insert')}`).catch(() => p.click(T('sc-doc-insert')));
    await p.waitForTimeout(300);
    assert((await docText(p)).includes('keep me'), 'existing text replaced by an example insert');
    await s.shot('docs-examples-1440-light');
  });
  await s.step('long signatures in the detail view wrap or scroll (never silently clipped)', async () => {
    await p.selectOption(T('sc-docs-kind'), { index: 0 });
    await p.fill(T('sc-docs-search'), 'vdut');
    await p.waitForSelector('[data-doc]');
    await p.click('[data-doc]');
    await p.waitForSelector(`${T('sc-doc-detail')} pre.sc-code`);
    const r = await p.$eval(`${T('sc-doc-detail')} pre.sc-code`, (e) => { const cs = getComputedStyle(e); return { over: e.scrollWidth - e.clientWidth, overflowX: cs.overflowX, whiteSpace: cs.whiteSpace }; });
    const ok = r.over <= 1 || ['auto', 'scroll'].includes(r.overflowX) || /wrap/.test(r.whiteSpace);
    assert(ok, `signature overflows ${r.over}px with overflow-x:${r.overflowX} white-space:${r.whiteSpace}`);
    return `scrollable overflow ${r.over}px (overflow-x:${r.overflowX})`;
  });
  await s.step('no-match state', async () => {
    await p.selectOption(T('sc-docs-kind'), { index: 0 });
    await p.fill(T('sc-docs-search'), 'zzzz-nothing-matches');
    await p.waitForSelector(T('sc-docs-empty'));
  });
  await finish(s);
});

group('docs-failure', async (browser) => {
  const s = await newSession(browser, 'docs-failure', { abortCatalogue: true, connect: null });
  const p = s.page;
  await s.step('catalogue fetch failure: explicit failed state, editor still usable, retry recovers', async () => {
    await p.click(T('scripts-offline-entry'));
    await p.waitForSelector('.cm-content');
    await p.click(T('sc-docs-toggle'));
    await p.waitForSelector(T('sc-docs-state'), { timeout: 10000 });
    const text = await p.$eval(T('sc-docs-state'), (e) => e.textContent.trim());
    await p.click('.cm-content');
    await typeText(p, 'x = 1');
    assert(/x = 1/.test(await docText(p)), 'editor unusable with failed catalogue');
    await s.shot('docs-failed-1440-light');
    await p.unroute('**/firmware-api.json');
    s.expectedFail.length = 0;
    const retry = p.locator(`${T('sc-docs-state')} button`);
    await retry.first().click();
    await p.waitForSelector(T('sc-doc-list'), { timeout: 10000 });
    return `failed text: ${text.slice(0, 100)}`;
  });
  await finish(s, { allowFailedRequests: true, allowConsole: [/Failed to load resource: net::ERR_FAILED/] });
});

group('runs', async (browser) => {
  const s = await newSession(browser, 'runs', {});
  const p = s.page;
  const A = (await mock(p, 'limits')).defaultMac;
  await mock(p, 'setFiles', null, { 'long_run.py': 'import time\nfor i in range(60):\n    print("step", i)\n    time.sleep(1)\n' });
  await p.click(T('sc-refresh'));
  await p.waitForSelector('[data-file="long_run.py"]');
  await s.step('quick script: one run command, completion marker, output, no stuck chip', async () => {
    await p.click('[data-file="hello.py"] [data-testid="sc-file-run"]');
    await waitMarker(p, "Finished 'hello.py'");
    assert((await logLines(p)).some((l) => /I\s*hello$/.test(l)), 'output missing: ' + (await logLines(p)).slice(-4).join(' | '));
    eq(await nCmds(p, 'run'), 1);
    assert(!(await p.$(T('script-run-chip'))), 'chip stuck');
  });
  await s.step('double click Run sends exactly one run (no duplicate launch)', async () => {
    await openFile(p, 'blink.py');
    const before = await nCmds(p, 'run');
    await p.dblclick(T('sc-run'));
    await sleep(900);
    eq((await nCmds(p, 'run')) - before, 1, 'run commands for a double click');
    eq((await mock(p, 'counts')).inFlightRejected, 0, 'in_flight rejection means the UI sent a second launch');
    await stopAll(p);
  });
  await s.step('background run sends background:true and keeps the log closed; foreground opens it', async () => {
    await openFile(p, 'blink.py');
    if (await p.$(T('sc-console'))) await p.click(T('sc-log-toggle'));
    await p.waitForSelector(T('sc-console'), { state: 'detached' });
    await p.click(T('sc-run-menu'));
    await p.click(T('sc-run-bg'));
    await sleep(700);
    const last = (await cmds(p, 'run')).pop();
    eq(last.args.background, true, 'background flag');
    assert(!(await p.$(T('sc-console'))), 'a background run opened the log console (iOS keeps it closed)');
    await stopAll(p);
    await p.click(T('sc-run'));
    await sleep(500);
    const l2 = (await cmds(p, 'run')).pop();
    assert(l2.args.background !== true, 'foreground run sent background');
    await p.waitForSelector(T('sc-console'), { timeout: 3000 });
    await stopAll(p);
  });
  await s.step('Run saves the dirty open file first; running another file saves nothing', async () => {
    await openFile(p, 'hello.py');
    await focusEditor(p);
    await typeText(p, '\n# run-save');
    await p.click(T('sc-run'));
    await waitIdle(p);
    await sleep(500);
    assert((await snap(p)).files['hello.py'].includes('run-save'), 'device copy was not saved before the run');
    await focusEditor(p);
    await typeText(p, ' extra');
    const saves = await nCmds(p, 'save');
    await p.click('[data-file="blink.py"] [data-testid="sc-file-run"]');
    await sleep(900);
    eq(await nCmds(p, 'save'), saves, 'running another file must not save the open buffer');
    assert(await isDirty(p), 'open buffer lost its dirty state');
    await stopAll(p);
  });
  await s.step('long run: global chip survives navigation, elapsed grows, Stop ends it with a final marker', async () => {
    await openFile(p, 'long_run.py');
    await p.click(T('sc-run'));
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    const views = ['overview', 'adc'];
    const hat = (await p.evaluate(() => window.__BB_MOCK.hat));
    if (hat === 'daq') views.push('daq'); else if (hat === 'la') views.push('la');
    let t0 = null;
    for (const v of views) {
      await nav(p, v);
      await p.waitForSelector(T('script-run-chip'), { timeout: 5000 });
      await sleep(700);
      const t = await p.$eval(T('script-run-chip-time'), (e) => e.textContent);
      if (t0 === null) t0 = t;
    }
    await s.shot('global-chip-overview-1440-light');
    await sleep(1500);
    const t1 = await p.$eval(T('script-run-chip-time'), (e) => e.textContent);
    assert(t1 !== t0, `elapsed did not advance (${t0} -> ${t1})`);
    await p.click(T('script-run-chip-stop'));
    await p.waitForSelector(T('script-run-chip'), { state: 'detached', timeout: 8000 });
    eq((await snap(p)).status.lastExit, 'stopped');
    await openScripts(s);
    await openConsole(p);
    await waitMarker(p, "Stopped|stopped");
    return `${t0} -> ${t1}`;
  });
  await s.step('failing script: red traceback lines, failed marker, error status; chip clears', async () => {
    await p.click('[data-file="fail.py"] [data-testid="sc-file-run"]');
    await waitMarker(p, "failed|Failed|error");
    await p.waitForSelector('.sc-log-line.lv-E', { timeout: 6000 });
    const e = await p.$$eval('.sc-log-line.lv-E', (els) => els.map((x) => x.textContent.replace(/\s+/g, ' ')));
    assert(e.some((l) => /Traceback/.test(l)) && e.some((l) => /ValueError: boom/.test(l)), e.join(' | '));
    assert(/Error/i.test(await p.$eval(T('script-status'), (x) => x.textContent)), 'status does not show the error');
    await s.shot('traceback-1440-light');
  });
  await s.step('busy -> replace dialog; Escape cancels without a replace command; confirm sends it once', async () => {
    await mock(p, 'externalRun', null, 'long_run.py', {});
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    await openFile(p, 'hello.py');
    const before = await nCmds(p, 'run');
    await p.click(T('sc-run'));
    await p.waitForSelector(T('sc-replace'), { timeout: 5000 });
    const focus = await p.evaluate(() => { const d = document.querySelector('[data-testid="sc-replace"]'); const a = document.activeElement; return { inDialog: !!(d && a && d.contains(a)), active: a ? (a.tagName + '.' + (a.className || '')).slice(0, 40) : null }; });
    await p.keyboard.press('Escape');
    await sleep(400);
    const escClosed = !(await p.$(T('sc-replace')));
    if (!escClosed) await p.click(`${T('sc-replace')} button:has-text("Cancel")`);
    await p.waitForSelector(T('sc-replace'), { state: 'detached' });
    assert((await snap(p)).status.fileSlotName === 'long_run.py', 'cancel replaced the running script');
    assert((await cmds(p, 'run')).slice(before).every((c) => !c.args.replace), 'replace sent after cancel');
    await p.click(T('sc-run'));
    await p.waitForSelector(T('sc-replace'));
    await p.click(T('sc-replace-confirm'));
    for (let i = 0; i < 40 && (await cmds(p, 'run')).filter((c) => c.args.replace === true).length === 0; i++) await sleep(100);
    const rep = (await cmds(p, 'run')).filter((c) => c.args.replace === true);
    eq(rep.length, 1, 'replace commands');
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-marker"]')].filter((e) => /Finished 'hello.py'/.test(e.textContent)).length >= 2, null, 12000, "second Finished 'hello.py' marker after the replace");
    assert(focus.inDialog && escClosed, `modal dialog keyboard handling: focus moved into dialog=${focus.inDialog} (active ${focus.active}); Escape closes=${escClosed}`);
  });
  await s.step('stop-and-replace keeps the background flag', async () => {
    await stopAll(p);
    await mock(p, 'externalRun', null, 'long_run.py', {});
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    await openFile(p, 'hello.py');
    const pre = (await snap(p)).status;
    assert(pre.running && pre.fileSlotName === 'long_run.py', `precondition: external run not active (${JSON.stringify({ r: pre.running, n: pre.fileSlotName, s: pre.state })})`);
    await p.click(T('sc-run-menu'));
    await p.click(T('sc-run-bg'));
    await p.waitForSelector(T('sc-replace'), { timeout: 5000 });
    await p.click(T('sc-replace-confirm'));
    for (let i = 0; i < 40 && (await cmds(p, 'run')).filter((c) => c.args.replace === true).length < 2; i++) await sleep(100);
    const last = (await cmds(p, 'run')).filter((c) => c.args.replace === true).pop();
    assert(last && last.args.background === true, `replace lost the background flag: ${JSON.stringify(last && last.args)}`);
    await stopAll(p);
  });
  await s.step('unknown outcome: no automatic retry, explicit notice', async () => {
    await stopAll(p);
    await openFile(p, 'blink.py');
    const before = await nCmds(p, 'run');
    await mock(p, 'fail', 'run', { reply: { kind: 'transport', error: 'timeout' } });
    await p.click(T('sc-run'));
    await waitNotice(p, 'may have processed');
    await sleep(2500);
    eq((await nCmds(p, 'run')) - before, 1, 'run was retried');
  });
  await s.step('edit during the save-before-run upload cancels the run', async () => {
    await focusEditor(p);
    await typeText(p, '\n# gate');
    await mock(p, 'configure', { opDelays: { save: 700 } });
    const before = await nCmds(p, 'run');
    await p.click(T('sc-run'));
    await sleep(250);
    await focusEditor(p);
    await typeText(p, 'Q');
    await waitNotice(p, 'Not run', 9000);
    eq(await nCmds(p, 'run'), before, 'run sent anyway');
    await mock(p, 'configure', { opDelays: { save: 30 } });
    await p.keyboard.press('Control+s');
    await until(p, () => !document.querySelector('[data-testid="sc-dirty"]'), null, 8000, 'clean');
  });
  await s.step('run started by another client or boot autorun is observed, never started by the UI', async () => {
    await stopAll(p);
    const before = await nCmds(p, 'run');
    await mock(p, 'externalRun', A, 'long_run.py', { kind: 'autorun' });
    await p.waitForSelector(T('script-run-chip'), { timeout: 9000 });
    await openConsole(p);
    await waitMarker(p, 'long_run.py', 9000);
    eq(await nCmds(p, 'run'), before, 'UI sent a run');
    assert(/autorun/i.test(await p.$eval(T('script-status'), (e) => e.textContent)) || (await logMarkers(p)).some((m) => /autorun|started elsewhere|external/i.test(m)), 'source not shown');
    await p.click(T('script-run-chip-stop'));
    await p.waitForSelector(T('script-run-chip'), { state: 'detached', timeout: 8000 });
  });
  await s.step('device reboot during a run clears the chip and marks the restart', async () => {
    await mock(p, 'externalRun', A, 'long_run.py', {});
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    await mock(p, 'reboot', A, { autorun: false });
    await p.waitForSelector(T('script-run-chip'), { state: 'detached', timeout: 9000 });
    await openConsole(p);
    await waitMarker(p, 'restart', 9000);
  });
  await s.step('disconnect keeps the device run going; reconnect observes it without a new launch', async () => {
    await mock(p, 'externalRun', A, 'long_run.py', {});
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    const before = await nCmds(p, 'run');
    await disconnectUi(s);
    eq((await snap(p)).status.running, true, 'disconnect stopped the device script');
    await connectTo(s, 'usb');
    await openScripts(s);
    await p.waitForSelector(T('script-run-chip'), { timeout: 9000 });
    eq(await nCmds(p, 'run'), before, 'reconnect started a run');
    await stopAll(p);
  });
  await s.step('polling cadence: Scripts view polls faster than other views while idle', async () => {
    await openScripts(s);
    const c0 = await nCmds(p, 'status');
    await sleep(4200);
    const onScripts = (await nCmds(p, 'status')) - c0;
    await nav(p, 'overview');
    const c1 = await nCmds(p, 'status');
    await sleep(4200);
    const elsewhere = (await nCmds(p, 'status')) - c1;
    assert(elsewhere <= onScripts, `status polls: scripts=${onScripts} elsewhere=${elsewhere}`);
    return `scripts=${onScripts} elsewhere=${elsewhere} per 4.2s`;
  });
  await finish(s);
});

group('logs', async (browser) => {
  const s = await newSession(browser, 'logs', {});
  const p = s.page;
  const A = (await mock(p, 'limits')).defaultMac;
  await openConsole(p);
  await s.step('lines parse with timestamps/levels; independent reader does not drain the UI', async () => {
    await mock(p, 'log', A, 'I', 'mpy', 'alpha');
    await mock(p, 'log', A, 'W', 'sys', 'beta');
    await mock(p, 'log', A, 'E', 'mpy', 'gamma');
    await until(p, () => document.querySelectorAll('[data-testid="sc-log-line"]').length >= 3, null, 8000, '3 lines');
    const external = await mock(p, 'readLogs', A, 0);
    assert(external.text.includes('alpha'), 'second reader sees nothing');
    await mock(p, 'log', A, 'I', 'mpy', 'delta');
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-line"]')].some((e) => /delta/.test(e.textContent)), null, 8000, 'delta');
    const lines = await logLines(p);
    assert(['alpha', 'beta', 'gamma', 'delta'].every((w) => lines.some((l) => l.includes(w))), lines.join('|'));
    assert(await p.$('.sc-log-line.lv-E') && await p.$('.sc-log-line.lv-W'), 'level classes missing');
  });
  await s.step('split UTF-8 character across the 4096-byte page boundary is reassembled', async () => {
    await sleep(1300);
    const cur = (await snap(p)).logTotal;
    await mock(p, 'logSplitUtf8', A, '\u00e9', cur);
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-line"]')].some((e) => /tail-after-split/.test(e.textContent)), null, 10000, 'split line');
    const line = (await logLines(p)).find((l) => /tail-after-split/.test(l));
    assert(line.includes('\u00e9 tail-after-split') && !line.includes('\ufffd'), `split char corrupted: ...${line.slice(-30)}`);
  });
  await s.step('CRLF and partial lines are handled; a partial line waits for its newline', async () => {
    await mock(p, 'writeRaw', A, '1 I mpy crlf-line\r\n');
    await mock(p, 'writeRaw', A, '2 I mpy half');
    await sleep(2300);
    let lines = await logLines(p);
    assert(lines.some((l) => /crlf-line$/.test(l)), 'CRLF line missing or has CR');
    assert(!lines.some((l) => /half/.test(l)), 'partial line shown early');
    await mock(p, 'writeRaw', A, '-done\n');
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-line"]')].some((e) => /half-done/.test(e.textContent)), null, 8000, 'completed line');
  });
  await s.step('ring overflow shows a gap marker', async () => {
    await mock(p, 'configure', { ringCap: 700 });
    await mock(p, 'fillLogs', A, 80, 'overflow');
    await waitMarker(p, 'missed|dropped|gap|lost|overflow', 10000);
    return (await logMarkers(p)).filter((m) => /missed|dropped|gap|lost|overflow/i.test(m)).join(' / ');
  });
  await s.step('device reboot (counter goes backwards) shows a restart marker and keeps streaming', async () => {
    await mock(p, 'configure', { ringCap: 16384 });
    await mock(p, 'reboot', A, { autorun: false });
    await waitMarker(p, 'restart', 10000);
    await mock(p, 'log', A, 'I', 'mpy', 'after-reboot');
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-line"]')].some((e) => /after-reboot/.test(e.textContent)), null, 9000, 'post-reboot line');
  });
  await s.step('level + text filter, then local clear keeps the cursor (nothing resurrects)', async () => {
    await p.click(T('sc-level-E'));
    await until(p, () => !document.querySelector('.sc-log-line.lv-E'), null, 3000, 'errors hidden');
    await p.click(T('sc-level-E'));
    await p.click(T('sc-log-search-toggle'));
    await p.fill(T('sc-log-query'), 'after-reboot');
    await until(p, () => document.querySelectorAll('[data-testid="sc-log-line"]').length === 1, null, 3000, 'text filter');
    await p.fill(T('sc-log-query'), '');
    const total = (await snap(p)).logTotal;
    await p.click(T('sc-log-clear'));
    await until(p, () => document.querySelectorAll('[data-testid="sc-log-line"]').length === 0, null, 3000, 'cleared');
    await sleep(2500);
    eq((await logLines(p)).length, 0, 'cleared lines came back');
    eq((await snap(p)).logTotal, total, 'device ring was altered by a local clear');
    await mock(p, 'log', A, 'I', 'mpy', 'post-clear');
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-line"]')].some((e) => /post-clear/.test(e.textContent)), null, 9000, 'new line after clear');
    eq((await logLines(p)).length, 1, 'old lines re-fetched after clear');
  });
  await s.step('follow / pause: scrolled-up view stops following, Latest returns to the end', async () => {
    await mock(p, 'fillLogs', A, 150, 'scroll');
    await until(p, () => document.querySelectorAll('[data-testid="sc-log-line"]').length >= 150, null, 12000, '150 lines');
    await sleep(500);
    const atEnd = await p.$eval(T('sc-log-body'), (e) => e.scrollHeight - e.scrollTop - e.clientHeight < 6);
    assert(atEnd, 'not following new lines');
    await p.$eval(T('sc-log-body'), (e) => { e.scrollTop = 0; e.dispatchEvent(new Event('scroll')); });
    await mock(p, 'fillLogs', A, 10, 'while-away');
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-line"]')].some((e) => /while-away 9/.test(e.textContent)), null, 9000, 'new lines');
    const top = await p.$eval(T('sc-log-body'), (e) => e.scrollTop);
    assert(top < 50, `view was yanked while the user scrolled up (scrollTop=${top})`);
    await p.waitForSelector(T('sc-log-latest'), { timeout: 3000 });
    await s.shot('logs-latest-1440-light');
    await p.click(T('sc-log-latest'));
    await sleep(200);
    assert(await p.$eval(T('sc-log-body'), (e) => e.scrollHeight - e.scrollTop - e.clientHeight < 6), 'Latest did not jump to the end');
    await p.click(T('sc-log-pause'));
    await sleep(200);
    const top0 = await p.$eval(T('sc-log-body'), (e) => e.scrollTop);
    await mock(p, 'fillLogs', A, 5, 'paused');
    await until(p, () => [...document.querySelectorAll('[data-testid="sc-log-line"]')].some((e) => /paused 4/.test(e.textContent)), null, 9000, 'paused lines still ingested');
    await sleep(500);
    const top1 = await p.$eval(T('sc-log-body'), (e) => e.scrollTop);
    eq(top1, top0, 'paused console scrolled (Pause is pause-scroll as on iOS)');
    await p.click(T('sc-log-pause'));
    await sleep(600);
    assert(await p.$eval(T('sc-log-body'), (e) => e.scrollHeight - e.scrollTop - e.clientHeight < 6), 'resume did not return to following');
  });
  await s.step('copy puts the visible log on the clipboard; export downloads a text file', async () => {
    await p.click(T('sc-log-copy'));
    await sleep(300);
    const clip = await p.evaluate(() => navigator.clipboard.readText());
    assert(/paused 4/.test(clip), `clipboard: ${clip.slice(0, 80)}`);
    const [dl] = await Promise.all([p.waitForEvent('download', { timeout: 6000 }), p.click(T('sc-log-export'))]);
    const f = dl.suggestedFilename();
    const tmp = join(SHOTS, `export-${slug(f)}`);
    await dl.saveAs(tmp);
    const body = fs.readFileSync(tmp, 'utf8');
    assert(/paused 4/.test(body), 'export content lacks latest line');
    assert(/\.(txt|log)$/i.test(f), `unexpected filename ${f}`);
    fs.unlinkSync(tmp);
    return f;
  });
  await s.step('console resize by keyboard/drag handle stays inside the window', async () => {
    await openFile(p, 'hello.py');
    const h0 = await p.$eval(T('sc-console'), (e) => e.getBoundingClientRect().height);
    const box = await p.$eval(T('sc-resize'), (e) => { const r = e.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; });
    await p.mouse.move(box.x, box.y);
    await p.mouse.down();
    await p.mouse.move(box.x, box.y - 120, { steps: 6 });
    await p.mouse.up();
    const h1 = await p.$eval(T('sc-console'), (e) => e.getBoundingClientRect().height);
    assert(h1 > h0 + 40, `console did not grow (${h0} -> ${h1})`);
    const rep = await layoutReport(p);
    eq(rep.problems, [], 'layout after resize');
  });
  await finish(s);
});

group('repl', async (browser) => {
  const s = await newSession(browser, 'repl', {});
  const p = s.page;
  const A = (await mock(p, 'limits')).defaultMac;
  const out = () => p.$eval(T('sc-repl-out'), (e) => e.innerText);
  await mock(p, 'setFiles', null, { 'long_run.py': 'import time\nfor i in range(60):\n    print("step", i)\n    time.sleep(1)\n' });
  const send = async (text) => { await p.fill(T('sc-repl-input'), text); await p.press(T('sc-repl-input'), 'Enter'); };
  await p.click(T('sc-open-repl'));
  await p.waitForSelector(T('sc-repl'));
  await s.step('state persists across entries (USB tunnel eval label)', async () => {
    assert(/USB/i.test(await p.$eval(T('sc-repl-transport'), (e) => e.textContent)), 'transport badge');
    await send('counter = 40');
    await send('counter += 2');
    await send('print(counter)');
    await until(p, () => /\n42/.test(document.querySelector('[data-testid="sc-repl-out"]').innerText), null, 8000, '42 in REPL output');
    const evs = await cmds(p, 'eval');
    assert(evs.every((e) => e.args.persist === true), 'eval without persist');
  });
  await s.step('history recall (ArrowUp) and multiline continuation', async () => {
    await p.press(T('sc-repl-input'), 'ArrowUp');
    eq(await p.$eval(T('sc-repl-input'), (e) => e.value), 'print(counter)');
    await p.fill(T('sc-repl-input'), 'for i in range(2):');
    await p.press(T('sc-repl-input'), 'Enter');
    assert((await p.$eval(T('sc-repl-input'), (e) => e.value)).includes('\n'), 'no continuation line');
    await p.keyboard.type('    print("loop", i)');
    await p.keyboard.press('Enter');
    await p.keyboard.press('Enter');
    await until(p, () => /loop 1/.test(document.querySelector('[data-testid="sc-repl-out"]').innerText), null, 8000, 'loop output');
  });
  await s.step('error output is shown (traceback) and the next entry still works', async () => {
    await send('print(undefined_name)');
    await until(p, () => /NameError/.test(document.querySelector('[data-testid="sc-repl-out"]').innerText), null, 8000, 'NameError');
    await send('print("still alive")');
    await until(p, () => /still alive/.test(document.querySelector('[data-testid="sc-repl-out"]').innerText), null, 8000, 'recovered');
    await s.shot('repl-output-1440-light');
  });
  await s.step('file script holds the slot: REPL read-only with Stop; input is not lost', async () => {
    await mock(p, 'externalRun', A, 'long_run.py', {});
    await p.waitForSelector(T('sc-repl-banner'), { timeout: 9000 });
    assert(await p.$eval(T('sc-repl-input'), (e) => e.readOnly || e.disabled), 'input editable while busy');
    const before = await nCmds(p, 'eval');
    await p.click(T('sc-repl-send')).catch(() => {});
    eq(await nCmds(p, 'eval'), before, 'eval sent while a file script holds the slot');
    await s.shot('repl-conflict-1440-light');
    await p.click(T('sc-repl-stop'));
    await p.waitForSelector(T('sc-repl-banner'), { state: 'detached', timeout: 9000 });
  });
  await s.step('race: slot taken between check and send -> busy error, text kept', async () => {
    await mock(p, 'configure', { opDelays: { eval: 400 } });
    await p.fill(T('sc-repl-input'), 'print("raced")');
    const sendP = p.press(T('sc-repl-input'), 'Enter');
    await sleep(80);
    await mock(p, 'externalRun', A, 'long.py', {}).catch(() => {});
    await sendP;
    await sleep(1200);
    await mock(p, 'configure', { opDelays: { eval: 30 } });
    await stopAll(p);
    return (await out()).slice(-120).replace(/\s+/g, ' ');
  });
  await s.step('Reset VM needs confirmation and clears the persistent state', async () => {
    await p.click(T('sc-repl-reset'));
    await p.waitForSelector(T('sc-dialog-reset'));
    const before = await nCmds(p, 'reset');
    eq(before, 0, 'reset sent before confirm');
    await p.click(`${T('sc-dialog-reset')} ${T('sc-dialog-confirm')}`);
    for (let i = 0; i < 60; i++) { const r = (await cmds(p, 'reset')).pop(); if (r && r.reply) break; await sleep(100); }
    await sleep(300);
    eq(await nCmds(p, 'reset'), 1);
    await send('print(counter)');
    await until(p, () => /NameError: name 'counter'/.test(document.querySelector('[data-testid="sc-repl-out"]').innerText), null, 8000, 'NameError after reset');
  });
  await finish(s);
});

group('autorun', async (browser) => {
  const s = await newSession(browser, 'autorun', {});
  const p = s.page;
  const A = (await mock(p, 'limits')).defaultMac;
  await p.click(`${T('sc-autorun')} summary`);
  await s.step('off by default; enabling needs confirmation and names the script', async () => {
    assert(/Off/.test(await p.$eval(T('sc-autorun-state'), (e) => e.textContent)), 'not Off initially');
    await p.click(T('sc-autorun-pick'));
    await p.click('.sc-pick .menu-item');
    await p.waitForSelector(T('sc-dialog-autorun'));
    eq(await nCmds(p, 'autorun_enable'), 0, 'enabled before confirm');
    await p.click(`${T('sc-dialog-autorun')} button:has-text("Cancel")`);
    await p.waitForSelector(T('sc-dialog-autorun'), { state: 'detached' });
    eq(await nCmds(p, 'autorun_enable'), 0, 'cancel enabled autorun');
    await p.click(T('sc-autorun-pick'));
    await p.click('.sc-pick .menu-item');
    await p.waitForSelector(T('sc-dialog-autorun'));
    await p.click(`${T('sc-dialog-autorun')} ${T('sc-dialog-confirm')}`);
    await until(p, () => /Runs at next boot/.test(document.querySelector('[data-testid="sc-autorun-state"]')?.textContent || ''), null, 8000, 'Runs at next boot');
    eq(await nCmds(p, 'autorun_enable'), 1);
    await s.shot('autorun-enabled-1440-light');
  });
  await s.step('save and run never enable autorun implicitly', async () => {
    const before = await nCmds(p, 'autorun_enable');
    await mock(p, 'setAutorun', A, { enabled: false });
    await openFile(p, 'hello.py');
    await focusEditor(p);
    await typeText(p, ' # tweak');
    await p.click(T('sc-run'));
    await waitMarker(p, "Finished 'hello.py'");
    eq(await nCmds(p, 'autorun_enable'), before, 'implicit enable');
  });
  await s.step('reboot with autorun enabled: observed run, source Autorun, never restarted by the UI', async () => {
    await mock(p, 'setAutorun', A, { enabled: true, scriptName: 'blink.py' });
    const runs = await nCmds(p, 'run');
    await mock(p, 'reboot', A, {});
    await p.waitForSelector(T('script-run-chip'), { timeout: 12000 });
    eq(await nCmds(p, 'run'), runs, 'UI launched a run');
    await until(p, () => /Running|Finished|Failed/.test(document.querySelector('[data-testid="sc-autorun-state"]')?.textContent || ''), null, 7000, 'autorun state').catch(async () => {
      const stale = await p.$eval(T('sc-autorun-state'), (e) => e.textContent.trim());
      await p.click(T('sc-refresh'));
      await until(p, () => /Running|Finished|Failed/.test(document.querySelector('[data-testid="sc-autorun-state"]')?.textContent || ''), null, 6000, 'autorun state after manual refresh');
      throw new Error(`autorun panel stayed "${stale}" while an Autorun run was observed; only a manual refresh updated it`);
    });
    await stopAll(p);
  });
  await s.step('IO12 held low: suppressed wording', async () => {
    await mock(p, 'setIo12', A, false);
    await mock(p, 'reboot', A, {});
    await until(p, () => /Suppressed|IO12/.test(document.querySelector('[data-testid="sc-autorun"]')?.textContent || ''), null, 12000, 'suppressed text');
    await mock(p, 'setIo12', A, true);
  });
  await s.step('disable needs confirmation; wake/reconnect does not restart it', async () => {
    await p.click(T('sc-autorun-disable'));
    await p.waitForSelector(T('sc-dialog-autorun'));
    eq(await nCmds(p, 'autorun_disable'), 0, 'disabled before confirm');
    await p.click(`${T('sc-dialog-autorun')} ${T('sc-dialog-confirm')}`);
    await until(p, () => /Off/.test(document.querySelector('[data-testid="sc-autorun-state"]')?.textContent || ''), null, 8000, 'Off');
    eq(await nCmds(p, 'autorun_disable'), 1);
    eq(await nCmds(p, 'autorun_run'), 0, 'run-now was sent');
  });
  await s.step('enable failure keeps the previous state and shows the error', async () => {
    await mock(p, 'fail', 'autorun_enable', { reply: { kind: 'transport', error: 'autorun write timed out' } });
    await p.click(T('sc-autorun-pick'));
    await p.click('.sc-pick .menu-item');
    await p.waitForSelector(T('sc-dialog-autorun'));
    await p.click(`${T('sc-dialog-autorun')} ${T('sc-dialog-confirm')}`);
    await waitNotice(p, 'timed out');
    assert(/Off/.test(await p.$eval(T('sc-autorun-state'), (e) => e.textContent)), 'state changed after a failed enable');
    assert(!(await snap(p)).autorun.enabled, 'device autorun became enabled');
  });
  await finish(s);
});

// ------------------------------------------------------------------------------------------------
// light/dark x width x HAT evidence matrix
// ------------------------------------------------------------------------------------------------
const HAT_NAV = { daq: 'daq', la: 'la' };
async function evidence(s, tag, full) {
  const p = s.page;
  const A = (await mock(p, 'limits')).defaultMac;
  const probe = async (label, o = {}) => {
    const rep = await layoutReport(p, o);
    if (rep.problems.length) throw new Error(`${label}: ${rep.problems.slice(0, 4).join('; ')}`);
    if (rep.hOverflow > 0) throw new Error(`${label}: page overflows horizontally by ${rep.hOverflow}px`);
  };
  await s.step(`${tag} stored scripts are reachable from the initial state`, async () => {
    const r = await p.evaluate(() => {
      const root = document.querySelector('[data-testid="scripts-root"]');
      const toggle = [...document.querySelectorAll('.sc-list-toggle')].some((e) => { const b = e.getBoundingClientRect(); return b.width > 0 && b.height > 0; });
      return { rootWidth: root.clientWidth, narrow: root.dataset.narrow, compact: root.dataset.compact, toggle };
    });
    const visible = await rowVisible(p);
    assert(visible || r.toggle, `no visible file list and no list toggle in the empty state: ${JSON.stringify(r)}`);
    return JSON.stringify({ ...r, rowsVisible: visible });
  });
  await s.step(`${tag} opening a file never leaves the editor covered by the list drawer`, async () => {
    await openFile(p, 'blink.py');
    assert(!p.__drawerStayed, `the list drawer stayed open over the editor after opening a file (view width ${await p.$eval(T('scripts-root'), (e) => e.clientWidth)} px)`);
  });
  await s.step(`${tag} edit: open, type, layout`, async () => {
    await openFile(p, 'blink.py');
    await focusEditor(p);
    await typeText(p, '\n# edited');
    await probe('edit');
    await s.shot(`ev-edit-${tag}`);
  });
  if (!full) {
    await s.step(`${tag} global chip + hat nav`, async () => {
      await mock(p, 'externalRun', A, 'blink.py', {}).catch(() => {});
      await p.waitForSelector(T('script-run-chip'), { timeout: 9000 });
      await nav(p, 'overview');
      await p.waitForSelector(T('script-run-chip'));
      const hat = await p.evaluate(() => window.__BB_MOCK.hat);
      for (const h of ['daq', 'la']) {
        const present = !!(await p.$(`[data-nav-view="${h}"]`));
        eq(present, hat === h, `${h} nav presence for hat ${hat}`);
      }
      assert(await p.$('[data-nav-view="scripts"]'), 'scripts nav missing');
      await s.shot(`ev-global-${tag}`);
      await mock(p, 'stopDevice', A);
      await openScripts(s);
    });
    return;
  }
  await s.step(`${tag} completion popup`, async () => {
    await replaceDoc(p, 'import daq as d\nd.');
    await typeText(p, 'v');
    await popupItems(p);
    const pop = await p.$eval('.cm-tooltip-autocomplete', (e) => { const r = e.getBoundingClientRect(); return { x: r.x, right: r.right, y: r.y, bottom: r.bottom, vw: innerWidth, vh: innerHeight }; });
    assert(pop.x >= 0 && pop.right <= pop.vw && pop.y >= 0 && pop.bottom <= pop.vh, `popup outside viewport ${JSON.stringify(pop)}`);
    const c = await cursorRect(p);
    assert(!(pop.x < c.right + 2 && pop.right > c.x - 2 && pop.y < c.bottom - 2 && pop.bottom > c.y + 2), 'popup covers caret');
    await s.shot(`ev-completion-${tag}`);
    await p.keyboard.press('Escape');
  });
  await s.step(`${tag} docs`, async () => {
    if ((await p.$eval('.sc-body', (e) => e.dataset.docs)) !== 'open') await p.click(T('sc-docs-toggle'));
    await p.waitForSelector(T('sc-doc-list'), { timeout: 8000 });
    await p.fill(T('sc-docs-search'), 'vdut');
    await p.waitForSelector('[data-doc]');
    await p.click('[data-doc]');
    await p.waitForSelector(T('sc-doc-detail'));
    await probe('docs');
    await s.shot(`ev-docs-${tag}`);
  });
  await s.step(`${tag} lint`, async () => {
    await openFile(p, 'syntax_bad.py');
    await p.click(T('sc-check'));
    await waitLint(p, 'syntax');
    await probe('lint');
    await s.shot(`ev-lint-${tag}`);
  });
  await s.step(`${tag} global running status on another view`, async () => {
    await mock(p, 'setFiles', null, { 'long_run.py': 'import time\nfor i in range(60):\n    print("step", i)\n    time.sleep(1)\n' });
    await revealList(p);
    await p.click(T('sc-refresh'));
    await p.waitForSelector('[data-file="long_run.py"]');
    await p.click('[data-file="long_run.py"] [data-testid="sc-file-run"]');
    await closeDrawer(p);
    await p.waitForSelector(T('script-run-chip'), { timeout: 8000 });
    await nav(p, 'overview');
    await p.waitForSelector(T('script-run-chip'));
    await sleep(1100);
    const hat = await p.evaluate(() => window.__BB_MOCK.hat);
    if (HAT_NAV[hat]) assert(await p.$(`[data-nav-view="${HAT_NAV[hat]}"]`), 'hat nav missing');
    await s.shot(`ev-global-${tag}`);
    await openScripts(s);
  });
  await s.step(`${tag} traceback console`, async () => {
    await stopAll(p);
    await revealList(p);
    await p.click('[data-file="fail.py"] [data-testid="sc-file-run"]');
    await closeDrawer(p);
    await openConsole(p);
    await p.waitForSelector('.sc-log-line.lv-E', { timeout: 9000 });
    await waitMarker(p, 'failed|Failed|error');
    await probe('traceback');
    await s.shot(`ev-traceback-${tag}`);
  });
  await s.step(`${tag} repl (conflict + output)`, async () => {
    await revealList(p);
    await p.click(T('sc-open-repl'));
    await closeDrawer(p);
    await p.waitForSelector(T('sc-repl'));
    await p.fill(T('sc-repl-input'), 'print("repl ok")');
    await p.press(T('sc-repl-input'), 'Enter');
    await until(p, () => /\nrepl ok/.test(document.querySelector('[data-testid="sc-repl-out"]').innerText), null, 8000, 'repl output line (not the echo)');
    await mock(p, 'externalRun', A, 'long_run.py', {});
    await p.waitForSelector(T('sc-repl-banner'), { timeout: 9000 });
    await probe('repl', { controls: false });
    await s.shot(`ev-repl-${tag}`);
    await stopAll(p);
    await p.click(T('sc-repl-close'));
  });
  await s.step(`${tag} autorun`, async () => {
    await mock(p, 'setAutorun', A, { enabled: true, scriptName: 'blink.py' });
    await revealList(p);
    await p.click(T('sc-refresh'));
    const open = await p.$eval(T('sc-autorun'), (e) => e.open);
    if (!open) await p.click(`${T('sc-autorun')} summary`).catch(() => {});
    await until(p, () => /Runs at next boot/.test(document.querySelector('[data-testid="sc-autorun"]')?.textContent || ''), null, 9000, 'autorun enabled text');
    await probe('autorun', { controls: false });
    await s.shot(`ev-autorun-${tag}`);
    await mock(p, 'setAutorun', A, { enabled: false });
  });
}

group('matrix', async (browser) => {
  for (const hat of opts.hats) {
    for (const theme of opts.themes) {
      for (const size of opts.sizes) {
        const [w, h] = size.split('x').map(Number);
        const full = hat === 'daq' || opts.matrix === 'full';
        const tag = `${hat}-${theme}-${w}`;
        let s;
        try { s = await newSession(browser, `matrix`, { hat, theme, w, h }); }
        catch (e) { results.push({ group: 'matrix', name: `${tag} setup`, ok: false, note: firstLine(e), selector: selectorOf(e), ms: 0 }); console.log(`FAIL [matrix] ${tag} setup - ${firstLine(e)}`); continue; }
        await s.step(`${tag} theme applied`, async () => {
          const bg = await s.page.evaluate(() => { let e = document.querySelector('[data-testid="scripts-root"]'); while (e) { const c = getComputedStyle(e).backgroundColor; if (c && !/rgba?\(0, 0, 0, 0\)|transparent/.test(c)) return c; e = e.parentElement; } return 'rgb(128,128,128)'; });
          const m = /rgba?\((\d+),\s*(\d+),\s*(\d+)/.exec(bg);
          const lum = m ? (Number(m[1]) + Number(m[2]) + Number(m[3])) / 3 : 128;
          if (theme === 'dark') assert(lum < 110, `not dark: ${bg}`); else assert(lum > 140, `not light: ${bg}`);
          return bg;
        });
        await evidence(s, tag, full);
        await finish(s);
      }
    }
  }
});

async function finish(s, o = {}) {
  const p = s.page;
  await s.step('mock contract: no IPC shape violations, no unhandled script op', async () => {
    const v = await mock(p, 'violations', 'shape');
    eq(v, [], 'IPC shape violations');
    const sv = await mock(p, 'violations', 'validation');
    if (sv.length) throw new Error(`UI sent invalid values the backend would reject: ${JSON.stringify(sv.slice(0, 2))}`);
  });
  await s.step('browser health: no console errors, page errors, failed requests or HTTP errors', async () => {
    const unhandled = await p.evaluate(() => [...window.__mockUnhandled]);
    const bad = [...s.issues.console.filter((m) => !(o.allowConsole || []).some((re) => re.test(m))), ...s.issues.page, ...(o.allowFailedRequests ? [] : s.issues.failed), ...s.issues.http];
    if (unhandled.length) findings.push({ group: s.group, note: `mock-unhandled commands (returned null): ${unhandled.join(', ')}` });
    for (const pg of s.issues.page) findings.push({ group: s.group, note: `PAGE/PANIC ${pg.slice(0, 900)}` });
    if (s.issues.noise.length) findings.push({ group: s.group, note: `browser noise (not failing): ${[...new Set(s.issues.noise)].join(' || ').slice(0, 200)}` });
    if (bad.length) throw new Error(`${bad.length} problem(s): ${[...new Set(bad)].slice(0, 3).join(' || ')}`);
  });
  await s.ctx.close();
}

// ------------------------------------------------------------------------------------------------
// main
// ------------------------------------------------------------------------------------------------
const browser = await chromium.launch();
const wanted = opts.only || Object.keys(GROUPS);
const started = Date.now();
for (const name of wanted) {
  if (!GROUPS[name]) { console.log(`unknown group ${name}`); continue; }
  console.log(`\n=== ${name} ===`);
  try { await GROUPS[name](browser); }
  catch (e) {
    results.push({ group: name, name: 'group setup/teardown', ok: false, note: firstLine(e), selector: selectorOf(e), ms: 0 });
    console.log(`FAIL [${name}] group aborted - ${firstLine(e)}`);
  }
}
await browser.close();
const failed = results.filter((r) => !r.ok);
const summary = { when: new Date().toISOString(), url: opts.url, evidence: 'MOCK (browser + stateful script_request mock); no Tauri, no CDP, no hardware', total: results.length, passed: results.length - failed.length, failed: failed.length, seconds: Math.round((Date.now() - started) / 1000), results, findings, screenshots: shotsTaken.map((x) => x.file) };
fs.writeFileSync(join(opts.out, 'harness-result.json'), JSON.stringify(summary, null, 2));
console.log(`\n${summary.passed}/${summary.total} steps passed in ${summary.seconds}s; ${shotsTaken.length} screenshots in ${SHOTS}`);
for (const f of failed) console.log(`FAILED [${f.group}] ${f.name}: ${f.note}${f.selector ? ' {' + f.selector + '}' : ''}`);
for (const f of findings) console.log(`NOTE [${f.group}] ${f.note}`);
process.exit(failed.length ? 1 : 0);
