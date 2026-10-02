// Screenshot + health report harness for the BugBuster desktop frontend (mock IPC, no hardware).
// Usage: node shoot.mjs [--url http://localhost:1431] [--hat daq|la|none] [--theme light|dark|both]
//                       [--only viewId,viewId] [--out shots] [--size 1440x900] [--clean]
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const o = { url: 'http://localhost:1431', hat: 'daq', theme: 'dark', only: null, out: 'shots', w: 1440, h: 900 };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i];
    const v = argv[i + 1];
    if (k === '--url') { o.url = v; i++; }
    else if (k === '--hat') { o.hat = v; i++; }
    else if (k === '--theme') { o.theme = v; i++; }
    else if (k === '--only') { o.only = v.split(',').map((s) => s.trim()).filter(Boolean); i++; }
    else if (k === '--out') { o.out = v; i++; }
    else if (k === '--size') { [o.w, o.h] = v.split('x').map(Number); i++; }
    else if (k === '--clean') { o.clean = true; }
  }
  return o;
}
const opts = parseArgs(process.argv);
const outRoot = path.resolve(here, opts.out);

// Mirrors CATEGORIES in src/app.rs.
const CATEGORIES = [
  ['Overview', [['overview', 'Dashboard'], ['board', 'Board Map'], ['voltages', 'Voltages & Cal'], ['faults', 'Faults'], ['diag', 'Diagnostics']]],
  ['Analog', [['adc', 'ADC'], ['vdac', 'VDAC'], ['idac', 'IDAC'], ['iin', 'IIN']]],
  ['Digital', [['gpio', 'GPIO'], ['din', 'DIN'], ['dout', 'DOUT'], ['hv_io', 'HV IO'], ['ioexp', 'IO Expander']]],
  ['Instruments', [['scope', 'Scope'], ['la', 'Logic Analyzer'], ['daq', 'HS DAQ'], ['wavegen', 'WaveGen'], ['sigpath', 'Signal Path']]],
  ['System', [['hat', 'HAT'], ['usbpd', 'USB PD'], ['uart', 'UART']]],
];
const VIEWS = [];
for (const [cat, tabs] of CATEGORIES) for (const [id, label] of tabs) VIEWS.push({ id, label, cat });
VIEWS.forEach((v, i) => { v.n = String(i + 1).padStart(2, '0'); });

function hiddenFor(hat, id) {
  if (id === 'la') return hat !== 'la';
  if (id === 'daq') return hat !== 'daq';
  if (id === 'hat') return hat === 'none';
  return false;
}

const WAIT_MS = { overview: 9500, scope: 6000, la: 3000, daq: 3000 };
const DEFAULT_WAIT_MS = 1500;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function clickByText(page, selector, text) {
  return page.evaluate(({ selector, text }) => {
    const el = [...document.querySelectorAll(selector)].find((e) => e.textContent.trim() === text);
    if (!el) return false;
    el.click();
    return true;
  }, { selector, text });
}

async function navigate(page, view) {
  const sel = `[data-nav-view="${view.id}"]`;
  const deadline = Date.now() + 8000;
  while (Date.now() < deadline) {
    const viaAttr = await page.evaluate((s) => {
      const el = document.querySelector(s);
      if (!el) return false;
      el.click();
      return true;
    }, sel);
    if (viaAttr) return 'data-nav-view';
    if (await clickByText(page, '.category-item', view.cat)) {
      await sleep(120);
      if (await clickByText(page, '.tab-item', view.label)) return 'category+tab';
    }
    await sleep(250);
  }
  return null;
}

async function measure(page) {
  return page.evaluate(() => {
    const root = document.querySelector('.tab-container') || document.querySelector('[data-view-content]') || document.querySelector('main') || document.body;
    const text = (root.innerText || '').trim();
    const canvases = [...root.querySelectorAll('canvas')];
    let blank = 0;
    for (const c of canvases) {
      try {
        const ctx = c.getContext('2d');
        if (!ctx || !c.width || !c.height) continue;
        const d = ctx.getImageData(0, 0, Math.min(c.width, 512), Math.min(c.height, 512)).data;
        let same = true;
        for (let i = 4; i < d.length; i += 4) {
          if (d[i] !== d[0] || d[i + 1] !== d[1] || d[i + 2] !== d[2] || d[i + 3] !== d[3]) { same = false; break; }
        }
        if (same) blank++;
      } catch (e) { /* webgl or tainted canvas */ }
    }
    return { contentLen: text.length, canvases: canvases.length, blankCanvases: blank };
  });
}

