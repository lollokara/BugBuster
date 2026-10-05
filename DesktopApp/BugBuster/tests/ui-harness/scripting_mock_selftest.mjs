// Browser-free self-test of the stateful script_request mock in mock_ipc.js (window.__BB_SCRIPT_MOCK).
// Run: node --test scripting_mock_selftest.mjs     (mock evidence only: no Tauri, no device)
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import test, { after, beforeEach } from 'node:test';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const here = dirname(fileURLToPath(import.meta.url));
const code = readFileSync(join(here, 'mock_ipc.js'), 'utf8');
const timers = [];
const ctx = {
  console, setTimeout, clearTimeout, btoa, atob, TextEncoder, TextDecoder, Date, Promise, Uint8Array, Float32Array,
  setInterval: (...a) => { const t = setInterval(...a); timers.push(t); return t; }, clearInterval,
  document: { documentElement: null, addEventListener() {} },
};
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext(code, ctx);
const SM = ctx.window.__BB_SCRIPT_MOCK;
const invoke = (cmd, args) => ctx.window.__TAURI__.core.invoke(cmd, args);
const req = (operation, args) => invoke('script_request', args === undefined ? { operation } : { operation, args });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const P = (x) => JSON.parse(JSON.stringify(x));
const decode = (b64) => Buffer.from(b64, 'base64');
const events = [];
await ctx.window.__TAURI__.event.listen('script-upload-progress', (e) => events.push(e.payload));

async function until(fn, ms = 4000) {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() > end) throw new Error('timeout waiting for ' + fn.toString().slice(0, 80));
    await sleep(8);
  }
}

beforeEach(async () => {
  await invoke('disconnect_device', {});
  SM.reset();
  SM.configure({ delayMs: 2, stmtMs: 1, sleepScale: 0.02, stopMs: 15 });
  await invoke('connect_device', { deviceId: 'usb:COM6' });
  events.length = 0;
});
after(() => { timers.forEach(clearInterval); setTimeout(() => process.exit(0), 50); });

test('strict shapes: unknown op rejects, wrong shapes are not generic success', async () => {
  await assert.rejects(() => req('format_disk', {}), /unknown script operation/);
  for (const [op, args] of [['get', {}], ['get', { name: 5 }], ['save', { name: 'a.py' }], ['run', { name: 'a.py', background: 'yes' }], ['logs', { since: -1 }], ['logs', { since: 'x' }], ['files', { junk: 1 }], ['lint', {}], ['lint', { name: 'a.py', source: 'x' }], ['eval', { source: 4 }], ['status', []]]) {
    const r = await req(op, args);
    assert.equal(r.ok, false, `${op} ${JSON.stringify(args)}`);
    assert.equal(r.kind, 'invalid');
    assert.match(r.error, /mock contract/);
  }
  assert.equal(SM.violations('shape').length, 12);
  SM.clearViolations();
  await assert.rejects(() => invoke('script_request', { operation: 'files', args: {}, extra: 1 }).then((r) => { assert.equal(r.ok, true); throw new Error('extra key accepted'); }), /extra key accepted/);
  assert.equal(SM.violations('shape').length, 1);
});

test('device identity is per MAC and stable across USB and Wi-Fi', async () => {
  let r = await req('files');
  assert.deepEqual(P(r.device), { transport: 'usb', address: 'COM6', mac: SM.limits.defaultMac });
  await req('save', { name: 'only_a.py', source: 'x = 1\n' });
  await invoke('disconnect_device', {});
  r = await req('files');
  assert.equal(r.kind, 'not_connected');
  assert.equal(r.device, undefined);
  await invoke('connect_device', { deviceId: 'http://192.168.1.58' });
  r = await req('files');
  assert.equal(r.device.transport, 'http');
  assert.equal(r.device.mac, SM.limits.defaultMac);
  assert.ok(r.files.includes('only_a.py'), 'same device over Wi-Fi sees its files');
  SM.addDevice('usb:COM7', 'B', 'COM7');
  await invoke('connect_device', { deviceId: 'usb:COM7' });
  r = await req('files');
  assert.equal(r.device.mac, SM.limits.secondMac);
  assert.deepEqual(P(r.files), ['hello.py', 'other.py']);
});

