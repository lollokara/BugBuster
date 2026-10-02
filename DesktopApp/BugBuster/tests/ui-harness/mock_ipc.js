/*
 * Mock Tauri IPC for the BugBuster desktop frontend.
 * Evaluate BEFORE the app loads. Installs window.__TAURI__ = { core: { invoke }, event: { listen } }
 * and emulates a connected BugBuster (mainboard + optional HAT) with live-updating data.
 *
 * Scenario (set window.__BB_MOCK before this script):
 *   { hat: 'daq' | 'la' | 'none', connected: true, theme: null }
 *
 * Shapes mirror the serde structs in src/tauri_bridge.rs and src-tauri/src/*.rs.
 * Debug handles: window.__mockLog, window.__mockUnhandled, window.__mock (state + emit).
 */
(function () {
  'use strict';

  var cfg = Object.assign({ hat: 'daq', connected: true, theme: null }, window.__BB_MOCK || {});
  window.__BB_MOCK = cfg;
  window.__mockLog = [];
  window.__mockUnhandled = new Set();

  // ---------------------------------------------------------------------------
  // Real versions (Firmware/tools/firmware_version.py, check_proto_version.py)
  // ---------------------------------------------------------------------------
  var V = {
    esp32: '6.0.0', rp2040: [5, 0], p4: '3.0.0', c6: '3.0.0', proto: 12, desktop: '3.0.0', idf: 'v5.3.1',
  };

  var HAT_TYPE = cfg.hat === 'daq' ? 0x10 : cfg.hat === 'la' ? 0x01 : 0;
  var HAT_PRESENT = HAT_TYPE !== 0;

  // ---------------------------------------------------------------------------
  // Utilities
  // ---------------------------------------------------------------------------
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
  function rnd() { return Math.random() - 0.5; }
  function clamp(x, lo, hi) { return Math.max(lo, Math.min(hi, x)); }
  function int(x) { return Math.round(x); }
  function arg(a, names, dflt) {
    for (var i = 0; i < names.length; i++) {
      if (a && a[names[i]] !== undefined && a[names[i]] !== null) return a[names[i]];
    }
    return dflt;
  }
  function hash32(n) {
    n = n | 0;
    n = (n ^ 61) ^ (n >>> 16);
    n = (n + (n << 3)) | 0;
    n = n ^ (n >>> 4);
    n = Math.imul(n, 0x27d4eb2d);
    n = n ^ (n >>> 15);
    return (n >>> 0) / 4294967296;
  }
  function tsec() { return (Date.now() - S.t0) / 1000; }

  // ---------------------------------------------------------------------------
  // Event bus (Tauri event.listen emulation)
  // ---------------------------------------------------------------------------
  var listeners = {};
  var nextEventId = 1;
  function listen(event, handler) {
    (listeners[event] = listeners[event] || new Set()).add(handler);
    return Promise.resolve(function () { listeners[event].delete(handler); });
  }
  function emit(event, payload) {
    var set = listeners[event];
    if (!set) return;
    set.forEach(function (h) {
      try { h({ event: event, id: nextEventId++, payload: payload }); }
      catch (e) { console.error('[mock] listener threw for', event, e); }
    });
  }

  // ---------------------------------------------------------------------------
  // Device state
  // ---------------------------------------------------------------------------
  var S = {
    t0: Date.now(),
    connected: false,
    alert: 0x0001, // RESET latched after boot - the one standing warning
    supplyAlert: 0,
    ch: [
      { fn: 1, range: 0, rate: 9, mux: 0, dacV: 3.3, dacMa: 4.0, vin: 5.032, iin: 12.384, doOn: false, rtd: 1000, alert: 0, bipolar: false },
      { fn: 4, range: 5, rate: 8, mux: 0, dacV: 0, dacMa: 4.0, vin: 5.032, iin: 12.384, doOn: false, rtd: 1000, alert: 0, bipolar: false },
      { fn: 3, range: 0, rate: 9, mux: 0, dacV: 0, dacMa: 4.0, vin: 5.032, iin: 12.384, doOn: false, rtd: 1000, alert: 0, bipolar: false },
      { fn: 8, range: 0, rate: 4, mux: 0, dacV: 0, dacMa: 4.0, vin: 5.032, iin: 12.384, doOn: false, rtd: 1000, alert: 0, bipolar: false },
    ],
    diagSrc: [1, 2, 3, 5],
    gpio: [
      { mode: 1, output: true, input: true, pulldown: false },
      { mode: 2, output: false, input: false, pulldown: true },
      { mode: 1, output: false, input: false, pulldown: false },
      { mode: 0, output: false, input: false, pulldown: false },
      { mode: 2, output: false, input: true, pulldown: false },
      { mode: 3, output: false, input: false, pulldown: false },
      { mode: 0, output: false, input: false, pulldown: false },
      { mode: 0, output: false, input: false, pulldown: false },
      { mode: 4, output: true, input: false, pulldown: false },
      { mode: 0, output: false, input: false, pulldown: false },
      { mode: 1, output: true, input: true, pulldown: false },
      { mode: 0, output: false, input: false, pulldown: false },
    ],
    mux: [0x51, 0xa2, 0x14, 0x08],
    idac: [
      { code: 8, mid: 3.4, step: 12.6, vmin: 1.8, vmax: 5.0, name: 'LevelShift' },
      { code: 85, mid: 9.0, step: 47.2, vmin: 3.0, vmax: 15.0, name: 'V_ADJ1' },
      { code: -64, mid: 9.0, step: 47.2, vmin: 3.0, vmax: 15.0, name: 'V_ADJ2' },
    ],
    pca: { vadj1: true, vadj2: true, v15: true, logic: true, mux: true, hub: true, ef: [true, true, true, false], efFault: [false, false, false, false] },
    pdSel: 6,
    pdPdos: [
      { v: '5V', det: true, a: 3.0 }, { v: '9V', det: true, a: 3.0 }, { v: '12V', det: true, a: 3.0 },
      { v: '15V', det: true, a: 3.0 }, { v: '18V', det: false, a: 0.0 }, { v: '20V', det: true, a: 3.0 },
    ],
    wifi: { connected: true, sta_ssid: 'LabNet-5G', sta_ip: '192.168.1.58', rssi: -54, ap_ssid: 'BugBuster-A1B2C3', ap_ip: '192.168.4.1', ap_mac: '34:85:18:A1:B2:C3' },
    imon: { efuse: 1 },
    worker: true,
    cal: null,
    hatCal: null,
    lsOe: false, lsDir: false, lsRoute: 0,
    ioBank: { dirs: 0x0f, ups: 0x00, dns: 0x30 },
    hatPins: [5, 6, 7, 0],
    hatConn: [{ enabled: true, current_ma: 12.5, fault: false }, { enabled: false, current_ma: 0, fault: false }],
    hatIoMv: 3300,
    hatRails: [
      { railId: 0, enabled: true, target: 3300 },
      { railId: 1, enabled: false, target: 3300 },
      { railId: 2, enabled: true, target: 3300 },
    ],
    hatTarget: { detected: true, dpidr: 0x0bc11477 },
    quick: [
      { occ: true, name: 'Bench 3V3 + loop', hash: 0x5a }, { occ: true, name: 'Sensor sweep', hash: 0x21 },
      { occ: false, name: null, hash: 0 }, { occ: false, name: null, hash: 0 },
    ],
    scope: { active: false, mask: 0x0f, seq: 0 },
    wave: { active: false },
    rec: { count: 0 },
    la: { state: 0, total: 100000, rate: 1000000, channels: 4, trigger: 12000, has: true, stream: false },
  };

  // ---------------------------------------------------------------------------
  // Channel + device-state synthesis
  // ---------------------------------------------------------------------------
  function chValue(i) {
    var c = S.ch[i], t = tsec();
    switch (c.fn) {
      case 1: return c.dacV + rnd() * 0.0016;
      case 2: case 10: return c.dacMa + rnd() * 0.004;
      case 3: return c.vin + 0.35 * Math.sin(2 * Math.PI * t / 6 + i) + rnd() * 0.004;
      case 4: case 5: case 11: case 12: return c.iin + 1.4 * Math.sin(2 * Math.PI * t / 4 + i) + rnd() * 0.01;
      case 7: return 1097.3 + rnd() * 0.2;
      default: return 0;
    }
  }
  function chState(i) {
    var c = S.ch[i], t = tsec();
    var v = chValue(i);
    var dig = c.fn === 8 || c.fn === 9;
    var tick = Math.floor(t / 1.5);
    var dacV = c.fn === 1 ? c.dacV : 0;
    var dacOut = c.fn === 1 ? c.dacV : (c.fn === 2 || c.fn === 10) ? c.dacMa : 0;
    var fs = { 2: 25, 4: 25, 5: 25, 10: 25, 11: 25, 12: 25, 7: 4000 }[c.fn] || 12;
    return {
      function: c.fn,
      adc_raw: clamp(int(Math.abs(v) / fs * 16777215), 0, 16777215),
      adc_value: v,
      adc_range: c.range, adc_rate: c.rate, adc_mux: c.mux,
      dac_code: clamp(int((c.fn === 1 ? dacV / (c.bipolar ? 24 : 12) : dacOut / 25) * 65535), 0, 65535),
      dac_value: dacOut,
      din_state: dig ? (tick % 2 === 1) : false,
      din_counter: dig ? tick * 3 + 17 : 0,
      do_state: !!c.doOn,
      channel_alert: c.alert,
      channel_alert_mask: 0x7f,
      rtd_excitation_ua: c.fn === 7 ? c.rtd : 0,
    };
  }
  var DIAG_VAL = { 0: 0.0012, 2: 3.301, 3: 5.012, 4: 1.803, 5: 15.02, 6: 5.02, 7: -15.03, 8: 3.294, 9: 4.98, 10: 3.298, 11: 0.0004, 12: 0.0, 13: 15.01 };
  function dieTemp() { return 38.6 + 0.4 * Math.sin(tsec() / 20) + rnd() * 0.05; }
  function deviceState() {
    var t = tsec();
    return {
      spi_ok: true,
      die_temperature: dieTemp(),
      alert_status: S.alert, alert_mask: 0xff3d,
      supply_alert_status: S.supplyAlert, supply_alert_mask: 0x7f,
      live_status: 0x0000,
      channels: [0, 1, 2, 3].map(chState),
      diag: S.diagSrc.map(function (src, i) {
        var val = src === 1 ? dieTemp() : (DIAG_VAL[src] !== undefined ? DIAG_VAL[src] : 0) + rnd() * 0.002;
        var raw = src === 1 ? int((val + 273.15) * 20) : clamp(int(Math.abs(val) / 15 * 65535), 0, 65535);
        return { source: src, raw_code: raw, value: val };
      }),
      gpio: S.gpio.map(function (g, i) {
        var input = g.mode === 2 ? ((Math.floor(t / 2) + i) % 3 === 0) : g.mode === 1 ? g.output : g.input;
        return { mode: g.mode, output: g.output, input: input, pulldown: g.pulldown };
      }),
      mux_states: S.mux.slice(),
    };
  }

  // ---------------------------------------------------------------------------
  // IDAC / PCA / USB-PD / HAT shapes
  // ---------------------------------------------------------------------------
  function idacStatus() {
    return {
      present: true,
      channels: S.idac.map(function (c) {
        var tv = c.mid - c.code * c.step / 1000;
        return {
          code: c.code, target_v: tv, midpoint_v: c.mid, v_min: c.vmin, v_max: c.vmax, step_mv: c.step,
          calibrated: true, cal_points: [],
          cal_poly: [c.mid, -127 * c.step / 1000, 0.012, -0.004],
          name: c.name,
        };
      }),
    };
  }
  function pcaStatus() {
    var p = S.pca;
    return {
      present: true, input0: 0xfd, input1: 0x7f, output0: 0x3a, output1: 0x0f,
      logic_pg: true, vadj1_pg: p.vadj1, vadj2_pg: p.vadj2,
      vadj1_en: p.vadj1, vadj2_en: p.vadj2, en_15v: p.v15, en_mux: p.mux, en_usb_hub: p.hub,
      efuses: p.ef.map(function (en, i) { return { id: i + 1, enabled: en, fault: p.efFault[i] }; }),
    };
  }
  function usbpdStatus() {
    var sel = S.pdSel, volts = [5, 9, 12, 15, 18, 20];
    var v = volts[sel - 1], a = 3.0;
    return {
      present: true, attached: true, cc: 'CC1', voltage_v: v, current_a: a, power_w: v * a, pd_response: 1,
      source_pdos: S.pdPdos.map(function (p, i) {
        return { voltage: p.v, detected: p.det, max_current_a: p.a, max_power_w: volts[i] * p.a };
      }),
      selected_pdo: sel,
    };
  }
  function hatStatus() {
    if (!HAT_PRESENT) {
      return { detected: false, connected: false, hat_type: 0, detect_voltage: 0, fw_major: 0, fw_minor: 0, config_confirmed: false,
        pin_config: [0, 0, 0, 0], connectors: [], io_voltage_mv: 0, dap_connected: false, target_detected: false, target_dpidr: 0 };
    }
    var fw = HAT_TYPE === 0x10 ? [3, 0] : V.rp2040;
    return {
      detected: true, connected: true, hat_type: HAT_TYPE, detect_voltage: HAT_TYPE === 0x10 ? 2.47 : 0.82,
      fw_major: fw[0], fw_minor: fw[1], config_confirmed: true,
      pin_config: S.hatPins.slice(),
      connectors: S.hatConn.map(function (c) { return { enabled: c.enabled, current_ma: c.enabled ? c.current_ma + rnd() * 0.6 : 0, fault: c.fault }; }),
      io_voltage_mv: S.hatIoMv,
      dap_connected: HAT_TYPE === 0x01, target_detected: HAT_TYPE === 0x01 && S.hatTarget.detected, target_dpidr: HAT_TYPE === 0x01 ? S.hatTarget.dpidr : 0,
    };
  }
  function hatRails() {
    return S.hatRails.map(function (r) {
      var meas = r.enabled ? r.target + int(rnd() * 8) : 0;
      var cur = !r.enabled ? 0 : r.railId === 2 ? 41 + int(rnd() * 3) : r.railId === 0 ? 6 + int(rnd() * 2) : 0;
      return { railId: r.railId, enabled: r.enabled, voltageMv: meas, currentMa: cur, status: 0, targetMv: r.target };
    });
  }
  function hatCaps() {
    var fw = HAT_TYPE === 0x10 ? [3, 0] : V.rp2040;
    return { hwRevision: 2, flags: 31, railCount: 3, ledCount: 8, shiftedIoCount: 8, laRouteCount: 2, fwMajor: fw[0], fwMinor: fw[1] };
  }

  // ---------------------------------------------------------------------------
  // Logic analyzer synthetic capture: SPI frames on CH0 MOSI, CH1 MISO, CH2 CLK, CH3 CS
  // ---------------------------------------------------------------------------
  var LA = null;
  function buildLa() {
    var bit = 10;
    var edges = [[[0, 0]], [[0, 0]], [[0, 0]], [[0, 1]]];
    var frames = [];
    function push(ch, s, v) { var l = edges[ch]; if (l[l.length - 1][1] !== v) l.push([s, v]); }
    for (var k = 0; k < 24; k++) {
      var fs = 1500 + k * 4000;
      push(3, fs, 0);
      var mosi = [0x9f, k & 0xff, 0xa5, (0x3c ^ k) & 0xff];
      var miso = [0x00, 0xef, (k * 3) & 0xff, 0x55];
      for (var b = 0; b < 4; b++) {
        for (var j = 7; j >= 0; j--) {
          var s = fs + 20 + (b * 8 + (7 - j)) * bit;
          push(0, s, (mosi[b] >> j) & 1);
          push(1, s, (miso[b] >> j) & 1);
          push(2, s + 5, 1);
          push(2, s + 10, 0);
        }
      }
      var end = fs + 20 + 32 * bit + 10;
      push(3, end, 1); push(0, end, 0); push(1, end, 0);
      frames.push({ start: fs + 20, mosi: mosi, miso: miso });
    }
    var buckets = 256, density = new Array(buckets).fill(0);
    edges.forEach(function (l) {
      l.forEach(function (e) { density[Math.min(buckets - 1, Math.floor(e[0] / S.la.total * buckets))]++; });
    });
    LA = { edges: edges, frames: frames, density: density };
  }
  function laInfo() {
    return { channels: S.la.channels, sampleRateHz: S.la.rate, totalSamples: S.la.total, durationSec: S.la.total / S.la.rate, triggerSample: S.la.trigger };
  }
  function laView(start, end) {
    if (!LA) buildLa();
    start = clamp(Math.floor(start), 0, S.la.total);
    end = clamp(Math.floor(end), start + 1, S.la.total);
    var ct = LA.edges.map(function (list) {
      var lo = 0, hi = list.length;
      while (lo < hi) { var m = (lo + hi) >> 1; if (list[m][0] <= start) lo = m + 1; else hi = m; }
      var out = [];
      if (lo > 0) out.push(list[lo - 1]);
      for (var i = lo; i < list.length && list[i][0] <= end; i++) out.push(list[i]);
      return out;
    });
    return {
      channels: S.la.channels, sampleRateHz: S.la.rate, totalSamples: S.la.total,
      viewStart: start, viewEnd: end, triggerSample: S.la.trigger,
      channelTransitions: ct, density: LA.density, decimated: false,
    };
  }
  function laDecode(config, start, end) {
    if (!LA) buildLa();
    var type = config && config.type, out = [];
    if (type === 'spi') {
      var mosiCh = arg(config, ['mosiChannel', 'mosi_channel'], 0), misoCh = arg(config, ['misoChannel', 'miso_channel'], 1);
      LA.frames.forEach(function (f) {
        for (var b = 0; b < 4; b++) {
          var ss = f.start + b * 80, es = ss + 80;
          if (es < start || ss > end) continue;
          var h = function (n) { return '0x' + ('0' + n.toString(16).toUpperCase()).slice(-2); };
          out.push({ channel: mosiCh, row: 0, startSample: ss, endSample: es, text: h(f.mosi[b]), annType: b === 0 ? 'control' : 'data' });
          out.push({ channel: misoCh, row: 0, startSample: ss, endSample: es, text: h(f.miso[b]), annType: 'data' });
        }
      });
    } else if (type === 'uart') {
      var txCh = arg(config, ['txChannel', 'tx_channel'], 0), txt = 'BugBuster';
      for (var i = 0; i < 40; i++) {
        var s1 = 1000 + i * 1000;
        if (s1 + 870 < start || s1 > end) continue;
        var ch = txt.charCodeAt(i % txt.length);
        out.push({ channel: txCh, row: 0, startSample: s1, endSample: s1 + 870, text: '0x' + ch.toString(16).toUpperCase() + " '" + String.fromCharCode(ch) + "'", annType: 'data' });
      }
    } else if (type === 'i2c') {
      var sda = arg(config, ['sdaChannel', 'sda_channel'], 0);
      for (var k = 0; k < 12; k++) {
        var base = 2000 + k * 7500;
        if (base + 1800 < start || base > end) continue;
        out.push({ channel: sda, row: 0, startSample: base, endSample: base + 100, text: 'START', annType: 'control' });
        out.push({ channel: sda, row: 0, startSample: base + 100, endSample: base + 900, text: '0x48 W', annType: 'address' });
        out.push({ channel: sda, row: 0, startSample: base + 900, endSample: base + 1700, text: '0x1F', annType: 'data' });
      }
    }
    return out;
  }

  // ---------------------------------------------------------------------------
  // DAQ (P4 power analyzer) synthesis. Waveform is a pure function of sample index.
  // ---------------------------------------------------------------------------
  var DAQ_RATES = [10000, 50000, 100000, 250000, 1000000];
  var D = { connected: false, active: false, base: 0, startedAt: 0, rateIdx: 3, srcEn: true, vdutMv: 3300, ilimMa: 500, rangeLock: 0xff, fft: { n: 256, src: 0, win: 1 }, cal: null, logic: 1, armed: false, fired: false, ios: null };
  function daqRate() { return DAQ_RATES[D.rateIdx] || 250000; }
  function daqTotal() {
    var t = D.active ? D.base + (Date.now() - D.startedAt) / 1000 * daqRate() : D.base;
    return Math.min(Math.floor(t), 40000000);
  }
  function daqCur(s, rate) {
    var t = s / rate, p = t % 0.5, I = 6e-6 + 1.2e-6 * (hash32(s >> 4) - 0.5);
    if (p < 0.018) {
      I = 0.022 + 0.0025 * Math.sin(2 * Math.PI * p * 1400) + 0.0008 * (hash32(s) - 0.5);
    } else if (p < 0.080) {
      var q = p - 0.018, w = q % 0.0085;
      I = w < 0.0025 ? 0.17 + 0.02 * Math.sin(2 * Math.PI * w * 900) + 0.004 * (hash32(s) - 0.5) : 0.012 + 0.0006 * (hash32(s) - 0.5);
    }
    var p2 = t % 2.0;
    if (p2 > 0.2 && p2 < 0.28) I = Math.max(I, 0.088 + 0.006 * Math.sin(2 * Math.PI * t * 40) + 0.002 * (hash32(s) - 0.5));
    return I;
  }
  function daqVolt(s, I) { return 3.3 - I * 0.9 + 0.0015 * (hash32(s * 7 + 3) - 0.5); }
  function daqSource(I) { return I < 0.003 ? 0 : I > 0.1 ? 1 : 2; }
  function daqViewBytes(start, end, maxPoints) {
    var rate = daqRate(), total = daqTotal();
    start = clamp(Math.floor(start), 0, Math.max(0, total - 1));
    end = clamp(Math.floor(end), start + 1, Math.max(start + 1, total));
    var n = end - start, cols = Math.max(1, Math.min(Math.floor(maxPoints) || 1000, n, 4096));
    var K = cols > 1500 ? 6 : 10;
    var iMin = new Float32Array(cols), iMax = new Float32Array(cols), vMin = new Float32Array(cols), vMax = new Float32Array(cols);
    var pMin = new Float32Array(cols), pMax = new Float32Array(cols), didt = new Float32Array(cols);
    var src = new Uint8Array(cols), gap = new Uint8Array(cols);
    for (var c = 0; c < cols; c++) {
      var a = start + Math.floor(c * n / cols), b = start + Math.floor((c + 1) * n / cols) - 1;
      if (b < a) b = a;
      var imn = Infinity, imx = -Infinity, vmn = Infinity, vmx = -Infinity, pmn = Infinity, pmx = -Infinity, ia = 0, ib = 0;
      for (var k = 0; k < K; k++) {
        var s = a + Math.floor((b - a) * k / (K - 1 || 1));
        var I = daqCur(s, rate), Vv = daqVolt(s, I), P = I * Vv;
        if (k === 0) ia = I;
        ib = I;
        if (I < imn) imn = I; if (I > imx) imx = I;
        if (Vv < vmn) vmn = Vv; if (Vv > vmx) vmx = Vv;
        if (P < pmn) pmn = P; if (P > pmx) pmx = P;
      }
      iMin[c] = imn; iMax[c] = imx; vMin[c] = vmn; vMax[c] = vmx; pMin[c] = pmn; pMax[c] = pmx;
      didt[c] = Math.abs(ib - ia) / (((b - a + 1) / rate) || 1e-6);
      src[c] = daqSource(imx);
    }
    var markers = [];
    var firstK = Math.floor(start / rate / 0.5), lastK = Math.floor(end / rate / 0.5);
    for (var kk = firstK; kk <= lastK && markers.length < 400; kk++) {
      var s0 = Math.floor(kk * 0.5 * rate), s1 = Math.floor((kk * 0.5 + 0.08) * rate);
      if (s0 >= start && s0 < end) markers.push({ s: s0, ch: 4, edge: 1, kind: 0 });
      if (s1 >= start && s1 < end) markers.push({ s: s1, ch: 4, edge: 0, kind: 0 });
    }
    var size = 4 + 4 + 8 + 8 + 8 + 1 + 8 + 7 * (4 + cols * 4) + 2 * (4 + cols) + 4 + markers.length * 19;
    var buf = new ArrayBuffer(size), dv = new DataView(buf), u8 = new Uint8Array(buf), o = 0;
    u8.set([0x44, 0x56, 0x42, 0x31], 0); o = 4; // "DVB1"
    dv.setUint32(o, rate, true); o += 4;
    dv.setBigUint64(o, BigInt(total), true); o += 8;
    dv.setBigUint64(o, BigInt(start), true); o += 8;
    dv.setBigUint64(o, BigInt(end), true); o += 8;
    dv.setUint8(o, n > cols ? 1 : 0); o += 1;
    dv.setFloat64(o, rate, true); o += 8;
    [iMin, iMax, vMin, vMax, pMin, pMax, didt].forEach(function (arr) {
      dv.setUint32(o, arr.length, true); o += 4;
      for (var i = 0; i < arr.length; i++) { dv.setFloat32(o, arr[i], true); o += 4; }
    });
    [src, gap].forEach(function (arr) { dv.setUint32(o, arr.length, true); o += 4; u8.set(arr, o); o += arr.length; });
    dv.setUint32(o, markers.length, true); o += 4;
    markers.forEach(function (m) {
      dv.setBigUint64(o, BigInt(m.s), true); o += 8;
      dv.setBigUint64(o, BigInt(Math.round(m.s / rate * 1e6)), true); o += 8;
      dv.setUint8(o++, m.ch); dv.setUint8(o++, m.edge); dv.setUint8(o++, m.kind);
    });
    return buf;
  }
  function daqStatBlock(vals) {
    var n = vals.length, mn = Infinity, mx = -Infinity, sum = 0, sq = 0;
    vals.forEach(function (x) { mn = Math.min(mn, x); mx = Math.max(mx, x); sum += x; sq += x * x; });
    var mean = sum / n, rms = Math.sqrt(sq / n), std = Math.sqrt(Math.max(0, sq / n - mean * mean));
    return { min: mn, max: mx, mean: mean, rms: rms, std: std, count: n };
  }
  function daqAvg(start, end) {
    var rate = daqRate(), n = 600, I = [], Vs = [], P = [];
    for (var k = 0; k < n; k++) {
      var s = start + Math.floor((end - start) * k / n), i = daqCur(s, rate), v = daqVolt(s, i);
      I.push(i); Vs.push(v); P.push(i * v);
    }
    return { I: I, V: Vs, P: P };
  }
  function daqSnapshots() {
    var total = daqTotal(), rate = daqRate();
    var from = Math.max(0, total - Math.floor(rate * 0.5));
    var d = daqAvg(from, Math.max(from + 1, total));
    var elapsed = total / rate, avgP = 0.0155;
    var rangeIdx = 0;
    return {
      totalSamples: total, sampleRateHz: rate, actualRateHz: rate * 0.9998, droppedSamples: 0,
      stats: { i: daqStatBlock(d.I), v: daqStatBlock(d.V), p: daqStatBlock(d.P) },
      energy: {
        energyMwh: avgP * elapsed / 3.6, energyJ: avgP * elapsed, chargeMah: 0.0044 * elapsed / 3.6, chargeC: 0.0044 * elapsed,
        elapsedS: elapsed, lastI: d.I[d.I.length - 1], lastV: d.V[d.V.length - 1], lastP: d.P[d.P.length - 1],
      },
      fft: {
        sampleRate: rate, source: D.fft.src, window: D.fft.win,
        bins: Array.from({ length: D.fft.n }, function (_, i) {
          var base = 1 / (1 + i * 0.35), pk = (i % 8 === 0 && i > 0) ? 0.5 / (1 + i / 12) : 0;
          return Math.max(1e-6, base * 0.2 + pk + 0.01 * hash32(i + int(Date.now() / 400)));
        }),
      },
      status: {
        sampleRate: rate, overflowCount: 0, range: rangeIdx, streaming: D.active, rangeLocked: D.rangeLock !== 0xff,
        sourceEnabled: D.srcEn, vdutSet: D.vdutMv / 1000, ilimitSet: D.ilimMa, inVoltage: 20.0, inCurrent: 0.082,
        adaqOkBits: 7, fineErrPct: 0, dropFine: 0, dropCoarse: 0, fineDiagSticky: 0, framesTx: Math.floor(total / 250), bytesPerSec: int(rate * 15), fifoDropFrames: 0, ringHighWater: 38, waveIIndexLo: 0,
      },
    };
  }
  function daqStreamStatus() {
    var rate = daqRate();
    return {
      connected: D.connected, mock: false, active: D.active, totalSamples: daqTotal(), frameCount: Math.floor(daqTotal() / 250),
      sampleRateHz: rate, actualRateHz: rate * 0.9998, droppedSamples: 0, voltageSpsHz: 50000, overflow: false,
      ingestSps: D.active ? rate * 0.9998 : 0, maxSamples: 40000000, rawCap: 8000000, memUsedMb: daqTotal() * 15 / 1e6, lastError: null,
    };
  }
  function daqTrigState() {
    if (!D.ios) {
      D.ios = [];
      for (var i = 0; i < 12; i++) D.ios.push({ role: i === 3 ? 1 : i === 0 ? 2 : 0, edge: i === 3 ? 2 : 0, source: 0, thresholdV: 1.5 });
    }
    return { logic: D.logic, armed: D.armed, fired: D.fired, ios: D.ios };
  }

  // ---------------------------------------------------------------------------
  // Timers for live data
  // ---------------------------------------------------------------------------
  var timers = [];
  function startConnTimers() {
    stopConnTimers();
    timers.push(setInterval(function () { emit('device-state', deviceState()); }, 150));
    timers.push(setInterval(function () {
      if (!S.scope.active) return;
      emit('scope-data', scopeBucket());
    }, 50));
    if (HAT_PRESENT) {
      var n = 0, lines = [
        '[HAT] uart link ok rtt=1.2ms', '[LA] state=IDLE rate=1000000 ch=4', '[HAT] rail VADJ4 en=1 mv=3300',
        '[LA] usb bulk mounted', '[HAT] irq: la_done=0 swd=1', '[HAT] ping seq=%d ok',
      ];
      timers.push(setInterval(function () { emit('hat-log', lines[n % lines.length].replace('%d', String(n))); n++; }, 2500));
    }
  }
  function stopConnTimers() { timers.forEach(clearInterval); timers = []; }
  function scopeBucket() {
    var t = tsec(), vals = [
      1.65 + 1.2 * Math.sin(2 * Math.PI * t * 0.8),
      2.5 + 1.8 * Math.sign(Math.sin(2 * Math.PI * t * 0.4)) * (1 - 0.1 * Math.abs(Math.sin(t * 9))),
      5.03 + 0.35 * Math.sin(2 * Math.PI * t / 6) + rnd() * 0.01,
      0.9 * Math.sin(2 * Math.PI * t * 0.4) + 0.4 * Math.sin(2 * Math.PI * t * 2.3),
    ];
    var buf = new ArrayBuffer(58), dv = new DataView(buf);
    dv.setUint32(0, S.scope.seq++, true);
    dv.setUint32(4, Math.floor(Date.now() % 4294967295), true);
    dv.setUint16(8, 24, true);
    for (var i = 0; i < 4; i++) {
      var v = vals[i] + rnd() * 0.004;
      dv.setFloat32(10 + i * 12, v, true);
      dv.setFloat32(14 + i * 12, v - 0.01, true);
      dv.setFloat32(18 + i * 12, v + 0.01, true);
    }
    return Array.from(new Uint8Array(buf));
  }
  function runProgress(label, durMs) {
    var steps = 12, i = 0;
    var iv = setInterval(function () {
      i++;
      if (i >= steps) {
        clearInterval(iv);
        emit('desktop-ota-progress', { stage: 'done', percent: 100, message: label + ' complete' });
      } else {
        emit('desktop-ota-progress', { stage: 'uploading', percent: i / steps * 100, message: label + ': ' + int(i / steps * 512) + ' / 512 KB' });
      }
    }, durMs / steps);
  }

  // ---------------------------------------------------------------------------
  // Command handlers
  // ---------------------------------------------------------------------------
  var devices = [
    { id: 'usb:COM6', name: 'BugBuster (COM6)', transport: 'usb', address: 'COM6', serial_number: 'A1B2C3' },
    { id: 'http://192.168.1.58', name: 'bugbuster-a1b2c3.local', transport: 'http', address: 'http://192.168.1.58', serial_number: null },
  ];
  function wifiNets() {
    return [
      { ssid: 'LabNet-5G', rssi: -54, auth: 3 }, { ssid: 'LabNet-2G', rssi: -61, auth: 3 },
      { ssid: 'Guest', rssi: -72, auth: 0 }, { ssid: 'FritzBox 7590', rssi: -80, auth: 3 },
    ];
  }
  function releases() {
    return [{
      tag: 'v6.0.0', publishedAt: '2026-09-12T09:30:00Z', manifestBuildId: 'a1b2c3d4', commit: 'a1b2c3d',
      rp2040Version: (HAT_TYPE === 0x10 ? [3, 0] : V.rp2040).join('.'), rp2040Url: 'https://example.invalid/bugbuster_hat.uf2', rp2040Size: 188416, rp2040Sha256: '00'.repeat(32), rp2040Crc32: 0x1234abcd,
      esp32Version: V.esp32, esp32Url: 'https://example.invalid/firmware.bin', esp32Size: 1572864, esp32Sha256: '11'.repeat(32),
      spiffsVersion: V.esp32, spiffsUrl: 'https://example.invalid/spiffs.bin', spiffsSize: 1048576, spiffsSha256: '22'.repeat(32),
    }];
  }
  function connStatus() {
    return { mode: 'Usb', port_or_url: 'COM6', device_info: { spi_ok: true, silicon_rev: 2, silicon_id0: 0x0a3b, silicon_id1: 0x4416, fw_version: V.esp32, proto_version: V.proto, mac_address: '34:85:18:A1:B2:C3' }, la_selector: null };
  }
  function nul() { return null; }
  function ownerSlots() {
    var out = [];
    for (var i = 0; i < 16; i++) {
      var held = i === 12 || i === 14;
      out.push({ slot: i, kind: held ? 1 : 0, session_id: held ? 1 : 0, display_hash: held ? 0x9e3779b9 + i : 0, lease_until_ms: held ? 30000 + i : 0 });
    }
    return out;
  }
  function parseRails(rail, mv, en) {
    var r = S.hatRails.filter(function (x) { return x.railId === rail; })[0];
    if (r) { if (mv !== undefined) r.target = mv; if (en !== undefined) r.enabled = en; }
    return hatRails();
  }

  var H = {
    // --- discovery / connection
    js_log: nul,
    discover_devices: function () {
      if (!cfg.connected) return [];
      devices.forEach(function (d, i) { setTimeout(function () { emit('device-found', d); }, 120 + i * 220); });
      return devices;
    },
    connect_device: function (a) {
      var id = arg(a, ['deviceId', 'device_id']);
      if (!cfg.connected) throw 'Connection failed: device not responding';
      var dev = devices.filter(function (d) { return d.id === id; })[0] || devices[0];
      S.connected = true;
      var st = connStatus();
      if (dev.transport === 'http') { st.mode = 'Http'; st.port_or_url = dev.address; }
      emit('connection-status', st);
      emit('device-state', deviceState());
      startConnTimers();
      return null;
    },
    disconnect_device: function () {
      S.connected = false; S.scope.active = false; stopConnTimers();
      emit('connection-status', { mode: 'Disconnected', port_or_url: '', device_info: null, la_selector: null });
      return null;
    },

    // --- channel configuration
    set_channel_function: function (a) {
      var c = S.ch[arg(a, ['channel'], 0)], fn = arg(a, ['function'], 0);
      if (!c) return null; c.fn = fn;
      if (fn === 4 || fn === 5 || fn === 11 || fn === 12) c.range = 5;
      if (fn === 7) c.rate = 4;
      return null;
    },
    set_dac_voltage: function (a) {
      var c = S.ch[arg(a, ['channel'], 0)]; if (!c) return null;
      c.dacV = arg(a, ['voltage'], 0); c.bipolar = !!arg(a, ['bipolar'], false); return null;
    },
    set_dac_current: function (a) { var c = S.ch[arg(a, ['channel'], 0)]; if (c) c.dacMa = arg(a, ['currentMa', 'current_ma'], 0); return null; },
    set_adc_config: function (a) {
      var c = S.ch[arg(a, ['channel'], 0)]; if (!c) return null;
      c.mux = arg(a, ['mux'], c.mux); c.range = arg(a, ['range'], c.range); c.rate = arg(a, ['rate'], c.rate); return null;
    },
    set_rtd_config: function (a) { var c = S.ch[arg(a, ['channel'], 0)]; if (c) c.rtd = arg(a, ['current'], 1) ? 1000 : 500; return null; },
    set_din_config: nul,
    set_do_config: nul,
    set_do_state: function (a) { var c = S.ch[arg(a, ['channel'], 0)]; if (c) c.doOn = !!arg(a, ['on'], false); return null; },
    set_vout_range: function (a) { var c = S.ch[arg(a, ['channel'], 0)]; if (c) c.bipolar = !!arg(a, ['bipolar'], false); return null; },
    set_gpio_config: function (a) {
      var g = S.gpio[arg(a, ['gpio'], 0)]; if (!g) return null;
      g.mode = arg(a, ['mode'], g.mode); g.pulldown = !!arg(a, ['pulldown'], false); return null;
    },
    set_gpio_value: function (a) { var g = S.gpio[arg(a, ['gpio'], 0)]; if (g) g.output = !!arg(a, ['value'], false); return null; },
    set_uart_config: nul,
    set_diag_config: function (a) { var s = arg(a, ['slot'], 0); if (s >= 0 && s < 4) S.diagSrc[s] = arg(a, ['source'], 0); return null; },
    clear_all_alerts: function () { S.alert = 0; S.supplyAlert = 0; S.ch.forEach(function (c) { c.alert = 0; }); return null; },
    clear_channel_alert: function (a) { var c = S.ch[arg(a, ['channel'], 0)]; if (c) c.alert = 0; return null; },
    device_reset: nul,

    // --- MUX
    mux_set_all: function (a) { var s = arg(a, ['states'], []); for (var i = 0; i < 4 && i < s.length; i++) S.mux[i] = s[i] & 0xff; return null; },
    mux_get_all: function () { return S.mux.slice(); },
    mux_set_switch: function (a) {
      var d = arg(a, ['device'], 0), sw = arg(a, ['switchNum', 'switch_num'], 0), on = !!arg(a, ['state'], false);
      if (on) S.mux[d] |= (1 << sw); else S.mux[d] &= ~(1 << sw);
      S.mux[d] &= 0xff; return null;
    },

    // --- streams
    start_adc_stream: nul, stop_adc_stream: nul,
    start_scope_stream: function (a) { S.scope.active = true; S.scope.mask = arg(a, ['chMask', 'ch_mask'], 0x0f); return null; },
    stop_scope_stream: function () { S.scope.active = false; return null; },
    start_wavegen: function () { S.wave.active = true; return null; },
    stop_wavegen: function () { S.wave.active = false; return null; },
    set_lshift_oe: function (a) { S.lsOe = !!arg(a, ['on'], false); return null; },

    // --- wifi
    wifi_get_status: function () { return S.wifi; },
    wifi_connect: function (a) { S.wifi.connected = true; S.wifi.sta_ssid = arg(a, ['ssid'], S.wifi.sta_ssid); return true; },
    wifi_scan: wifiNets,
    wifi_forget: function () { S.wifi.connected = false; S.wifi.sta_ssid = ''; S.wifi.sta_ip = ''; return true; },
    wifi_set_ap_password: function () { return { applied: true, persisted: true }; },

    // --- firmware / OTA / app update
    get_firmware_info: function () {
      return { fwVersion: V.esp32, protoVersion: V.proto, buildDate: 'Sep 12 2026 09:14:02', idfVersion: V.idf, partition: 'ota_0', nextPartition: 'ota_1' };
    },
    ota_upload_firmware: function () { runProgress('Firmware', 3000); return 'Uploaded 1572864 bytes over USB'; },
    ota_upload_daq: function () { runProgress('DAQ image', 3000); return 'DAQ p4 updated - now running ' + V.p4; },
    fetch_github_releases: releases,
    start_desktop_spiffs_ota: function () { runProgress('SPIFFS', 2500); return null; },
    start_desktop_ota: function () { runProgress('Firmware', 3500); return null; },
    check_app_update: function () { return { available: false, version: V.desktop, current_version: V.desktop, notes: '', is_nightly: false }; },
    apply_app_update: nul,
    list_desktop_releases: function () {
      return [
        { tag: 'v3.0.0', name: 'BugBuster Desktop 3.0.0', version: '3.0.0', prerelease: false, publishedAt: '2026-09-10T12:00:00Z', installerUrl: 'https://example.invalid/bugbuster_3.0.0_x64-setup.exe', installerSize: 18874368 },
        { tag: 'v2.9.0', name: 'BugBuster Desktop 2.9.0', version: '2.9.0', prerelease: false, publishedAt: '2026-08-02T12:00:00Z', installerUrl: 'https://example.invalid/bugbuster_2.9.0_x64-setup.exe', installerSize: 18530304 },
      ];
    },
    install_desktop_version: nul,
    'plugin:dialog|open': nul,

    // --- IDAC / self-test / e-fuse / quick setups
    idac_get_status: idacStatus,
    idac_set_code: function (a) { var c = S.idac[arg(a, ['channel'], 0)]; if (c) c.code = clamp(arg(a, ['code'], 0), -127, 127); return null; },
    idac_set_voltage: function (a) {
      var c = S.idac[arg(a, ['channel'], 0)]; if (!c) return null;
      c.code = clamp(int((c.mid - arg(a, ['voltage'], c.mid)) * 1000 / c.step), -127, 127); return null;
    },
    selftest_status: function () {
      var cal = { status: 0, channel: 0, points: 0, lastVoltageV: -1, errorMv: 0 };
      if (S.cal) {
        var pts = Math.min(100, Math.floor((Date.now() - S.cal.t) / 120));
        cal = { status: pts >= 100 ? 2 : 1, channel: S.cal.ch, points: pts, lastVoltageV: 3.0 + pts * 0.09, errorMv: 3.4 };
      }
      return { boot: { ran: true, passed: true, vadj1V: 5.012, vadj2V: 11.987, vlogicV: 3.301 }, cal: cal, workerEnabled: S.worker, supplyMonitorActive: S.worker };
    },
    selftest_worker_get: function () { return S.worker; },
    selftest_worker_set: function (a) { S.worker = !!arg(a, ['enabled'], false); return S.worker; },
    selftest_supplies_cached: function () {
      return { available: true, timestampMs: Date.now() % 4000000000, rails: [
        { rail: 0, name: 'VADJ1', voltageV: 5.012 + rnd() * 0.004 },
        { rail: 1, name: 'VADJ2', voltageV: 11.987 + rnd() * 0.006 },
        { rail: 2, name: 'VLOGIC', voltageV: 3.301 + rnd() * 0.002 },
      ] };
    },
    selftest_auto_calibrate: function (a) { S.cal = { t: Date.now(), ch: arg(a, ['channel'], 0) }; return { status: 1, channel: S.cal.ch, points: 0, lastVoltageV: -1, errorMv: 0 }; },
    efuse_imon_get: function () {
      var e = S.imon.efuse;
      var on = e > 0 && S.pca.ef[e - 1];
      var mA = on ? 12.4 + rnd() * 0.8 : 0;
      return { result: 'ok', efuse: e, valid: e > 0, saturated: false, efuseOn: !!on, imonV: mA / 1000 * 0.9, currentMa: mA };
    },
    efuse_imon_set: function (a) { S.imon.efuse = arg(a, ['efuse'], 0); return H.efuse_imon_get(); },
    quicksetup_list: function () {
      return { supported: true, slots: S.quick.map(function (q, i) { return { index: i, occupied: q.occ, summaryHash: q.hash }; }) };
    },
    quicksetup_get: function (a) {
      var i = arg(a, ['slot'], 0), q = S.quick[i] || S.quick[0];
      var json = JSON.stringify({ name: q.name, channels: [{ function: 1 }, { function: 4 }, { function: 3 }, { function: 8 }] });
      return { slot: i, json: json, name: q.name, byteLen: json.length };
    },
    quicksetup_save: function (a) {
      var i = arg(a, ['slot'], 0), q = S.quick[i]; if (q) { q.occ = true; q.name = q.name || 'Saved setup'; q.hash = 0x42; }
      return H.quicksetup_get(a);
    },
    quicksetup_apply: function (a) { return { slot: arg(a, ['slot'], 0), status: 0, ok: true, message: 'applied' }; },
    quicksetup_delete: function (a) {
      var i = arg(a, ['slot'], 0), q = S.quick[i]; if (q) { q.occ = false; q.name = null; q.hash = 0; }
      return { slot: i, status: 0, ok: true, message: 'deleted' };
    },

    // --- PCA / USB-PD
    pca_get_status: pcaStatus,
    pca_set_control: function (a) {
      var c = arg(a, ['control'], 0), on = !!arg(a, ['on'], false), p = S.pca;
      if (c === 0) p.vadj1 = on; else if (c === 1) p.vadj2 = on; else if (c === 2) p.v15 = on; else if (c === 3) p.logic = on;
      else if (c === 4) p.hub = on; else if (c >= 5 && c <= 8) p.ef[c - 5] = on;
      return null;
    },
    usbpd_get_status: usbpdStatus,
    usbpd_select_pdo: function (a) { var v = arg(a, ['voltage'], 6); if (S.pdPdos[v - 1] && S.pdPdos[v - 1].det) S.pdSel = v; return null; },

    // --- HAT
    hat_get_status: hatStatus,
    hat_set_pin: function (a) { var p = arg(a, ['pin'], 0); if (p >= 0 && p < 4) S.hatPins[p] = arg(a, ['function'], 0); return null; },
    hat_reset: nul,
    hat_set_power: function (a) { var c = S.hatConn[arg(a, ['connector'], 0)]; if (c) c.enabled = !!arg(a, ['enable'], false); return null; },
    hat_set_io_voltage: function (a) { S.hatIoMv = arg(a, ['voltageMv', 'voltage_mv'], 3300); return null; },
    hat_setup_swd: nul,
    hat_detect_target: function () { return { detected: S.hatTarget.detected, dpidr: S.hatTarget.dpidr }; },
    hat_calibrate_start: function (a) { S.hatCal = { t: Date.now(), rail: arg(a, ['railId', 'rail_id'], 1) }; return 0; },
    hat_calibrate_status: function () {
      var st = { state: 0, progress: 0, railId: 0, lastError: 0, persistState: 0, stage: 0, point: 0, code: 0, measuredMv: -1, minMv: -1, maxMv: -1, maxGapMv: -1, maxErrorMv: -1, validationFlags: 0 };
      if (S.hatCal) {
        var pct = Math.min(100, Math.floor((Date.now() - S.hatCal.t) / 60));
        st = { state: pct >= 100 ? 2 : 1, progress: pct, railId: S.hatCal.rail, lastError: 0, persistState: pct >= 100 ? 2 : 0, stage: Math.min(3, Math.floor(pct / 34)), point: Math.floor(pct / 10), code: -20 + Math.floor(pct / 5), measuredMv: 3300 + pct * 20, minMv: 3300, maxMv: 5300, maxGapMv: 40, maxErrorMv: 6, validationFlags: 0 };
      }
      return st;
    },
    hat_calibrate_import: nul,
    hat_set_io_bank: function (a) { S.ioBank = { dirs: arg(a, ['dirs'], 0), ups: arg(a, ['ups'], 0), dns: arg(a, ['dns'], 0) }; return null; },
    hat_set_level_shift: function (a) { S.lsOe = !!arg(a, ['oe'], false); S.lsDir = !!arg(a, ['dir'], false); return { oe: S.lsOe, dir: S.lsDir }; },
    hat_set_rail_voltage: function (a) { return parseRails(arg(a, ['railId', 'rail_id'], 0), arg(a, ['voltageMv', 'voltage_mv'], 3300)); },
    hat_get_caps: function () { if (!HAT_PRESENT) throw 'No HAT detected'; return hatCaps(); },
    hat_get_rail_status: function () { if (!HAT_PRESENT) throw 'No HAT detected'; return hatRails(); },
    hat_set_rail_enable: function (a) { return parseRails(arg(a, ['railId', 'rail_id'], 0), undefined, !!arg(a, ['enable'], false)); },
    hat_la_set_route: function (a) { S.lsRoute = arg(a, ['route'], 0); return S.lsRoute; },
    hat_la_log_enable: nul, hat_la_usb_reset: nul,
    hat_la_log_get: function () { return ['[LA] state=IDLE', '[LA] usb bulk mounted', '[HAT] uart link ok rtt=1.2ms']; },

    // --- files / recording / board profile
    pick_save_file: nul, pick_png_save_file: nul, pick_json_save_file: nul, save_scope_png: nul, write_text_file: nul,
    start_recording: function () { S.rec.count = 0; return null; },
    stop_recording: function () { return S.rec.count || 1240; },
    export_bbsc_to_csv: function () { return 1240; },
    export_config: nul, import_config: nul, pick_config_save_file: nul, pick_config_open_file: nul,
    save_board_profile: function () { return 'C:\\Users\\bench\\board_profile.json'; },
    pick_profile_save_path: nul,
    set_pin_drive_strength: nul,
    set_efuse_config: function () { throw 'e-fuse software current limit is not enforced by the firmware; saved to the board profile only'; },

    // --- IO ownership
    io_claim: function (a) { var n = (arg(a, ['slots'], []) || []).length; var out = [n]; for (var i = 0; i < n; i++) out.push(0); return out; },
    io_release: nul,
    io_owner_status: ownerSlots,
    io_force_release: nul,

    // --- Logic analyzer
    la_configure: function (a) {
      S.la.channels = arg(a, ['channels'], 4); S.la.rate = arg(a, ['rateHz', 'rate_hz'], 1000000); S.la.total = arg(a, ['depth'], 100000); return null;
    },
    la_set_trigger: nul,
    la_arm: function () {
      S.la.state = 1;
      setTimeout(function () { S.la.state = 3; emit('la-done', []); }, 900);
      return null;
    },
    la_force: function () { S.la.state = 3; setTimeout(function () { emit('la-done', []); }, 100); return null; },
    la_stop: function () { S.la.state = 0; S.la.stream = false; return null; },
    la_get_status: function () { return { state: S.la.state, channels: S.la.channels, samplesCaptured: S.la.state === 3 ? S.la.total : 0, totalSamples: S.la.total, actualRateHz: S.la.rate }; },
    la_get_view: function (a) {
      return laView(arg(a, ['startSample', 'start_sample'], 0), arg(a, ['endSample', 'end_sample'], S.la.total));
    },
    la_load_raw: function () { S.la.has = true; return S.la.total; },
    la_get_capture_info: function () { return S.la.has ? laInfo() : null; },
    la_decode: function (a) { return laDecode(arg(a, ['config'], {}), arg(a, ['startSample', 'start_sample'], 0), arg(a, ['endSample', 'end_sample'], S.la.total)); },
    la_delete_range: function () { return laInfo(); },
    la_stream_cycle: function () { S.la.has = true; return laInfo(); },
    la_stream_usb: function () { S.la.stream = true; S.la.has = true; return null; },
    la_stream_usb_stop: function () { S.la.stream = false; return laInfo(); },
    la_stream_usb_active: function () { return S.la.stream; },
    la_stream_usb_status: function () {
      return { active: S.la.stream, totalBytes: S.la.stream ? 4194304 : 0, chunkCount: S.la.stream ? 512 : 0, sequenceMismatches: 0, invalidFrames: 0, stopReason: null, lastError: null };
    },
    la_read_uart_chunks: function () { S.la.has = true; S.la.state = 0; return laInfo(); },
    la_export_vcd_file: nul, la_export_json: nul,
    la_import_json: function () { S.la.has = true; return laInfo(); },

    // --- DAQ
    daq_check_usb: function () { return cfg.hat === 'daq'; },
    daq_connect: function () {
      if (!D.connected) { D.connected = true; D.active = true; D.base = daqRate() * 4; D.startedAt = Date.now(); }
      return true;
    },
    daq_disconnect: function () { D.connected = false; D.active = false; return null; },
    daq_stream_start: function (a) {
      D.rateIdx = clamp(arg(a, ['sampleRateIdx', 'sample_rate_idx'], D.rateIdx), 0, 4);
      D.connected = true; D.active = true; D.base = 0; D.startedAt = Date.now(); return null;
    },
    daq_stream_stop: function () { D.base = daqTotal(); D.active = false; return null; },
    daq_stream_status: daqStreamStatus,
    daq_get_view: function (a) { return daqViewBytes(arg(a, ['start'], 0), arg(a, ['end'], 1), arg(a, ['maxPoints', 'max_points'], 1000)); },
    daq_get_integral: function (a) {
      var rate = daqRate(), s = arg(a, ['start'], 0), e = Math.max(s + 1, arg(a, ['end'], s + 1)), d = daqAvg(s, e);
      var avg = function (x) { return x.reduce(function (p, q) { return p + q; }, 0) / x.length; };
      var dur = (e - s) / rate, aI = avg(d.I), aV = avg(d.V), aP = avg(d.P);
      return { start: s, end: e, durationS: dur, chargeC: aI * dur, chargeMah: aI * dur / 3.6, energyJ: aP * dur, energyMwh: aP * dur / 3.6,
        avgI: aI, avgV: aV, avgP: aP, minI: Math.min.apply(null, d.I), maxI: Math.max.apply(null, d.I), projectedMwhPerHour: aP * 1000 };
    },
    daq_get_snapshots: daqSnapshots,
    daq_set_range_lock: function (a) { D.rangeLock = arg(a, ['range'], 0xff); return null; },
    daq_set_rate: function (a) { D.rateIdx = clamp(arg(a, ['sampleRateIdx', 'sample_rate_idx'], D.rateIdx), 0, 4); return null; },
    daq_set_source: function (a) {
      D.vdutMv = arg(a, ['vdutMv', 'vdut_mv'], D.vdutMv); D.ilimMa = arg(a, ['ilimitMa', 'ilimit_ma'], D.ilimMa); D.srcEn = !!arg(a, ['enable'], true); return null;
    },
    daq_set_fft: function (a) { D.fft = { n: arg(a, ['nbins'], 256), src: arg(a, ['source'], 0), win: arg(a, ['window'], 1) }; return null; },
    daq_reset_energy: nul, daq_reset_stats: nul,
    daq_cfg_set: nul,
    daq_cal_start: function (a) { D.cal = { phase: 1, mode: arg(a, ['mode'], 0), t: 0 }; return null; },
    daq_cal_ack: function () { if (D.cal && D.cal.phase === 1) { D.cal.phase = 2; D.cal.t = Date.now(); } return null; },
    daq_cal_abort: function () { D.cal = null; return null; },
    daq_cal_status: function () {
      var c = D.cal;
      if (!c) return { phase: 0, prompt: 0, mode: 0, progress: 0, point: 0, code: 0, persist: 0, measured: 0, min: 0, max: 0, flags: 0, vcount: 24, icount: 24 };
      if (c.phase === 1) return { phase: 1, prompt: c.mode === 0 ? 1 : 2, mode: c.mode, progress: 0, point: 0, code: 0, persist: 0, measured: 0, min: 0, max: 0, flags: 0, vcount: 24, icount: 24 };
      var pct = Math.min(100, Math.floor((Date.now() - c.t) / 80));
      if (pct >= 100) c.phase = 3;
      return { phase: c.phase, prompt: 0, mode: c.mode, progress: pct, point: Math.floor(pct / 4), code: -64 + Math.floor(pct * 1.2), persist: pct >= 100 ? 2 : 0,
        measured: c.mode === 0 ? 1.8 + pct * 0.18 : pct * 0.025, min: c.mode === 0 ? 1.8 : 0, max: c.mode === 0 ? 19.8 : 2.5, flags: 0, vcount: 24, icount: 24 };
    },
    daq_measure: function () {
      var rate = daqRate(), s = daqTotal(), I = daqCur(s, rate), Vv = daqVolt(s, I);
      return { range: daqSource(I), streaming: D.active, sourceEnabled: D.srcEn, currentA: I, voltageV: Vv, powerW: I * Vv, energyMwh: s / rate * 0.0155 / 3.6 };
    },
    daq_vdut_status: function () {
      var rate = daqRate(), s = daqTotal(), I = daqCur(s, rate);
      return { enabled: D.srcEn, setpointMv: D.vdutMv, ilimitMa: D.ilimMa, voltageV: D.srcEn ? daqVolt(s, I) : 0, currentA: D.srcEn ? I : 0, fault: false };
    },
    daq_vdut_set_enable: function (a) { D.srcEn = !!arg(a, ['enabled'], true); return null; },
    daq_vdut_set_setpoint: function (a) { D.vdutMv = arg(a, ['voltageMv', 'voltage_mv'], D.vdutMv); D.ilimMa = arg(a, ['ilimitMa', 'ilimit_ma'], D.ilimMa); return null; },
    daq_set_io_role: function (a) {
      daqTrigState();
      var io = arg(a, ['io'], 1) - 1;
      if (D.ios[io]) D.ios[io] = { role: arg(a, ['role'], 0), edge: arg(a, ['edge'], 0), source: arg(a, ['source'], 0), thresholdV: arg(a, ['thresholdV', 'threshold_v'], 1.5) };
      return null;
    },
    daq_set_trig_logic: function (a) { D.logic = arg(a, ['logic'], 1); return null; },
    daq_arm: function (a) { D.armed = !!arg(a, ['armed'], false); D.fired = false; return null; },
    daq_get_trig_state: daqTrigState,
    daq_get_markers: function (a) {
      var rate = daqRate(), s = arg(a, ['start'], 0), e = arg(a, ['end'], 0), out = [];
      for (var k = Math.floor(s / rate / 0.5); k <= Math.floor(e / rate / 0.5) && out.length < 400; k++) {
        var s0 = Math.floor(k * 0.5 * rate);
        if (s0 >= s && s0 < e) out.push({ sampleIndex: s0, timestampUs: Math.round(s0 / rate * 1e6), channel: 4, edge: 1, kind: 0 });
      }
      return out;
    },
  };

  // ---------------------------------------------------------------------------
  // invoke
  // ---------------------------------------------------------------------------
  async function invoke(cmd, args) {
    args = args || {};
    window.__mockLog.push({ cmd: cmd, args: args, t: Date.now() });
    await sleep(2);
    var h = H[cmd];
    if (!h) {
      console.warn('[mock] unhandled', cmd);
      window.__mockUnhandled.add(cmd);
      return null;
    }
    return h(args);
  }

  window.__TAURI__ = { core: { invoke: invoke }, event: { listen: listen } };
  window.__mock = { state: S, daq: D, emit: emit, handlers: H, deviceState: deviceState };

  if (cfg.theme) {
    var applyTheme = function () { document.documentElement.setAttribute('data-theme', cfg.theme); };
    if (document.documentElement) applyTheme();
    document.addEventListener('DOMContentLoaded', applyTheme);
  }
})();
