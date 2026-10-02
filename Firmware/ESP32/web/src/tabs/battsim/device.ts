// =============================================================================
// battsim/device.ts - battery-simulator HTTP transport (/api/daq/bs*, /api/daq/config)
// and an IndexedDB mirror of run files, so a month-long run downloads once and
// later visits fetch only the appended bytes.
// =============================================================================

import { request } from "../../api/core";
import { parseMeta, parseStatus, type BsMeta, type BsStatus, META_SIZE } from "./history";

const OP_STATUS = 0, OP_LIST_RUNS = 1, OP_RUN_DIR = 2, OP_SET_EPOCH = 5;
const CFG_SET = 1, CFG_ACTION = 4;
const READ_CHUNK = 2048;

const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
const unb64 = (s: string) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
const le16 = (v: number) => new Uint8Array([v & 0xff, (v >> 8) & 0xff]);
const le32 = (v: number) => new Uint8Array([v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]);
const cat = (...parts: Uint8Array[]) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
};

interface B64Reply { ok: boolean; n: number; data: string }

async function bs(op: number, args: Uint8Array = new Uint8Array()): Promise<Uint8Array> {
  const r = await request<B64Reply>("/api/daq/bs", { method: "POST", body: { op, args: hex(args) } });
  return unb64(r.data ?? "");
}

async function cfg(mac: string, op: number, args: Uint8Array): Promise<Uint8Array> {
  const r = await request<B64Reply>("/api/daq/config", { method: "POST", body: { op, args: hex(args) }, mac, admin: true });
  return unb64(r.data ?? "");
}

export async function readChunk(run: number, file: number, off: number, len: number): Promise<Uint8Array> {
  const r = await request<B64Reply>("/api/daq/bs/read", {
    method: "POST", body: { run, file, off, len: Math.min(len, READ_CHUNK) },
  });
  return unb64(r.data ?? "");
}

export async function readRange(run: number, file: number, off: number, size: number,
                                progress?: (done: number) => void): Promise<Uint8Array> {
  const parts: Uint8Array[] = [];
  let got = 0;
  while (got < size) {
    const want = size - got;
    const c = await readChunk(run, file, off + got, want);
    parts.push(c);
    got += c.length;
    progress?.(got);
    if (c.length === 0 || c.length < Math.min(want, READ_CHUNK)) break;
  }
  return cat(...parts);
}

export const status = async (): Promise<BsStatus> => parseStatus(await bs(OP_STATUS));
export const setEpoch = async () => { await bs(OP_SET_EPOCH, le32(Math.floor(Date.now() / 1000))); };

export interface RunFile { id: number; size: number; name: string }
export function fileName(id: number): string {
  if (id >= 0x100) return `m${String(id - 0x100).padStart(4, "0")}.bin`;
  return ["meta.bin", "ckpt.bin", "ev.bin", "q15.bin", "s1.bin"][id] ?? `f${id}.bin`;
}

export async function runDir(run: number): Promise<RunFile[]> {
  const files: RunFile[] = [];
  for (;;) {
    const raw = await bs(OP_RUN_DIR, cat(le16(run), new Uint8Array([files.length & 0xff])));
    const d = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    const total = raw[2] ?? 0;
    const n = Math.floor((raw.length - 4) / 6);
    for (let i = 0; i < n; i++) {
      const id = d.getUint16(4 + 6 * i, true);
      files.push({ id, size: d.getUint32(6 + 6 * i, true), name: fileName(id) });
    }
    if (n === 0 || files.length >= total) return files;
  }
}

export interface RunSummary { runId: number; active: boolean; meta: BsMeta | null; bytes: number }