test('save/get round trip, UTF-8 bytes, validation, limits', async () => {
  let r = await req('save', { name: 'u.py', source: 'print("\u00e9\u65e5\ud83d\ude00")\n' });
  assert.equal(r.ok, true);
  assert.equal(r.bytes, Buffer.byteLength('print("\u00e9\u65e5\ud83d\ude00")\n'));
  r = await req('get', { name: 'u.py' });
  assert.equal(r.source, 'print("\u00e9\u65e5\ud83d\ude00")\n');
  assert.equal(r.size, Buffer.byteLength(r.source));
  r = await req('save', { name: 'bad name.py', source: 'x' });
  assert.equal(r.kind, 'invalid');
  r = await req('save', { name: 'e.py', source: '' });
  assert.equal(r.kind, 'invalid');
  r = await req('save', { name: 'big.py', source: 'a'.repeat(32769) });
  assert.equal(r.kind, 'too_large');
  assert.equal((await req('get', { name: 'nope.py' })).kind, 'not_found');
  assert.equal((await req('get', { name: '.hidden.py' })).kind, 'invalid');
  assert.equal(SM.violations('shape').length, 0);
});

test('atomic save: failed upload keeps the old file, lost reply commits it, progress events fire', async () => {
  SM.setFiles(null, { 'keep.py': 'old = 1\n' });
  SM.fail('save', { atChunk: 2 });
  const big = 'x = 1\n'.repeat(900); // > 2 chunks over USB
  let r = await req('save', { name: 'keep.py', source: big });
  assert.equal(r.ok, false);
  assert.equal(r.kind, 'transport');
  assert.equal((await req('get', { name: 'keep.py' })).source, 'old = 1\n');
  assert.ok(events.length >= 2 && events.every((e) => e.name === 'keep.py'));
  SM.fail('save', { phase: 'after', reply: { kind: 'transport', error: 'lost reply' } });
  r = await req('save', { name: 'keep.py', source: 'new = 2\n' });
  assert.equal(r.ok, false);
  assert.equal((await req('get', { name: 'keep.py' })).source, 'new = 2\n');
});

test('storage limits: script count and capacity', async () => {
  const base = await req('storage');
  assert.equal(base.maxScriptBytes, 32768);
  SM.scenario('full-storage');
  const r = await req('save', { name: 'one_more.py', source: 'x = 1\n' });
  assert.equal(r.kind, 'firmware');
  assert.match(r.error, /storage full/);
});

test('run: quick completion, status transitions and non-draining cursor logs', async () => {
  const start = (await req('logs', { since: 0 })).next;
  let r = await req('run', { name: 'hello.py' });
  assert.equal(r.ok, true);
  assert.equal(r.name, 'hello.py');
  await until(async () => (await req('status')).state === 'done');
  const st = await req('status');
  assert.equal(st.lastExit, 'ok');
  assert.equal(st.fileSlotName, '');
  assert.equal(st.totalRuns, 1);
  const p1 = await req('logs', { since: start });
  const text = decode(p1.data).toString('utf8');
  assert.match(text, / I mpy hello\n$/);
  const again = await req('logs', { since: start });
  assert.equal(again.data, p1.data, 'reading twice does not drain');
  assert.equal(SM.readLogs(null, start).text, text, 'a second reader sees the same bytes');
  assert.equal(p1.restarted, false);
  assert.equal(p1.next, start + p1.n);
});

test('long run holds the file slot, busy/replace, stop, traceback', async () => {
  SM.configure({ sleepScale: 0.1 });
  let r = await req('run', { name: 'blink.py', background: true });
  assert.equal(r.background, true);
  let st = await req('status');
  assert.equal(st.running, true);
  assert.equal(st.fileSlotName, 'blink.py');
  assert.equal(st.state, 'running');
  r = await req('run', { name: 'hello.py' });
  assert.deepEqual([r.ok, r.kind, r.running], [false, 'busy', 'blink.py']);
  r = await req('eval', { source: 'print(1)' });
  assert.equal(r.kind, 'busy');
  r = await req('run', { name: 'fail.py', replace: true });
  assert.equal(r.ok, true);
  await until(async () => (await req('status')).state === 'error');
  st = await req('status');
  assert.equal(st.lastExit, 'error');
  assert.equal(st.lastError, 'ValueError: boom');
  const text = SM.readLogs(null, 0).text;
  assert.match(text, /E mpy Traceback \(most recent call last\):/);
  assert.match(text, /E mpy {3}File "fail.py", line 4, in <module>/);
  assert.match(text, /E mpy ValueError: boom/);
  assert.match(text, /script 'blink.py' stopped/);
  await req('run', { name: 'blink.py' });
  SM.configure({ stopMs: 200 });
  await req('stop');
  assert.equal((await req('status')).state, 'stopping');
  await until(async () => (await req('status')).lastExit === 'stopped');
});

test('launch operations never overlap (in_flight) and are counted', async () => {
  SM.configure({ opDelays: { run: 80 } });
  const [a, b] = await Promise.all([req('run', { name: 'hello.py' }), req('run', { name: 'hello.py' })]);
  assert.equal(a.ok, true);
  assert.equal(b.kind, 'in_flight');
  assert.equal(SM.counts().inFlightRejected, 1);
  assert.equal(SM.commands('run').length, 2);
});