async function runOne(browser, hat, theme) {
  const dir = path.join(outRoot, `${hat}-${theme}`);
  fs.mkdirSync(dir, { recursive: true });
  const context = await browser.newContext({
    viewport: { width: opts.w, height: opts.h },
    deviceScaleFactor: 1,
    colorScheme: theme,
    locale: 'en-US',
  });
  await context.addInitScript(`window.__BB_MOCK = ${JSON.stringify({ hat, connected: true, theme: null })};`);
  // Trunk's livereload socket would reload the page whenever a screenshot lands inside the watched tree.
  await context.addInitScript(`(function(){ var O = window.WebSocket; function W(u, p){ if (String(u).indexOf('.well-known/trunk/ws') >= 0) { return { close: function(){}, send: function(){}, addEventListener: function(){}, removeEventListener: function(){}, readyState: 0 }; } return p === undefined ? new O(u) : new O(u, p); } W.prototype = O.prototype; W.CONNECTING = 0; W.OPEN = 1; W.CLOSING = 2; W.CLOSED = 3; window.WebSocket = W; })();`);
  await context.addInitScript({ path: path.join(here, 'mock_ipc.js') });
  if (opts.clean) {
    await context.addInitScript(`document.addEventListener('DOMContentLoaded', function(){ var s = document.createElement('style'); s.textContent = '.toast-container{display:none!important}'; document.head.appendChild(s); });`);
  }
  await context.addInitScript(`(function(){ if (document.hidden) { window.requestAnimationFrame = function(cb){ return setTimeout(function(){ cb(performance.now()); }, 16); }; } })();`);
  const page = await context.newPage();

  let bucket = { consoleErrors: [], consoleWarnings: [], pageErrors: [] };
  page.on('console', (m) => {
    const t = m.type();
    const text = m.text();
    if (text.startsWith('[mock] unhandled')) return;
    if (text.includes('willReadFrequently') || text.includes('crbug.com/981419')) return; // harness readback / trunk preload noise
    if (t === 'error') bucket.consoleErrors.push(text.slice(0, 300));
    else if (t === 'warning') bucket.consoleWarnings.push(text.slice(0, 300));
  });
  page.on('pageerror', (e) => bucket.pageErrors.push(String(e && e.message ? e.message : e).slice(0, 300)));

  const results = [];
  // Trunk rebuilds on every file written under the project, so write all PNGs in one burst at the end.
  const pending = [];
  const shot = async (file) => { pending.push([file, await page.screenshot()]); };
  const flush = () => { for (const [f, b] of pending) fs.writeFileSync(f, b); };
  const unhandledNow = async () => page.evaluate(() => Array.from(window.__mockUnhandled || []));
  let seenUnhandled = new Set();

  async function record(viewId, file, extra) {
    const un = await unhandledNow();
    const delta = un.filter((c) => !seenUnhandled.has(c));
    un.forEach((c) => seenUnhandled.add(c));
    const m = await measure(page);
    const dedupe = (a) => [...new Set(a)].slice(0, 10);
    const r = {
      hat, theme, view: viewId, file,
      consoleErrors: dedupe(bucket.consoleErrors), consoleWarnings: dedupe(bucket.consoleWarnings), pageErrors: dedupe(bucket.pageErrors),
      unhandled: delta, ...m, empty: viewId === 'connect' ? false : m.contentLen < 20, ...extra,
    };
    bucket = { consoleErrors: [], consoleWarnings: [], pageErrors: [] };
    results.push(r);
  }

  await page.goto(opts.url, { waitUntil: 'load' });

  // Connection screen
  try {
    await page.waitForFunction(() => [...document.querySelectorAll('.device-item')].some((e) => /COM6/.test(e.textContent)), null, { timeout: 60000 });
  } catch (e) {
    await shot(path.join(dir, '00-connect.png'));
    await record('connect', path.relative(outRoot, path.join(dir, '00-connect.png')), { note: 'device list never populated (app not loaded?)' });
    flush();
    await context.close();
    return results;
  }
  await sleep(900);
  if (!opts.only || opts.only.includes('connect')) {
    const f = path.join(dir, '00-connect.png');
    await shot(f);
    await record('connect', path.relative(outRoot, f));
  } else {
    bucket = { consoleErrors: [], consoleWarnings: [], pageErrors: [] };
  }

  // Connect
  await page.evaluate(() => {
    const el = [...document.querySelectorAll('.device-item')].find((e) => /COM6/.test(e.textContent));
    if (el) el.click();
  });
  try {
    await page.waitForFunction(() => document.querySelector('.category-bar') || document.querySelector('[data-nav-view]'), null, { timeout: 20000 });
  } catch (e) {
    await shot(path.join(dir, '00-connect-failed.png'));
    results.push({ hat, theme, view: 'connect-flow', file: '', consoleErrors: bucket.consoleErrors, consoleWarnings: bucket.consoleWarnings, pageErrors: bucket.pageErrors, unhandled: [], empty: true, note: 'main layout never appeared after connect' });
    flush();
    await context.close();
    return results;
  }
  await sleep(1500); // let HAT detection latch and tabs appear
  bucket = { consoleErrors: [], consoleWarnings: [], pageErrors: [] };

  for (const view of VIEWS) {
    if (opts.only && !opts.only.includes(view.id)) continue;
    if (hiddenFor(hat, view.id)) {
      results.push({ hat, theme, view: view.id, file: '', consoleErrors: [], consoleWarnings: [], pageErrors: [], unhandled: [], empty: false, skipped: `hidden for hat=${hat}` });
      continue;
    }
    const how = await navigate(page, view);
    if (!how) {
      await record(view.id, '', { empty: true, note: 'navigation target not found' });
      continue;
    }
    await sleep(500);
    if (view.id === 'scope') {
      await page.evaluate(() => { const b = document.querySelector('.scope-pill'); if (b && /Start/.test(b.textContent)) b.click(); });
    } else if (view.id === 'la') {
      await clickByText(page, 'button', 'Arm');
    }
    await sleep(WAIT_MS[view.id] || DEFAULT_WAIT_MS);
    const file = path.join(dir, `${view.n}-${view.id}.png`);
    await shot(file);
    await record(view.id, path.relative(outRoot, file), { nav: how });
  }
  flush();
  await context.close();
  return results;
}

