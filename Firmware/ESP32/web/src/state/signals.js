// =============================================================================
// Global app signals — reactive state shared across tabs.
// =============================================================================
import { signal, computed } from "@preact/signals";
import { api } from "../api/client";
/* ---- Pairing ---- */
export const deviceInfo = signal(null);
export const pairingInfo = signal(null);
export const pairingRequired = signal(false);
export const deviceMac = computed(() => pairingInfo.value?.macAddress ?? deviceInfo.value?.macAddress ?? null);
/* ---- Core status ---- */
/** Last /api/status snapshot. Typed as `any` for now — refine incrementally. */
export const deviceStatus = signal(null);
/* ---- Selftest monitor ---- */
export const selftestStatus = signal(null);
export const selftestWorkerEnabled = signal(false);
export const supplyMonitorActive = signal(false);
let selftestPollRefs = 0;
let selftestPollTimer = null;
let selftestPollInFlight = false;
// Stale-poll suppression: if a poll started before the user's last write
// (plus a 750 ms grace for firmware-side propagation), we discard its
// response so the UI doesn't bounce back to the pre-write state.
let selftestLastLocalWriteMs = 0;
const SELFTEST_WRITE_GRACE_MS = 750;
function applySelftestStatus(status) {
    selftestStatus.value = status;
    selftestWorkerEnabled.value = !!status.workerEnabled;
    supplyMonitorActive.value = !!status.supplyMonitorActive;
}
async function pollSelftestStatus() {
    if (selftestPollRefs <= 0 || selftestPollInFlight)
        return;
    selftestPollInFlight = true;
    const startedMs = Date.now();
    try {
        const status = await api.selftestStatus();
        if (startedMs >= selftestLastLocalWriteMs + SELFTEST_WRITE_GRACE_MS) {
            applySelftestStatus(status);
        }
    }
    catch {
        /* Older firmware may not expose the extended response yet; keep last state. */
    }
    finally {
        selftestPollInFlight = false;
        if (selftestPollRefs > 0) {
            selftestPollTimer = window.setTimeout(() => {
                void pollSelftestStatus();
            }, 2500);
        }
    }
}
export function setSelftestStatus(status) {
    selftestLastLocalWriteMs = Date.now();
    applySelftestStatus(status);
}
export function startSelftestStatusPolling() {
    selftestPollRefs += 1;
    if (selftestPollRefs === 1) {
        if (selftestPollTimer !== null) {
            window.clearTimeout(selftestPollTimer);
            selftestPollTimer = null;
        }
        void pollSelftestStatus();
    }
    return () => {
        selftestPollRefs = Math.max(0, selftestPollRefs - 1);
        if (selftestPollRefs === 0 && selftestPollTimer !== null) {
            window.clearTimeout(selftestPollTimer);
            selftestPollTimer = null;
        }
    };
}
/* ---- Board profile ---- */
export const boardState = signal(null);
export const scopeBuffer = signal([]);
export const scopeSeq = signal(0);
export const scopeRunning = signal(true);
export const scopePlotMode = signal("overlay");
export const scopeChannelEnabled = signal([true, true, true, true]);
export const scopeChannelOffset = signal([
    0, 0, 0, 0,
]);
export const scopeChannelInvert = signal([
    false, false, false, false,
]);
export const scopeTriggerLevel = signal(0);
export const scopeTimeBase = signal(1); /* seconds of window */
/* Ring-buffer size; drop oldest samples beyond this. */
export const SCOPE_RING_CAPACITY = 8192;
export function pushScopeSamples(seq, samples) {
    if (samples.length === 0)
        return;
    const buf = scopeBuffer.value.slice();
    // Firmware /api/scope bucket layout (see webserver.cpp handle_get_scope):
    //   [t_ms, ch0_avg, ch1_avg, ch2_avg, ch3_avg,
    //    ch0_min, ch0_max, ch1_min, ch1_max, ch2_min, ch2_max, ch3_min, ch3_max]
    // t_ms is millis_now() on the device; convert to seconds for plot math.
    for (const s of samples) {
        buf.push({
            t: (s[0] ?? 0) / 1000,
            v: [s[1] ?? 0, s[2] ?? 0, s[3] ?? 0, s[4] ?? 0],
        });
    }
    if (buf.length > SCOPE_RING_CAPACITY) {
        buf.splice(0, buf.length - SCOPE_RING_CAPACITY);
    }
    scopeBuffer.value = buf;
    scopeSeq.value = seq;
}
/* ---- Sparklines (Overview tab) ---- */
export const SPARK_RING_CAPACITY = 200;
export const channelSparks = signal([[], [], [], []]);
export function pushChannelSamples(values) {
    const next = channelSparks.value.map((arr, i) => {
        const copy = arr.slice();
        copy.push(values[i]);
        if (copy.length > SPARK_RING_CAPACITY)
            copy.shift();
        return copy;
    });
    channelSparks.value = next;
}