test('lint: syntax error, busy, unsupported, transport, never executes', async () => {
  const before = SM.snapshot().status.totalRuns;
  let r = await req('lint', { source: 'print(1)\nx = (\n' });
  assert.deepEqual([r.ok, r.valid], [true, false]);
  assert.match(r.message, /SyntaxError: invalid syntax/);
  assert.match(r.message, /line 2/);
  assert.equal((await req('lint', { source: 'x = 1\n' })).valid, true);
  assert.equal((await req('lint', { name: 'syntax_bad.py' })).valid, false);
  assert.equal(SM.snapshot().status.totalRuns, before, 'lint does not execute');
  await req('run', { name: 'blink.py' });
  r = await req('lint', { source: 'x = 1\n' });
  assert.equal(r.kind, 'busy');
  await req('stop');
  await until(async () => (await req('status')).running === false);
  SM.setCaps({ lint: false });
  assert.equal((await req('lint', { source: 'x = 1\n' })).kind, 'unsupported');
  SM.setCaps({ lint: true });
  SM.fail('lint', { reply: { kind: 'transport', error: 'timeout' } });
  r = await req('lint', { source: 'x = 1\n' });
  assert.equal(r.kind, 'transport');
  assert.equal(r.outcomeUnknown, undefined, 'lint is safe to retry');
});