export async function listRuns(): Promise<RunSummary[]> {
  const ids: number[] = [];
  let active = 0;
  for (;;) {
    const raw = await bs(OP_LIST_RUNS, le16(ids.length));
    const d = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    const total = d.getUint16(0, true);
    active = d.getUint16(2, true);
    const page = Math.floor((raw.length - 4) / 2);
    for (let i = 0; i < page; i++) ids.push(d.getUint16(4 + 2 * i, true));
    if (page === 0 || ids.length >= total) break;
  }
  const out: RunSummary[] = [];
  for (const id of ids) {
    let meta: BsMeta | null = null;
    let bytes = 0;
    try { meta = parseMeta(await readRange(id, 0, 0, META_SIZE)); } catch { /* corrupt or missing */ }
    try { bytes = (await runDir(id)).reduce((s, f) => s + f.size, 0); } catch { /* ignore */ }
    out.push({ runId: id, active: id === active, meta, bytes });
  }
  return out;
}

// --- Settings registry (DAQ_K_BS_*), admin token required ---------------------
export type CfgType = "bool" | "u8" | "u16" | "u32" | "enum" | "str";
const TYPE_TAG: Record<CfgType, number> = { bool: 1, u8: 2, u16: 4, u32: 6, enum: 9, str: 10 };

export async function cfgSet(mac: string, key: number, type: CfgType, value: number | boolean | string) {
  let v: Uint8Array;
  if (type === "str") v = new TextEncoder().encode(String(value)).subarray(0, 23);
  else if (type === "u16") v = le16(Number(value));
  else if (type === "u32") v = le32(Number(value));
  else v = new Uint8Array([Number(value) & 0xff]);
  await cfg(mac, CFG_SET, cat(le16(key), new Uint8Array([TYPE_TAG[type], v.length]), v));
}

export async function action(mac: string, id: number, opts: { run?: number; slot?: number } = {}) {
  if (opts.run !== undefined) await cfgSet(mac, 0x080e, "u16", opts.run);
  if (opts.slot !== undefined) await cfgSet(mac, 0x080d, "u8", opts.slot);
  await cfg(mac, CFG_ACTION, new Uint8Array([id]));
}

// --- IndexedDB mirror ------------------------------------------------------------
const DB = "bb-battsim";
function db(): Promise<IDBDatabase> {
  return new Promise((res, rej) => {
    const r = indexedDB.open(DB, 1);
    r.onupgradeneeded = () => r.result.createObjectStore("files");
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
}
async function idb<T>(mode: IDBTransactionMode, fn: (s: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const d = await db();
  return new Promise((res, rej) => {
    const tx = d.transaction("files", mode);
    const rq = fn(tx.objectStore("files"));
    rq.onsuccess = () => res(rq.result);
    rq.onerror = () => rej(rq.error);
  });
}

/** Mirror every file of a run (append-only tails), return name -> bytes. */
export async function syncRun(run: number, onProgress?: (file: string, done: number, total: number) => void):
    Promise<Record<string, Uint8Array>> {
  const meta = parseMeta(await readRange(run, 0, 0, META_SIZE));
  const prefix = `${meta.createdEpoch}/${run}/`;
  const remote = await runDir(run);
  const total = remote.reduce((s, f) => s + f.size, 0);
  const out: Record<string, Uint8Array> = {};
  let base = 0;
  for (const f of remote) {
    const key = prefix + f.name;
    let local: Uint8Array = new Uint8Array();
    if (f.id >= 2 && f.id !== 4) {
      try { local = ((await idb("readonly", (s) => s.get(key))) as Uint8Array | undefined) ?? new Uint8Array(); } catch { /* none */ }
    }
    // q15 is compacted in place and day files age out: verify the head.
    if (local.length > f.size) local = new Uint8Array();
    else if (local.length) {
      const n = Math.min(local.length, 236);
      const head = await readChunk(run, f.id, 0, n);
      if (head.some((b, i) => b !== local[i])) local = new Uint8Array();
    }
    if (local.length < f.size) {
      const tail = await readRange(run, f.id, local.length, f.size - local.length,
        (d) => onProgress?.(f.name, base + local.length + d, total));
      local = cat(local, tail);
      try { await idb("readwrite", (s) => s.put(local, key)); } catch { /* quota: keep in memory only */ }
    }
    out[f.name] = local;
    base += f.size;
  }
  return out;
}