function printTable(rows) {
  const w = [14, 6, 7, 9, 7, 18];
  const pad = (s, n) => String(s).padEnd(n).slice(0, n);
  console.log([pad('view', w[0]), pad('hat', w[1]), pad('theme', w[2]), pad('errors', w[3]), pad('empty', w[4]), 'unhandled'].join(' '));
  for (const r of rows) {
    const errs = r.skipped ? 'skip' : `${r.consoleErrors.length}e/${r.pageErrors.length}p/${r.consoleWarnings.length}w`;
    console.log([pad(r.view, w[0]), pad(r.hat, w[1]), pad(r.theme, w[2]), pad(errs, w[3]), pad(r.skipped ? '-' : r.empty ? 'YES' : 'no', w[4]), r.unhandled.join(',') || '-'].join(' '));
  }
}

async function main() {
  const themes = opts.theme === 'both' ? ['dark', 'light'] : [opts.theme];
  const browser = await chromium.launch({
    headless: true,
    args: ['--lang=en-US', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--enable-webgl', '--disable-background-timer-throttling', '--disable-renderer-backgrounding'],
  });
  const all = [];
  try {
    for (const theme of themes) all.push(...(await runOne(browser, opts.hat, theme)));
  } finally {
    await browser.close();
  }
  fs.mkdirSync(outRoot, { recursive: true });
  const reportPath = path.join(outRoot, 'report.json');
  let existing = [];
  try { existing = JSON.parse(fs.readFileSync(reportPath, 'utf8')); } catch (e) { /* first run */ }
  const key = (r) => `${r.hat}|${r.theme}|${r.view}`;
  const fresh = new Set(all.map(key));
  const merged = existing.filter((r) => !fresh.has(key(r))).concat(all);
  fs.writeFileSync(reportPath, JSON.stringify(merged, null, 2), 'utf8');
  printTable(all);
  const bad = all.filter((r) => !r.skipped && (r.consoleErrors.length || r.pageErrors.length || r.empty || r.unhandled.length));
  console.log(`\n${all.length} rows, ${bad.length} with issues. Report: ${reportPath}`);
  for (const r of bad) {
    for (const m of [...r.pageErrors, ...r.consoleErrors].slice(0, 3)) console.log(`  [${r.view}] ${m}`);
  }
}

main().then(() => process.exit(0), (e) => { console.error(e); process.exit(0); });