test('persistent eval keeps variables, excludes file scripts, reset clears', async () => {
  const eval1 = async (s, persist) => { await req('eval', persist === undefined ? { source: s } : { source: s, persist }); await until(async () => !(await req('status')).running); };
  await eval1('x = 40\nx += 2');
  await eval1('print(x)');
  assert.match(SM.readLogs(null, 0).text, / I mpy 42\n$/);
  await eval1('for i in range(2):\n    print("i", i)');
  assert.match(SM.readLogs(null, 0).text, / I mpy i 1\n$/);
  await eval1('print(1/0)');
  assert.match(SM.readLogs(null, 0).text, /E mpy ZeroDivisionError: divide by zero\n$/);
  await eval1('print(zzz)');
  assert.match(SM.readLogs(null, 0).text, /NameError: name 'zzz' isn't defined\n$/);
  await eval1('y = 5', false);
  await eval1('print(y)');
  assert.match(SM.readLogs(null, 0).text, /NameError: name 'y' isn't defined\n$/);
  assert.deepEqual(P(SM.snapshot().vars), ['x', 'i']);
  await req('reset');
  assert.deepEqual(P(SM.snapshot().vars), []);
  await eval1('print(x)');
  assert.match(SM.readLogs(null, 0).text, /NameError: name 'x' isn't defined\n$/);
  SM.configure({ sleepScale: 0.2 });
  await req('run', { name: 'blink.py' });
  assert.equal((await req('eval', { source: 'print(1)' })).kind, 'busy');
  await req('stop');
});

test('logs: independent cursors, split UTF-8, gap, reboot restart', async () => {
  SM.fillLogs(null, 3);
  const a = await req('logs', { since: 0 });
  const bStart = SM.readLogs(null, 0).next;
  SM.log(null, 'I', 'mpy', 'after');
  const b = await req('logs', { since: bStart });
  assert.match(decode(b.data).toString(), /after\n$/);
  assert.equal(decode(a.data).length, a.n);
  // a multi-byte character straddles the page boundary
  const cur = SM.snapshot().logTotal;
  const info = SM.logSplitUtf8(null, '\u00e9', cur);
  const page1 = await req('logs', { since: cur });
  assert.equal(page1.n, 4096);
  assert.equal(page1.more, true);
  const last = decode(page1.data)[4095];
  assert.equal(last, 0xc3, 'first byte of the split character ends page one');
  const page2 = await req('logs', { since: page1.next });
  assert.equal(decode(page2.data)[0], 0xa9);
  assert.equal(Buffer.concat([decode(page1.data), decode(page2.data)]).toString().includes('\u00e9 tail-after-split'), true);
  assert.equal(info.boundary, cur + 4096);
  // overflow: oldest bytes disappear and the reader is told
  SM.configure({ ringCap: 600 });
  SM.fillLogs(null, 60, 'spam');
  const gap = await req('logs', { since: 0 });
  assert.ok(gap.dropped > 0 && gap.restarted === false);
  assert.equal(gap.next, SM.snapshot().logBase + gap.n);
  // reboot: counter goes backwards
  const cursor = gap.next;
  SM.reboot();
  const re = await req('logs', { since: cursor });
  assert.deepEqual([re.restarted, re.n, re.next], [true, 0, 0]);
  assert.equal(SM.snapshot().bootCount, 2);
});

test('autorun: enable/disable, status, boot run, run-now needs USB, IO12 gate', async () => {
  assert.equal((await req('autorun_enable', { name: 'nope.py' })).kind, 'not_found');
  let r = await req('autorun_enable', { name: 'hello.py' });
  assert.equal(r.ok, true);
  r = await req('autorun_status');
  assert.deepEqual([r.enabled, r.scriptName, r.has_script, r.ranThisBoot, r.running], [true, 'hello.py', true, false, false]);
  SM.reboot();
  await until(async () => (await req('autorun_status')).ranThisBoot);
  await until(async () => (await req('status')).state === 'done');
  assert.equal((await req('status')).lastExit, 'ok');
  assert.equal((await req('autorun_status')).last_run_ok, true);
  await req('autorun_disable');
  assert.equal((await req('autorun_status')).enabled, false);
  await req('autorun_enable', { name: 'blink.py' });
  assert.equal((await req('autorun_run')).ok, true);
  assert.equal((await req('status')).source, 'autorun');
  await req('stop');
  await until(async () => !(await req('status')).running);
  await invoke('connect_device', { deviceId: 'http://192.168.1.58' });
  r = await req('autorun_run');
  assert.deepEqual([r.ok, r.kind], [false, 'unsupported']);
  SM.setIo12(null, false);
  SM.reboot();
  await sleep(SM.config().cfg.bootRunDelayMs + 60);
  assert.equal((await req('autorun_status')).ranThisBoot, false);
});

test('external runs and unknown outcomes', async () => {
  const id = SM.externalRun(null, 'blink.py', { kind: 'autorun' });
  const st = await req('status');
  assert.deepEqual([st.running, st.source, st.fileSlotName, st.fileSlotId], [true, 'autorun', 'blink.py', id]);
  SM.stopDevice();
  await until(async () => !(await req('status')).running);
  SM.fail('run', { reply: { kind: 'transport', error: 'timeout' } });
  const r = await req('run', { name: 'hello.py' });
  assert.deepEqual([r.kind, r.outcomeUnknown], ['transport', true]);
  assert.equal(SM.snapshot().status.totalRuns, 1, 'the injected failure never reached the device');
  SM.fail('run', { phase: 'after', reply: { kind: 'transport', error: 'reply lost' } });
  const r2 = await req('run', { name: 'hello.py' });
  assert.equal(r2.outcomeUnknown, true);
  assert.equal(SM.snapshot().status.totalRuns, 2, 'the device did run it');
});

test('connection loss mid request: device_changed by default, stale reply when asked', async () => {
  SM.hold('files', 'after');
  const p = req('files');
  await until(() => SM.held().length === 1);
  SM.addDevice('usb:COM7', 'B', 'COM7');
  await invoke('connect_device', { deviceId: 'usb:COM7' });
  SM.release('files');
  const r = await p;
  assert.deepEqual([r.ok, r.kind, r.device], [false, 'device_changed', undefined]);
  SM.scenario('stale-replies');
  SM.hold('files', 'after');
  const p2 = req('files');
  await until(() => SM.held().length === 1);
  await invoke('connect_device', { deviceId: 'usb:COM6' });
  SM.release('files');
  const r2 = await p2;
  assert.equal(r2.ok, true);
  assert.equal(r2.device.mac, SM.limits.secondMac, 'old device answer arrives labelled with the old identity');
  assert.deepEqual(P(r2.files), ['hello.py', 'other.py']);
});

test('link failures and scenario switches', async () => {
  SM.scenario('wifi-only');
  let r = await req('files');
  assert.deepEqual([r.kind, r.outcomeUnknown], ['transport', undefined]);
  r = await req('stop');
  assert.equal(r.outcomeUnknown, true);
  SM.setLink({ usb: true });
  assert.equal((await req('files')).ok, true);
  SM.scenario('old-firmware');
  assert.equal((await req('logs', { since: 0 })).kind, 'unsupported');
  assert.equal((await req('storage')).kind, 'unsupported');
});

test('interpreter: syntax and import errors become tracebacks', async () => {
  SM.setFiles(null, { 'imp.py': 'import missing_mod\nprint("never")\n' });
  await req('run', { name: 'syntax_bad.py' });
  await until(async () => (await req('status')).state === 'error');
  let t = SM.readLogs(null, 0).text;
  assert.match(t, /SyntaxError: invalid syntax/);
  assert.match(t, /File "syntax_bad.py", line 2\n/);
  await req('run', { name: 'imp.py' });
  await until(async () => (await req('status')).lastError.startsWith('ImportError'));
  t = SM.readLogs(null, 0).text;
  assert.doesNotMatch(t, /never/);
  assert.equal(SM.parse('def ok():\n    return 1\nif True: print(1)\n'), null);
  assert.notEqual(SM.parse('for i in range(3)\n    pass\n'), null);
  assert.notEqual(SM.parse('x = [1, 2\n'), null);
});
