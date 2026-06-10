// =============================================================================
// api/client.ts — compatibility facade: re-exports core + types, defines api{}.
//
// All consumers import from "../../api/client" as before. Internals have been
// split into:
//   api/core.ts   — auth cache, error classes, request(), adminRawFetch()
//   api/types.ts  — shared domain interfaces and type aliases
// =============================================================================
export { ADMIN_TOKEN_HEADER, PairingRequiredError, HttpError, IoOwnerRejectError, isPersistentlyRemembered, getCachedToken, setCachedToken, clearCachedToken, request, adminRawFetch, } from "./core";
import { ADMIN_TOKEN_HEADER, PairingRequiredError, HttpError, getCachedToken, setCachedToken, clearCachedToken, request, adminRawFetch, } from "./core";
/* ---- Typed endpoints ---- */
export const api = {
    deviceInfo: () => request("/api/device/info"),
    pairingInfo: () => request("/api/pairing/info"),
    pairingVerify: (mac, token, options = {}) => fetch("/api/pairing/verify", {
        method: "POST",
        headers: { [ADMIN_TOKEN_HEADER]: token },
    }).then((r) => {
        if (r.status === 200) {
            setCachedToken(mac, token, options);
            return true;
        }
        return false;
    }),
    pairingRotate: (mac) => request("/api/pairing/rotate", {
        method: "POST",
        mac,
        admin: true,
    }),
    status: () => request("/api/status"),
    overview: () => request("/api/overview"),
    scope: (since) => request(`/api/scope?since=${since}`),
    faults: () => request("/api/faults"),
    debug: () => request("/api/debug"),
    board: () => request("/api/board"),
    boardSelect: (mac, boardId) => request("/api/board/select", {
        method: "POST",
        body: { boardId },
        mac,
        admin: true,
    }),
    channel: {
        get: (ch) => request(`/api/channel/${ch}`),
        setDac: (mac, ch, code) => request(`/api/channel/${ch}/dac`, {
            method: "POST",
            body: { code },
            mac,
            admin: true,
        }),
        setFunction: (mac, ch, func) => request(`/api/channel/${ch}/function`, {
            method: "POST",
            body: { function: func },
            mac,
            admin: true,
        }),
        setDacVoltage: (mac, ch, voltage, bipolar = false) => request(`/api/channel/${ch}/dac`, {
            method: "POST",
            body: { voltage, bipolar },
            mac,
            admin: true,
        }),
        setDacCurrent: (mac, ch, current_mA) => request(`/api/channel/${ch}/dac`, {
            method: "POST",
            body: { current_mA },
            mac,
            admin: true,
        }),
        setAdcConfig: (mac, ch, mux, range, rate) => request(`/api/channel/${ch}/adc/config`, {
            method: "POST",
            body: { mux, range, rate },
            mac,
            admin: true,
        }),
        setDinConfig: (mac, ch, cfg) => request(`/api/channel/${ch}/din/config`, {
            method: "POST",
            body: cfg,
            mac,
            admin: true,
        }),
        setDoConfig: (mac, ch, cfg) => request(`/api/channel/${ch}/do/config`, {
            method: "POST",
            body: cfg,
            mac,
            admin: true,
        }),
        setDoState: (mac, ch, on) => request(`/api/channel/${ch}/do/set`, {
            method: "POST",
            body: { on },
            mac,
            admin: true,
        }),
        setVoutRange: (mac, ch, bipolar) => request(`/api/channel/${ch}/vout/range`, {
            method: "POST",
            body: { bipolar },
            mac,
            admin: true,
        }),
        setCurrentLimit: (mac, ch, limit8mA) => request(`/api/channel/${ch}/ilimit`, {
            method: "POST",
            body: { limit8mA },
            mac,
            admin: true,
        }),
        setAvdd: (mac, ch, select) => request(`/api/channel/${ch}/avdd`, {
            method: "POST",
            body: { select },
            mac,
            admin: true,
        }),
        setRtdConfig: (mac, ch, current_uA) => request(`/api/channel/${ch}/rtd/config`, {
            method: "POST",
            body: { current_uA },
            mac,
            admin: true,
        }),
    },
    gpio: () => request("/api/gpio"),
    gpioSetConfig: (mac, gpio, mode, pulldown) => request(`/api/gpio/${gpio}/config`, {
        method: "POST",
        body: { mode, pulldown },
        mac,
        admin: true,
    }),
    gpioSetValue: (mac, gpio, value) => request(`/api/gpio/${gpio}/set`, {
        method: "POST",
        body: { value },
        mac,
        admin: true,
    }),
    dio: () => request("/api/dio"),
    dioGet: (io) => request(`/api/dio/${io}`),
    dioSetConfig: (mac, io, mode) => request(`/api/dio/${io}/config`, {
        method: "POST",
        body: { mode },
        mac,
        admin: true,
    }),
    dioSetValue: (mac, io, value) => request(`/api/dio/${io}/set`, {
        method: "POST",
        body: { value },
        mac,
        admin: true,
    }),
    idac: () => request("/api/idac"),
    idacSetCode: (mac, ch, code) => request("/api/idac/code", {
        method: "POST",
        body: { ch, code },
        mac,
        admin: true,
    }),
    idacSetVoltage: (mac, ch, voltage) => request("/api/idac/voltage", {
        method: "POST",
        body: { ch, voltage },
        mac,
        admin: true,
    }),
    idacCalPoint: (mac, ch, code, measuredV) => request("/api/idac/cal/point", {
        method: "POST",
        body: { ch, code, measuredV },
        mac,
        admin: true,
    }),
    idacCalClear: (mac, ch) => request("/api/idac/cal/clear", {
        method: "POST",
        body: { ch },
        mac,
        admin: true,
    }),
    idacCalSave: (mac) => request("/api/idac/cal/save", {
        method: "POST",
        mac,
        admin: true,
    }),
    usbpd: () => request("/api/usbpd"),
    usbpdSelect: (mac, voltage) => request("/api/usbpd/select", {
        method: "POST",
        body: { voltage },
        mac,
        admin: true,
    }),
    usbpdRequestCaps: (mac) => request("/api/usbpd/caps", {
        method: "POST",
        mac,
        admin: true,
    }),
    hat: () => request("/api/hat"),
    hatLaStatus: () => request("/api/hat/la/status"),
    hatSetPin: (mac, pin, fn) => request("/api/hat/config", {
        method: "POST",
        body: { pin, function: fn },
        mac,
        admin: true,
    }),
    hatSetPins: (mac, pins) => request("/api/hat/config", {
        method: "POST",
        body: { pins },
        mac,
        admin: true,
    }),
    hatReset: (mac) => request("/api/hat/reset", { method: "POST", mac, admin: true }),
    hatDetect: (mac) => request("/api/hat/detect", { method: "POST", mac, admin: true }),
    hatGetPower: () => request("/api/hat/power"),
    hatSetPower: (mac, connector, enable) => request("/api/hat/power", {
        method: "POST",
        body: { connector, enable },
        mac,
        admin: true,
    }),
    hatV2Caps: () => request("/api/hat/v2/caps"),
    hatV2Rails: () => request("/api/hat/v2/rails"),
    hatV2SetRailEnable: (mac, railId, enable) => request("/api/hat/v2/rail/enable", {
        method: "POST",
        body: { railId, enable },
        mac,
        admin: true,
    }),
    hatV2SetRailVoltage: (mac, railId, voltageMv) => request("/api/hat/v2/rail/voltage", {
        method: "POST",
        body: { railId, voltageMv },
        mac,
        admin: true,
    }),
    hatV2SetLed: (mac, ledId, colorCode) => request("/api/hat/v2/led", {
        method: "POST",
        body: { ledId, colorCode },
        mac,
        admin: true,
    }),
    hatV2SetLaRoute: (mac, route) => request("/api/hat/v2/la/route", {
        method: "POST",
        body: { route },
        mac,
        admin: true,
    }),
    hatV2CalibrateStart: (mac, railId) => request("/api/hat/v2/calibrate/start", {
        method: "POST",
        body: { railId },
        mac,
        admin: true,
    }),
    hatV2CalibrateStatus: () => request("/api/hat/v2/calibrate/status"),
    hatV2CalibrateImport: (mac, railId, points) => request("/api/hat/v2/calibrate/import", {
        method: "POST",
        body: { railId, points },
        mac,
        admin: true,
    }),
    hatV2SetIoBank: (mac, dirs, ups, dns, vals) => request("/api/hat/v2/io_bank", {
        method: "POST",
        body: { dirs, ups, dns, vals },
        mac,
        admin: true,
    }),
    hatV2SetLevelShift: (mac, oe, dir) => request("/api/hat/v2/level_shift", {
        method: "POST",
        body: { oe, dir },
        mac,
        admin: true,
    }),
    hatV2SetIoVoltage: (mac, voltageMv) => request("/api/hat/v2/io_voltage", {
        method: "POST",
        body: { voltageMv },
        mac,
        admin: true,
    }),
    hatV2SetupSwd: (mac, targetVoltageMv, connector) => request("/api/hat/v2/swd/setup", {
        method: "POST",
        body: { targetVoltageMv, connector },
        mac,
        admin: true,
    }),
    hatV2LaLogEnable: (mac, enable) => request("/api/hat/v2/la/log/enable", {
        method: "POST",
        body: { enable },
        mac,
        admin: true,
    }),
    hatV2LaLogPoll: () => request("/api/hat/v2/la/log"),
    diagnostics: () => request("/api/diagnostics"),
    diagnosticsSetConfig: (mac, slot, source) => request("/api/diagnostics/config", {
        method: "POST",
        body: { slot, source },
        mac,
        admin: true,
    }),
    selftestStatus: () => request("/api/selftest"),
    selftest: () => request("/api/selftest"),
    selftestWorker: (mac, enabled) => request("/api/selftest/worker", {
        method: "POST",
        body: { enabled },
        mac,
        admin: true,
    }),
    selftestSupplies: () => request("/api/selftest/supplies"),
    selftestSuppliesCached: () => request("/api/selftest/supplies/cached"),
    selftestSupply: (rail) => request(`/api/selftest/supply/${rail}`),
    selftestCalibrate: (mac, channel) => request("/api/selftest/calibrate", {
        method: "POST",
        body: { channel },
        mac,
        admin: true,
    }),
    scripts: {
        files: (mac) => request("/api/scripts/files", { mac, admin: true }),
        storage: (mac) => request("/api/scripts/storage", { mac, admin: true }),
        download: async (mac, name) => {
            const res = await adminRawFetch(mac, `/api/scripts/files/get?name=${encodeURIComponent(name)}`);
            return res.text();
        },
        upload: async (mac, name, source) => {
            const res = await adminRawFetch(mac, `/api/scripts/files?name=${encodeURIComponent(name)}`, {
                method: "POST",
                headers: { "Content-Type": "text/plain; charset=utf-8" },
                body: source,
            });
            return (await res.json());
        },
        delete: (mac, name) => request(`/api/scripts/files?name=${encodeURIComponent(name)}`, {
            method: "DELETE",
            mac,
            admin: true,
        }),
        runFile: (mac, name) => request(`/api/scripts/run-file?name=${encodeURIComponent(name)}`, {
            method: "POST",
            mac,
            admin: true,
        }),
        eval: async (mac, source, persist) => {
            const res = await adminRawFetch(mac, `/api/scripts/eval?persist=${persist ? "true" : "false"}`, {
                method: "POST",
                headers: { "Content-Type": "text/plain; charset=utf-8" },
                body: source,
            });
            return (await res.json());
        },
        stop: (mac) => request("/api/scripts/stop", { method: "POST", mac, admin: true }),
        reset: (mac) => request("/api/scripts/reset", { method: "POST", mac, admin: true }),
        status: (mac) => request("/api/scripts/status", { mac, admin: true }),
        logs: async (mac, since) => {
            const path = since === undefined ? "/api/scripts/logs" : `/api/scripts/logs?since=${since}`;
            const res = await adminRawFetch(mac, path);
            const text = await res.text();
            const next = Number(res.headers.get("X-BugBuster-Log-Next") ?? "0");
            return { text, next: Number.isFinite(next) ? next : since ?? 0 };
        },
        autorun: (mac) => request("/api/scripts/autorun/status", { mac, admin: true }),
        enableAutorun: (mac, name) => request(`/api/scripts/autorun/enable?name=${encodeURIComponent(name)}`, {
            method: "POST",
            mac,
            admin: true,
        }),
        disableAutorun: (mac) => request("/api/scripts/autorun/disable", {
            method: "POST",
            mac,
            admin: true,
        }),
    },
    quicksetupList: () => request("/api/quicksetup"),
    quicksetupGet: (slot) => request(`/api/quicksetup/${slot}`),
    quicksetupSave: (mac, slot) => request(`/api/quicksetup/${slot}`, {
        method: "POST",
        mac,
        admin: true,
    }),
    quicksetupApply: (mac, slot) => request(`/api/quicksetup/${slot}/apply`, {
        method: "POST",
        mac,
        admin: true,
    }),
    quicksetupDelete: (mac, slot) => request(`/api/quicksetup/${slot}/delete`, {
        method: "POST",
        mac,
        admin: true,
    }),
    uartConfig: () => request("/api/uart/config"),
    uartPins: () => request("/api/uart/pins"),
    uartSetConfig: (mac, id, cfg) => request(`/api/uart/${id}/config`, {
        method: "POST",
        body: cfg,
        mac,
        admin: true,
    }),
    wifi: () => request("/api/wifi"),
    wifiScan: () => request("/api/wifi/scan"),
    wifiConnect: (mac, ssid, password) => request("/api/wifi/connect", {
        method: "POST",
        body: { ssid, password },
        mac,
        admin: true,
    }),
    mux: Object.assign(() => request("/api/mux"), {
        setSwitch: (mac, device, switchNum, closed) => request("/api/mux/switch", {
            method: "POST",
            body: { device, switch: switchNum, closed },
            mac,
            admin: true,
        }),
        setAll: (mac, states) => request("/api/mux/all", {
            method: "POST",
            body: { states },
            mac,
            admin: true,
        }),
    }),
    ioexp: Object.assign(() => request("/api/ioexp"), {
        setControl: (mac, control, on) => request("/api/ioexp/control", {
            method: "POST",
            body: { control, on },
            mac,
            admin: true,
        }),
        faults: () => request("/api/ioexp/faults"),
        setFaultConfig: (mac, auto_disable, log_events) => request("/api/ioexp/fault_config", {
            method: "POST",
            body: { auto_disable, log_events },
            mac,
            admin: true,
        }),
    }),
    lshift: {
        setOe: (mac, enabled) => request("/api/lshift/oe", {
            method: "POST",
            body: { on: enabled },
            mac,
            admin: true,
        }),
    },
    wavegen: {
        start: (mac, args) => request("/api/wavegen/start", {
            method: "POST",
            body: args,
            mac,
            admin: true,
        }),
        stop: (mac) => request("/api/wavegen/stop", { method: "POST", mac, admin: true }),
    },
    faultsClearAll: (mac) => request("/api/faults/clear", { method: "POST", mac, admin: true }),
    faultsClearChannel: (mac, channel) => request(`/api/faults/clear/${channel}`, { method: "POST", mac, admin: true }),
    faultsSetMasks: (mac, alertMask, supplyMask) => request("/api/faults/mask", {
        method: "POST",
        body: { alertMask, supplyMask },
        mac,
        admin: true,
    }),
    faultsSetChannelMask: (mac, channel, mask) => request(`/api/faults/mask/${channel}`, {
        method: "POST",
        body: { mask },
        mac,
        admin: true,
    }),
    deviceReset: (mac) => request("/api/device/reset", { method: "POST", mac, admin: true }),
    /* ---- GitHub firmware autoupdate ---- */
    update: {
        check: () => request("/api/update/check", { admin: true }),
        status: () => request("/api/update/status", { admin: true }),
        apply: (rp2040, esp32) => request("/api/update/apply", {
            method: "POST",
            admin: true,
            body: JSON.stringify({ rp2040, esp32 }),
        }),
    },
    /* ---- OTA ---- */
    ota: {
        info: () => request("/api/ota/info"),
        rollback: (mac) => request("/api/ota/rollback", {
            method: "POST",
            mac,
            admin: true,
        }),
        uploadFirmware: (mac, file, onProgress) => uploadOtaImage(mac, "/api/ota/upload", file, true, onProgress),
        uploadSpiffs: (mac, file, onProgress) => uploadOtaImage(mac, "/api/ota/uploadfs", file, false, onProgress),
    },
};
async function sha256Hex(file) {
    const buf = await file.arrayBuffer();
    const digest = await crypto.subtle.digest("SHA-256", buf);
    return Array.from(new Uint8Array(digest))
        .map((b) => b.toString(16).padStart(2, "0"))
        .join("");
}
async function uploadOtaImage(mac, path, file, withSha, onProgress) {
    const token = getCachedToken(mac);
    if (!token)
        throw new PairingRequiredError();
    const url = withSha
        ? `${path}?sha256=${await sha256Hex(file)}`
        : path;
    return await new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open("POST", url, true);
        xhr.setRequestHeader(ADMIN_TOKEN_HEADER, token);
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.responseType = "text";
        if (xhr.upload && onProgress) {
            xhr.upload.onprogress = (e) => {
                if (e.lengthComputable)
                    onProgress(e.loaded, e.total);
            };
        }
        xhr.onload = () => {
            if (xhr.status === 401) {
                clearCachedToken(mac);
                window.dispatchEvent(new CustomEvent("bb:pairing-required"));
                reject(new PairingRequiredError());
                return;
            }
            if (xhr.status < 200 || xhr.status >= 300) {
                let msg = `${xhr.status} ${xhr.statusText}`;
                try {
                    const j = JSON.parse(xhr.responseText);
                    if (j && typeof j.error === "string")
                        msg = j.error;
                }
                catch {
                    if (xhr.responseText)
                        msg = xhr.responseText;
                }
                reject(new HttpError(xhr.status, xhr.statusText, msg));
                return;
            }
            try {
                resolve(JSON.parse(xhr.responseText));
            }
            catch {
                resolve({ success: true });
            }
        };
        xhr.onerror = () => reject(new Error("Upload network error"));
        xhr.onabort = () => reject(new Error("Upload aborted"));
        xhr.send(file);
    });
}
