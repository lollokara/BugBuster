use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Deserialize;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

const COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const MATH_COLOR: &str = "#ec4899";
const MAX_POINTS: usize = 100_000;
const STATS_WINDOW: usize = 5000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlotMode {
    Overlay,
    Stacked,
}

#[derive(Clone)]
pub struct ScopePoint {
    pub time_ms: f64,
    pub value: f32,
}

/// Parse EVT_SCOPE_DATA payload into per-channel avg values.
/// Format: bucketSeq(u32) + timestamp_ms(u32) + count(u16) + [avg(f32), min(f32), max(f32)] × 4
fn parse_scope_event(data: &[u8]) -> Option<[f32; 4]> {
    if data.len() < 58 { return None; }
    let mut pos = 10;
    let mut avg = [0f32; 4];
    for ch in 0..4 {
        avg[ch] = f32::from_le_bytes([data[pos], data[pos+1], data[pos+2], data[pos+3]]);
        pos += 12;
    }
    Some(avg)
}

/// Parse effective SPS integer from an ADC rate label like "200 SPS HR1".
fn rate_to_sps(rate_code: u8) -> u32 {
    // From ADC_RATE_OPTIONS: (code, label)
    for (c, label) in ADC_RATE_OPTIONS {
        if *c == rate_code {
            // Parse first integer (optionally with "k" suffix) from label.
            let s = label.trim();
            let mut num = String::new();
            let mut chars = s.chars().peekable();
            while let Some(&c) = chars.peek() {
                if c.is_ascii_digit() || c == '.' { num.push(c); chars.next(); } else { break; }
            }
            let val: f32 = num.parse().unwrap_or(0.0);
            let is_k = s.to_lowercase().contains("k");
            return if is_k { (val * 1000.0) as u32 } else { val as u32 };
        }
    }
    0
}

/// Return the display label for an ADC rate code (e.g. "1.2 kSPS"), or "— SPS".
fn rate_label(rate_code: u8) -> String {
    for (c, label) in ADC_RATE_OPTIONS {
        if *c == rate_code {
            // Strip any " HR"/" HR1" suffix for compact display.
            let s = label.trim();
            let compact = s.split_whitespace().take(2).collect::<Vec<_>>().join(" ");
            return compact;
        }
    }
    "— SPS".to_string()
}

/// Infer unit (V or mA) from ADC range code.
fn range_unit(range_code: u8) -> &'static str {
    // Ranges 0-7 in tauri_bridge are all voltage. But user may pick 0-25mA externally.
    // The scope always reads voltage via ADC_RANGE_OPTIONS — keep "V".
    let _ = range_code;
    "V"
}

/// Auto-scale a time delta (ms) into engineering units.
fn fmt_duration_ms(dt_ms: f64) -> String {
    let abs = dt_ms.abs();
    if abs < 1000.0 { format!("{:.2} ms", dt_ms) }
    else if abs < 60_000.0 { format!("{:.3} s", dt_ms / 1000.0) }
    else if abs < 3_600_000.0 { format!("{:.2} min", dt_ms / 60_000.0) }
    else { format!("{:.2} h", dt_ms / 3_600_000.0) }
}

/// Format elapsed ms as HH:MM:SS.s (tenths of a second).
fn fmt_elapsed(ms: f64) -> String {
    let total = (ms.max(0.0) / 100.0) as u64; // tenths of a second
    let tenths = total % 10;
    let total_s = total / 10;
    let s = total_s % 60;
    let m = (total_s / 60) % 60;
    let h = total_s / 3600;
    format!("{:02}:{:02}:{:02}.{}", h, m, s, tenths)
}

/// Format elapsed ms as HH:MM:SS (no tenths).
fn fmt_hms(ms: f64) -> String {
    let total_s = (ms.max(0.0) / 1000.0) as u64;
    let s = total_s % 60;
    let m = (total_s / 60) % 60;
    let h = total_s / 3600;
    format!("{:02}:{:02}:{:02}", h, m, s)
}

/// Truncate a path for display.
fn truncate_path(p: &str, max_len: usize) -> String {
    if p.len() <= max_len { p.to_string() }
    else {
        let tail = p.len().saturating_sub(max_len - 3);
        format!("...{}", &p[tail..])
    }
}

/// Stats over the last STATS_WINDOW samples.
#[derive(Clone, Copy, Debug, Default)]
pub struct ChStats {
    current: f32,
    min: f32,
    max: f32,
    mean: f32,
    rms: f32,
    vpp: f32,
    freq: f32,
    has_data: bool,
}

fn compute_stats(data: &[ScopePoint], t_start: f64) -> ChStats {
    let mut mn = f32::INFINITY;
    let mut mx = f32::NEG_INFINITY;
    let mut sum = 0.0f64;
    let mut sum_sq = 0.0f64;
    let mut n = 0u64;
    let mut last: Option<f32> = None;
    let mut crossings = 0u32;
    let mut prev: Option<f32> = None;
    let mut first_ms: Option<f64> = None;
    let mut last_ms: Option<f64> = None;

    // First pass: mean
    for p in data.iter().rev() {
        if p.time_ms < t_start { break; }
        sum += p.value as f64;
        n += 1;
    }
    if n == 0 {
        return ChStats::default();
    }
    let mean = (sum / n as f64) as f32;

    // Second pass: min/max/rms/zero crossings around mean
    for p in data.iter() {
        if p.time_ms < t_start { continue; }
        let v = p.value;
        if v < mn { mn = v; }
        if v > mx { mx = v; }
        let dev = (v - mean) as f64;
        sum_sq += dev * dev;
        last = Some(v);
        if first_ms.is_none() { first_ms = Some(p.time_ms); }
        last_ms = Some(p.time_ms);
        if let Some(pv) = prev {
            let a = pv - mean;
            let b = v - mean;
            if a == 0.0 && b == 0.0 {}
            else if (a <= 0.0 && b > 0.0) || (a >= 0.0 && b < 0.0) {
                crossings += 1;
            }
        }
        prev = Some(v);
    }
    let rms = (sum_sq / n as f64).sqrt() as f32;
    let freq = match (first_ms, last_ms) {
        (Some(a), Some(b)) if crossings >= 3 && b > a => {
            (crossings as f64 / 2.0) / ((b - a) / 1000.0)
        }
        _ => 0.0,
    } as f32;

    ChStats {
        current: last.unwrap_or(0.0),
        min: mn,
        max: mx,
        mean,
        rms,
        vpp: mx - mn,
        freq,
        has_data: true,
    }
}

/// Hoisted Scope UI state — lives in app-level context so signals survive
/// tab switches. Use `use_context::<ScopeUiState>()` inside `ScopeTab`.
#[derive(Clone, Copy)]
pub struct ScopeUiState {
    pub running: RwSignal<bool>,
    pub channels_en: RwSignal<[bool; 4]>,
    pub window_sec: RwSignal<f64>,
    pub y_range: RwSignal<String>,
    pub recording: RwSignal<bool>,
    pub csv_path: RwSignal<String>,
    pub sample_rate: RwSignal<u32>,
    pub cursor_x: RwSignal<Option<f64>>,
    pub scope_data: [RwSignal<Vec<ScopePoint>>; 4],
    pub sample_counter: RwSignal<u32>,
    pub render_epoch: RwSignal<u64>,
    // New (scientific redesign)
    pub recording_start_ms: RwSignal<Option<f64>>,
    pub recording_stop_ms: RwSignal<Option<f64>>,
    pub recording_freeze_label: RwSignal<Option<String>>,
    pub recording_bucket_count: RwSignal<u64>,
    pub adc_rate: [RwSignal<u8>; 4],
    pub adc_range: [RwSignal<u8>; 4],
    pub adc_mux: [RwSignal<u8>; 4],
    pub channel_labels: [RwSignal<String>; 4],
    pub y_offset: [RwSignal<f64>; 4],
    pub invert: [RwSignal<bool>; 4],
    pub plot_mode: RwSignal<PlotMode>,
    pub cursor_a: RwSignal<Option<f64>>,
    pub cursor_b: RwSignal<Option<f64>>,
    pub math_a_enabled: RwSignal<bool>,
    pub single_shot: RwSignal<bool>,
    pub channel_collapsed: [RwSignal<bool>; 4],
    pub capture_start_ms: RwSignal<Option<f64>>,
    pub timer_tick: RwSignal<u64>,
    /// Cached per-channel stats, refreshed by a throttled Effect (~500 ms).
    /// Display reads from here instead of computing stats on every view tick.
    pub cached_stats: [RwSignal<ChStats>; 4],
    pub last_draw_counter: RwSignal<u32>,
    /// True while start_scope_stream has returned Ok and the backend is
    /// actively streaming. Set/cleared by the app-level start/stop Effect so
    /// spurious re-fires (tab switches, re-mounts) become no-ops.
    pub backend_stream_active: RwSignal<bool>,
}

impl ScopeUiState {
    pub fn new() -> Self {
        Self {
            running: RwSignal::new(false),
            channels_en: RwSignal::new([true, false, false, false]),
            window_sec: RwSignal::new(30.0f64),
            y_range: RwSignal::new("auto".to_string()),
            recording: RwSignal::new(false),
            csv_path: RwSignal::new(String::new()),
            sample_rate: RwSignal::new(0u32),
            cursor_x: RwSignal::new(None),
            scope_data: std::array::from_fn(|_| RwSignal::new(Vec::new())),
            sample_counter: RwSignal::new(0u32),
            render_epoch: RwSignal::new(0u64),
            recording_start_ms: RwSignal::new(None),
            recording_stop_ms: RwSignal::new(None),
            recording_freeze_label: RwSignal::new(None),
            recording_bucket_count: RwSignal::new(0u64),
            adc_rate: std::array::from_fn(|_| RwSignal::new(3u8)),   // 20 SPS HR
            adc_range: std::array::from_fn(|_| RwSignal::new(1u8)),  // ±12V
            adc_mux: std::array::from_fn(|_| RwSignal::new(0u8)),    // LF to AGND
            channel_labels: std::array::from_fn(|i| {
                RwSignal::new(format!("CH {}", CH_NAMES[i]))
            }),
            y_offset: std::array::from_fn(|_| RwSignal::new(0.0f64)),
            invert: std::array::from_fn(|_| RwSignal::new(false)),
            plot_mode: RwSignal::new(PlotMode::Overlay),
            cursor_a: RwSignal::new(None),
            cursor_b: RwSignal::new(None),
            math_a_enabled: RwSignal::new(false),
            single_shot: RwSignal::new(false),
            channel_collapsed: std::array::from_fn(|_| RwSignal::new(false)),
            capture_start_ms: RwSignal::new(None),
            timer_tick: RwSignal::new(0u64),
            cached_stats: std::array::from_fn(|_| RwSignal::new(ChStats::default())),
            last_draw_counter: RwSignal::new(0u32),
            backend_stream_active: RwSignal::new(false),
        }
    }
}

/// Install the app-lifetime scope acquisition manager. Call ONCE from
/// `App` after `provide_context(ScopeUiState::new())`.
///
/// This centralises the two pieces of state that must survive `ScopeTab`
/// unmount/remount:
///   (1) the `scope-data` event listener — otherwise buckets arriving while
///       the tab is off-screen are lost.
///   (2) the Effect that watches `running` and issues start/stop commands —
///       otherwise the Effect is recreated on re-mount and fires spuriously,
///       producing the "restart storm" the user observes when returning to
///       the Scope tab.
pub fn install_scope_lifetime_manager(ui: ScopeUiState, device_state: ReadSignal<DeviceState>) {
    let running = ui.running;
    let channels_en = ui.channels_en;
    let scope_data = ui.scope_data;
    let sample_counter = ui.sample_counter;
    let recording = ui.recording;
    let recording_bucket_count = ui.recording_bucket_count;
    let window_sec = ui.window_sec;
    let y_offset = ui.y_offset;
    let invert = ui.invert;
    let single_shot = ui.single_shot;
    let capture_start_ms = ui.capture_start_ms;
    let adc_rate = ui.adc_rate;
    let adc_range = ui.adc_range;
    let adc_mux = ui.adc_mux;
    let backend_stream_active = ui.backend_stream_active;

    // (1) Single app-lifetime scope-data listener.
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            #[derive(Deserialize)]
            struct TauriEvt { payload: Vec<u8> }
            let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvt>(event) else { return };
            if !running.get_untracked() { return; }

            let Some(avg) = parse_scope_event(&evt.payload) else { return };
            let now = js_sys::Date::now();
            let ch_en = channels_en.get_untracked();
            let y_off = [
                y_offset[0].get_untracked(),
                y_offset[1].get_untracked(),
                y_offset[2].get_untracked(),
                y_offset[3].get_untracked(),
            ];
            let inv = [
                invert[0].get_untracked(),
                invert[1].get_untracked(),
                invert[2].get_untracked(),
                invert[3].get_untracked(),
            ];

            for ch in 0..4 {
                if !ch_en[ch] { continue; }
                let mut v = avg[ch];
                if inv[ch] { v = -v; }
                v += y_off[ch] as f32;
                scope_data[ch].update(|data| {
                    data.push(ScopePoint { time_ms: now, value: v });
                });
            }

            sample_counter.update(|c| *c += 1);
            if recording.get_untracked() {
                recording_bucket_count.update(|c| *c += 1);
            }

            if single_shot.get_untracked() {
                if let Some(t0) = capture_start_ms.get_untracked() {
                    if now - t0 >= window_sec.get_untracked() * 1000.0 {
                        single_shot.set(false);
                        running.set(false);
                    }
                }
            }

            let win = window_sec.get_untracked();
            let cutoff = now - win * 1000.0 * 1.5;
            for ch in 0..4 {
                scope_data[ch].update(|data| {
                    data.retain(|p| p.time_ms > cutoff);
                    if data.len() > MAX_POINTS { data.drain(0..data.len() - MAX_POINTS); }
                });
            }
        });
        listen("scope-data", &closure).await;
        closure.forget();
    });

    // (2) App-lifetime start/stop Effect. Survives ScopeTab unmount/remount,
    // so the backend stream is not disturbed when the user switches tabs.
    Effect::new(move || {
        let is_running = running.get();
        let ch_en = channels_en.get();
        let active = backend_stream_active.get_untracked();

        // Defense in depth: idempotent transitions only.
        if is_running == active {
            return;
        }

        if is_running {
            let has_channels = ch_en.iter().any(|&en| en);
            if !has_channels {
                log("Scope: no channels enabled");
                running.set(false);
                return;
            }
            const CH_FUNC_VIN: u8 = 3;
            let ds = device_state.get_untracked();
            let mut to_promote: Vec<u8> = Vec::new();
            for (i, en) in ch_en.iter().enumerate() {
                if !*en { continue; }
                let func = ds.channels.get(i).map(|c| c.function).unwrap_or(0);
                if func != CH_FUNC_VIN {
                    to_promote.push(i as u8);
                    let msg = format!("CH{} auto-set to VIN for scope", CH_NAMES[i]);
                    show_toast(&msg, "ok");
                    log(&msg);
                }
            }

            let rate_v = [
                adc_rate[0].get_untracked(), adc_rate[1].get_untracked(),
                adc_rate[2].get_untracked(), adc_rate[3].get_untracked(),
            ];
            let range_v = [
                adc_range[0].get_untracked(), adc_range[1].get_untracked(),
                adc_range[2].get_untracked(), adc_range[3].get_untracked(),
            ];
            let mux_v = [
                adc_mux[0].get_untracked(), adc_mux[1].get_untracked(),
                adc_mux[2].get_untracked(), adc_mux[3].get_untracked(),
            ];

            capture_start_ms.set(Some(js_sys::Date::now()));
            web_sys::console::log_1(&JsValue::from_str(
                &format!("[scope] start_scope_stream @ {:.0}", js_sys::Date::now())
            ));

            spawn_local(async move {
                #[derive(serde::Serialize)]
                struct ChFuncArgs { channel: u8, function: u8 }
                for ch_idx in to_promote {
                    let args = serde_wasm_bindgen::to_value(&ChFuncArgs {
                        channel: ch_idx, function: CH_FUNC_VIN,
                    }).unwrap();
                    let _ = invoke("set_channel_function", args).await;
                }
                sleep_ms(300).await;

                let _ = invoke("stop_scope_stream", wasm_bindgen::JsValue::NULL).await;
                sleep_ms(200).await;

                #[derive(serde::Serialize)]
                struct AdcCfgArgs { channel: u8, mux: u8, range: u8, rate: u8 }
                for (i, en) in ch_en.iter().enumerate() {
                    if *en {
                        let args = serde_wasm_bindgen::to_value(&AdcCfgArgs {
                            channel: i as u8,
                            mux: mux_v[i],
                            range: range_v[i],
                            rate: rate_v[i],
                        }).unwrap();
                        let _ = invoke("set_adc_config", args).await;
                    }
                }

                let result = invoke("start_scope_stream", wasm_bindgen::JsValue::NULL).await;
                log(&format!("Scope: stream started, result={:?}", result));
                backend_stream_active.set(true);
            });
        } else {
            capture_start_ms.set(None);
            web_sys::console::log_1(&JsValue::from_str(
                &format!("[scope] stop_scope_stream @ {:.0}", js_sys::Date::now())
            ));
            spawn_local(async move {
                let _ = invoke("stop_scope_stream", wasm_bindgen::JsValue::NULL).await;
                log("Scope: stream stopped");
                backend_stream_active.set(false);
            });
        }
    });
}

#[component]
pub fn ScopeTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let ui = use_context::<ScopeUiState>()
        .expect("ScopeUiState not provided — call provide_context(ScopeUiState::new()) in App");

    let running = ui.running.read_only();
    let set_running = ui.running.write_only();
    let channels_en = ui.channels_en.read_only();
    let set_channels_en = ui.channels_en.write_only();
    let window_sec = ui.window_sec.read_only();
    let set_window_sec = ui.window_sec.write_only();
    let y_range = ui.y_range.read_only();
    let set_y_range = ui.y_range.write_only();
    let recording = ui.recording.read_only();
    let set_recording = ui.recording.write_only();
    let csv_path = ui.csv_path.read_only();
    let set_csv_path = ui.csv_path.write_only();
    let sample_rate = ui.sample_rate.read_only();
    let set_sample_rate = ui.sample_rate.write_only();
    let cursor_x = ui.cursor_x.read_only();
    let set_cursor_x = ui.cursor_x.write_only();

    let scope_data = ui.scope_data;
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    let sample_counter = ui.sample_counter;
    let recording_start_ms = ui.recording_start_ms;
    let recording_stop_ms = ui.recording_stop_ms;
    let recording_freeze_label = ui.recording_freeze_label;
    let recording_bucket_count = ui.recording_bucket_count;
    let adc_rate = ui.adc_rate;
    let adc_range = ui.adc_range;
    let adc_mux = ui.adc_mux;
    let channel_labels = ui.channel_labels;
    let y_offset = ui.y_offset;
    let invert = ui.invert;
    let plot_mode = ui.plot_mode;
    let cursor_a = ui.cursor_a;
    let cursor_b = ui.cursor_b;
    let math_a_enabled = ui.math_a_enabled;
    let single_shot = ui.single_shot;
    let channel_collapsed = ui.channel_collapsed;
    let capture_start_ms = ui.capture_start_ms;
    let timer_tick = ui.timer_tick;
    let cached_stats = ui.cached_stats;
    let last_draw_counter = ui.last_draw_counter;

    // Claim a fresh render epoch — any prior-mount render loops running on
    // the same canvas will notice their epoch is stale and exit.
    let render_epoch = ui.render_epoch;
    let my_epoch = render_epoch.get_untracked().wrapping_add(1);
    render_epoch.set(my_epoch);

    on_cleanup(move || {
        render_epoch.set(render_epoch.get_untracked().wrapping_add(1));
    });

    // NOTE: The "scope-data" listener and the start/stop Effect that watches
    // `running` are installed at app-level in `install_scope_lifetime_manager`
    // so they survive ScopeTab unmount/remount. Previously each remount
    // registered a fresh listener (duplicate signal writes) and a fresh
    // Effect that fired immediately and produced a backend start/stop storm.

    // Recording / elapsed timer tick — 100 ms so the display updates cleanly.
    spawn_local(async move {
        loop {
            sleep_ms(100).await;
            if render_epoch.get_untracked() != my_epoch { break; }
            timer_tick.update(|t| *t = t.wrapping_add(1));
            // Clear stop-freeze after 3 s.
            if let Some(stop_ms) = recording_stop_ms.get_untracked() {
                if js_sys::Date::now() - stop_ms >= 3000.0 {
                    recording_stop_ms.set(None);
                    recording_freeze_label.set(None);
                }
            }
        }
    });

    // Stats refresh loop — throttled to 500 ms to keep the main thread
    // responsive. Previously stats (Min/Max/Mean/RMS/Vpp/Freq) were recomputed
    // inside every view render across 4 channels, which scanned up to
    // STATS_WINDOW points per tick and starved the WASM thread (A5).
    // Skip the scan when idle (not running and no new buckets) to drop CPU.
    spawn_local(async move {
        let mut last_seen_counter: u32 = 0;
        loop {
            sleep_ms(500).await;
            if render_epoch.get_untracked() != my_epoch { break; }
            let sc = sample_counter.get_untracked();
            if !running.get_untracked() && sc == last_seen_counter {
                continue;
            }
            last_seen_counter = sc;
            let now = js_sys::Date::now();
            let ts = now - window_sec.get_untracked() * 1000.0;
            let ch_en = channels_en.get_untracked();
            for ch in 0..4 {
                if !ch_en[ch] {
                    cached_stats[ch].set(ChStats::default());
                    continue;
                }
                let stats = scope_data[ch].with_untracked(|d| compute_stats(d, ts));
                cached_stats[ch].set(stats);
            }
        }
    });

    // Rate counter + auto-restart.
    let zero_count = std::cell::Cell::new(0u32);
    let restart_count = std::cell::Cell::new(0u32);
    let auto_restart_disabled = std::cell::Cell::new(false);
    const MAX_RESTARTS: u32 = 3;
    spawn_local(async move {
        loop {
            sleep_ms(1000).await;
            if render_epoch.get_untracked() != my_epoch { break; }
            if !running.get_untracked() {
                zero_count.set(0);
                restart_count.set(0);
                auto_restart_disabled.set(false);
                set_sample_rate.set(0);
                continue;
            }
            let count = sample_counter.get_untracked();
            sample_counter.set(0);
            set_sample_rate.set(count);

            if count == 0 {
                let z = zero_count.get() + 1;
                zero_count.set(z);
                if z >= 3 && !auto_restart_disabled.get() {
                    zero_count.set(0);
                    let restarts = restart_count.get() + 1;
                    restart_count.set(restarts);
                    if restarts <= MAX_RESTARTS {
                        log(&format!("Scope: 0 SPS for 3s, restarting stream ({}/{})...", restarts, MAX_RESTARTS));
                        set_running.set(false);
                        sleep_ms(500).await;
                        if render_epoch.get_untracked() != my_epoch { break; }
                        if running.get_untracked() { continue; }
                        set_running.set(true);
                    } else {
                        auto_restart_disabled.set(true);
                        let ch_en_arr = channels_en.get_untracked();
                        let first_ch = (0..4).find(|&i| ch_en_arr[i]).unwrap_or(0);
                        let msg = format!(
                            "Scope stalled on CH{} — check channel function (VIN channels can't be streamed)",
                            CH_NAMES[first_ch]
                        );
                        show_toast(&msg, "err");
                        log(&msg);
                    }
                }
            } else {
                zero_count.set(0);
                restart_count.set(0);
                auto_restart_disabled.set(false);
            }
        }
    });

    // Start/stop stream Effect moved to install_scope_lifetime_manager (app.rs).
    // See the note above near the scope-data listener comment.

    // Render loop — 20 FPS when running (save main-thread budget), 30 FPS idle.
    // When idle with nothing to redraw, back off to 5 Hz to keep CPU low.
    spawn_local(async move {
        let mut first_iter = true;
        let mut last_perf_log = 0.0f64;
        loop {
            // Snapshot once per iteration; the same values gate both the
            // pre-sleep idle backoff and the post-sleep redraw skip.
            let sc = sample_counter.get_untracked();
            let ldc = last_draw_counter.get_untracked();
            let cursor_active = cursor_x.get_untracked().is_some()
                || cursor_a.get_untracked().is_some()
                || cursor_b.get_untracked().is_some();
            let is_running = running.get_untracked();
            let idle = !is_running && sc == ldc && !cursor_active;
            // Background CPU win: a hidden Scope tab (running=false, no
            // pending redraw) used to wake at 30 Hz; now backs off to 5 Hz.
            let delay = if is_running { 50 } else if idle { 200 } else { 33 };
            sleep_ms(delay).await;

            if render_epoch.get_untracked() != my_epoch { break; }

            // Skip redraw when nothing changed AND no interactive cursor moved.
            if idle {
                continue;
            }
            last_draw_counter.set(sc);

            let draw_start = js_sys::Date::now();
            let Some(canvas) = canvas_ref.get() else { continue };
            let canvas: HtmlCanvasElement = canvas.into();
            if first_iter {
                canvas.set_width(0);
                canvas.set_height(0);
                first_iter = false;
            }
            let Some(ctx) = canvas.get_context("2d").ok().flatten() else { continue };
            let ctx: CanvasRenderingContext2d = ctx.unchecked_into();

            let dpr = web_sys::window().unwrap().device_pixel_ratio();
            let rect = canvas.get_bounding_client_rect();
            let w = rect.width();
            let h = rect.height();
            let cw = (w * dpr) as u32;
            let ch_px = (h * dpr) as u32;
            if canvas.width() != cw { canvas.set_width(cw); }
            if canvas.height() != ch_px { canvas.set_height(ch_px); }
            ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();

            // Clear — darker background for scientific view.
            ctx.set_fill_style_str("#0a0f1e");
            ctx.fill_rect(0.0, 0.0, w, h);

            let ch_en = channels_en.get_untracked();
            let now = js_sys::Date::now();
            let win_sec = window_sec.get_untracked();
            let win_ms = win_sec * 1000.0;
            let t_start = now - win_ms;
            let yr = y_range.get_untracked();
            let mode = plot_mode.get_untracked();
            let math_on = math_a_enabled.get_untracked();

            // Compute Y range (used for Overlay; Stacked computes per-band)
            let (y_min_all, y_max_all) = if yr == "auto" {
                let mut mn = f64::MAX;
                let mut mx = f64::MIN;
                for i in 0..4 {
                    if !ch_en[i] { continue; }
                    scope_data[i].with_untracked(|data| {
                        for p in data.iter().rev().take(STATS_WINDOW) {
                            if p.time_ms < t_start { break; }
                            let v = p.value as f64;
                            if v < mn { mn = v; }
                            if v > mx { mx = v; }
                        }
                    });
                }
                if mn == f64::MAX { (0.0, 1.0) }
                else {
                    let margin = (mx - mn).max(0.01) * 0.15;
                    (mn - margin, mx + margin)
                }
            } else {
                match yr.as_str() {
                    "0-12" => (0.0, 12.0), "pm12" => (-12.0, 12.0),
                    "0-625m" => (0.0, 0.625), "0-25m" => (0.0, 25.0),
                    _ => (0.0, 12.0),
                }
            };

            // Plot area margins
            let ml = 65.0; let mr = 15.0; let mt = 18.0; let mb = 30.0;
            let pw = w - ml - mr;
            let ph = h - mt - mb;

            // Per-channel bands for Stacked mode.
            let active_channels: Vec<usize> = (0..4).filter(|&i| ch_en[i]).collect();
            let band_count = active_channels.len().max(1);
            let band_h = ph / band_count as f64;

            // -- Grid (10 x 8 major + minor subdivisions) --
            // Minor verticals (finer dotted), 5 per major division
            let v_majors = 10;
            let h_majors = 8;
            // Major gridlines
            ctx.set_stroke_style_str("rgba(148,163,184,0.16)");
            ctx.set_line_width(1.0);
            for j in 0..=h_majors {
                let frac = j as f64 / h_majors as f64;
                let y = mt + frac * ph;
                ctx.begin_path();
                ctx.move_to(ml, y);
                ctx.line_to(w - mr, y);
                ctx.stroke();
            }
            for j in 0..=v_majors {
                let frac = j as f64 / v_majors as f64;
                let x = ml + frac * pw;
                ctx.begin_path();
                ctx.move_to(x, mt);
                ctx.line_to(x, h - mb);
                ctx.stroke();
            }
            // Minor gridlines
            ctx.set_stroke_style_str("rgba(148,163,184,0.08)");
            ctx.set_line_width(0.5);
            for j in 0..v_majors * 5 {
                if j % 5 == 0 { continue; }
                let frac = j as f64 / (v_majors as f64 * 5.0);
                let x = ml + frac * pw;
                ctx.begin_path();
                ctx.move_to(x, mt);
                ctx.line_to(x, h - mb);
                ctx.stroke();
            }
            for j in 0..h_majors * 5 {
                if j % 5 == 0 { continue; }
                let frac = j as f64 / (h_majors as f64 * 5.0);
                let y = mt + frac * ph;
                ctx.begin_path();
                ctx.move_to(ml, y);
                ctx.line_to(w - mr, y);
                ctx.stroke();
            }

            // Y axis labels
            ctx.set_font("10px 'JetBrains Mono', monospace");
            ctx.set_text_align("right");
            ctx.set_fill_style_str("rgba(148,163,184,0.55)");
            match mode {
                PlotMode::Overlay => {
                    let y_span = y_max_all - y_min_all;
                    for j in 0..=h_majors {
                        let frac = j as f64 / h_majors as f64;
                        let y = mt + frac * ph;
                        let val = y_max_all - frac * y_span;
                        let _ = ctx.fill_text(&format!("{:.2}", val), ml - 8.0, y + 4.0);
                    }
                }
                PlotMode::Stacked => {
                    for (bi, &c) in active_channels.iter().enumerate() {
                        // Compute per-channel band autoscale
                        let (bmin, bmax) = if yr == "auto" {
                            let mut mn = f64::MAX;
                            let mut mx = f64::MIN;
                            scope_data[c].with_untracked(|data| {
                                for p in data.iter().rev().take(STATS_WINDOW) {
                                    if p.time_ms < t_start { break; }
                                    let v = p.value as f64;
                                    if v < mn { mn = v; }
                                    if v > mx { mx = v; }
                                }
                            });
                            if mn == f64::MAX { (0.0, 1.0) }
                            else {
                                let margin = (mx - mn).max(0.01) * 0.15;
                                (mn - margin, mx + margin)
                            }
                        } else { (y_min_all, y_max_all) };
                        let band_top = mt + bi as f64 * band_h;
                        let band_bot = band_top + band_h;
                        // Label at band top + middle + bottom
                        ctx.set_fill_style_str(COLORS[c]);
                        let _ = ctx.fill_text(&format!("{:.2}", bmax), ml - 8.0, band_top + 10.0);
                        let _ = ctx.fill_text(&format!("{:.2}", (bmin + bmax) / 2.0), ml - 8.0, (band_top + band_bot) / 2.0);
                        let _ = ctx.fill_text(&format!("{:.2}", bmin), ml - 8.0, band_bot - 4.0);
                    }
                }
            }

            // X axis labels
            ctx.set_text_align("center");
            ctx.set_fill_style_str("rgba(148,163,184,0.55)");
            for j in 0..=v_majors {
                let frac = j as f64 / v_majors as f64;
                let x = ml + frac * pw;
                let secs_ago = (1.0 - frac) * win_sec;
                let lbl = if secs_ago >= 60.0 {
                    format!("-{:.0}m", secs_ago / 60.0)
                } else {
                    format!("-{:.0}s", secs_ago)
                };
                let _ = ctx.fill_text(&lbl, x, h - mb + 16.0);
            }

            // Plot frame
            ctx.set_stroke_style_str("rgba(148,163,184,0.28)");
            ctx.set_line_width(1.0);
            ctx.stroke_rect(ml, mt, pw, ph);

            // -- Plot channel traces --
            let plot_ch = |ctx: &CanvasRenderingContext2d, c: usize, color: &str, get_val: &dyn Fn(usize) -> Option<(f64, f32)>, n: usize, band_top: f64, band_h: f64, y_min: f64, y_max: f64| {
                if n < 2 { return; }
                let y_span = y_max - y_min;
                ctx.set_stroke_style_str(color);
                ctx.set_line_width(1.3);
                ctx.begin_path();
                let mut started = false;
                // Decimation
                let skip = if n > (pw as usize * 2) { n / (pw as usize * 2) } else { 1 };
                for idx in 0..n {
                    if skip > 1 && idx % skip != 0 { continue; }
                    let Some((t, v)) = get_val(idx) else { continue };
                    let x = ml + ((t - t_start) / win_ms) * pw;
                    let y = band_top + ((y_max - v as f64) / y_span) * band_h;
                    let y = y.clamp(band_top, band_top + band_h);
                    if !started { ctx.move_to(x, y); started = true; }
                    else { ctx.line_to(x, y); }
                }
                ctx.stroke();
                let _ = c;
            };

            match mode {
                PlotMode::Overlay => {
                    for c in 0..4 {
                        if !ch_en[c] { continue; }
                        scope_data[c].with_untracked(|data| {
                            // Build a filtered view
                            let filt: Vec<(f64, f32)> = data.iter()
                                .filter(|p| p.time_ms >= t_start)
                                .map(|p| (p.time_ms, p.value))
                                .collect();
                            let n = filt.len();
                            plot_ch(&ctx, c, COLORS[c], &|i| filt.get(i).copied(), n, mt, ph, y_min_all, y_max_all);
                        });
                    }
                    // Math trace: CH A - CH B (overlay only)
                    if math_on && ch_en[0] && ch_en[1] {
                        // Build via nearest-neighbor alignment on A's timestamps
                        scope_data[0].with_untracked(|data_a| {
                            scope_data[1].with_untracked(|data_b| {
                                if data_a.is_empty() || data_b.is_empty() { return; }
                                let mut bj = 0usize;
                                let filt: Vec<(f64, f32)> = data_a.iter()
                                    .filter(|p| p.time_ms >= t_start)
                                    .map(|pa| {
                                        while bj + 1 < data_b.len() && (data_b[bj + 1].time_ms - pa.time_ms).abs() < (data_b[bj].time_ms - pa.time_ms).abs() {
                                            bj += 1;
                                        }
                                        let vb = data_b[bj].value;
                                        (pa.time_ms, pa.value - vb)
                                    })
                                    .collect();
                                let n = filt.len();
                                plot_ch(&ctx, 4, MATH_COLOR, &|i| filt.get(i).copied(), n, mt, ph, y_min_all, y_max_all);
                            });
                        });
                    }
                }
                PlotMode::Stacked => {
                    for (bi, &c) in active_channels.iter().enumerate() {
                        let band_top = mt + bi as f64 * band_h;
                        // Band divider
                        if bi > 0 {
                            ctx.set_stroke_style_str("rgba(148,163,184,0.18)");
                            ctx.set_line_width(0.8);
                            ctx.begin_path();
                            ctx.move_to(ml, band_top);
                            ctx.line_to(w - mr, band_top);
                            ctx.stroke();
                        }
                        let (bmin, bmax) = if yr == "auto" {
                            let mut mn = f64::MAX;
                            let mut mx = f64::MIN;
                            scope_data[c].with_untracked(|data| {
                                for p in data.iter().rev().take(STATS_WINDOW) {
                                    if p.time_ms < t_start { break; }
                                    let v = p.value as f64;
                                    if v < mn { mn = v; }
                                    if v > mx { mx = v; }
                                }
                            });
                            if mn == f64::MAX { (0.0, 1.0) }
                            else {
                                let margin = (mx - mn).max(0.01) * 0.15;
                                (mn - margin, mx + margin)
                            }
                        } else { (y_min_all, y_max_all) };
                        scope_data[c].with_untracked(|data| {
                            let filt: Vec<(f64, f32)> = data.iter()
                                .filter(|p| p.time_ms >= t_start)
                                .map(|p| (p.time_ms, p.value))
                                .collect();
                            let n = filt.len();
                            plot_ch(&ctx, c, COLORS[c], &|i| filt.get(i).copied(), n, band_top, band_h, bmin, bmax);
                        });
                        // Channel badge
                        ctx.set_fill_style_str("rgba(10,15,30,0.75)");
                        ctx.fill_rect(ml + 6.0, band_top + 4.0, 52.0, 16.0);
                        ctx.set_fill_style_str(COLORS[c]);
                        ctx.fill_rect(ml + 6.0, band_top + 4.0, 3.0, 16.0);
                        ctx.set_font("10px 'JetBrains Mono', monospace");
                        ctx.set_text_align("left");
                        ctx.set_fill_style_str(COLORS[c]);
                        let _ = ctx.fill_text(&format!("CH {}", CH_NAMES[c]), ml + 12.0, band_top + 15.0);
                    }
                }
            }

            // -- Measurement cursors A / B --
            let draw_named_cursor = |ctx: &CanvasRenderingContext2d, x: f64, label: &str, color: &str| {
                ctx.set_stroke_style_str(color);
                ctx.set_line_width(1.0);
                ctx.set_line_dash(&js_sys::Array::of2(&4.0.into(), &4.0.into()).into()).ok();
                ctx.begin_path();
                ctx.move_to(x, mt);
                ctx.line_to(x, mt + ph);
                ctx.stroke();
                ctx.set_line_dash(&js_sys::Array::new().into()).ok();
                ctx.set_font("10px 'JetBrains Mono', monospace");
                ctx.set_text_align("center");
                ctx.set_fill_style_str("rgba(10,15,30,0.85)");
                ctx.fill_rect(x - 10.0, mt - 2.0, 20.0, 14.0);
                ctx.set_fill_style_str(color);
                let _ = ctx.fill_text(label, x, mt + 9.0);
            };

            let cur_a_ms = cursor_a.get_untracked();
            let cur_b_ms = cursor_b.get_untracked();
            if let Some(t) = cur_a_ms {
                let x = ml + ((t - t_start) / win_ms) * pw;
                if x >= ml && x <= w - mr {
                    draw_named_cursor(&ctx, x, "A", "#22d3ee");
                }
            }
            if let Some(t) = cur_b_ms {
                let x = ml + ((t - t_start) / win_ms) * pw;
                if x >= ml && x <= w - mr {
                    draw_named_cursor(&ctx, x, "B", "#f472b6");
                }
            }

            // -- Hover cursor (existing behavior) --
            if let Some(cx) = cursor_x.get_untracked() {
                ctx.set_stroke_style_str("rgba(248,250,252,0.6)");
                ctx.set_line_width(1.0);
                ctx.begin_path();
                ctx.move_to(cx, mt);
                ctx.line_to(cx, mt + ph);
                ctx.stroke();

                let cursor_frac = (cx - ml) / pw;
                let cursor_time = t_start + cursor_frac * win_ms;
                let secs_ago = (now - cursor_time) / 1000.0;

                let mut tooltip_lines: Vec<String> = Vec::new();
                tooltip_lines.push(format!("t: -{:.2}s", secs_ago));

                for c in 0..4 {
                    if !ch_en[c] { continue; }
                    scope_data[c].with_untracked(|data| {
                        let mut interp_val: Option<f32> = None;
                        for i in 1..data.len() {
                            if data[i - 1].time_ms <= cursor_time && data[i].time_ms >= cursor_time {
                                let dt_seg = data[i].time_ms - data[i - 1].time_ms;
                                if dt_seg > 0.0 {
                                    let t_frac = (cursor_time - data[i - 1].time_ms) / dt_seg;
                                    interp_val = Some(
                                        data[i - 1].value + (data[i].value - data[i - 1].value) * t_frac as f32
                                    );
                                } else {
                                    interp_val = Some(data[i].value);
                                }
                                break;
                            }
                        }
                        if let Some(v) = interp_val {
                            tooltip_lines.push(format!("CH {}: {:.4}", CH_NAMES[c], v));
                        }
                    });
                }

                let tt_x = if cx + 160.0 > w - mr { cx - 165.0 } else { cx + 10.0 };
                let tt_y = mt + 10.0;
                let line_h = 16.0;
                let tt_h = tooltip_lines.len() as f64 * line_h + 8.0;
                let tt_w = 155.0;

                ctx.set_fill_style_str("rgba(10,15,30,0.92)");
                ctx.fill_rect(tt_x, tt_y, tt_w, tt_h);
                ctx.set_stroke_style_str("rgba(59,130,246,0.3)");
                ctx.stroke_rect(tt_x, tt_y, tt_w, tt_h);

                ctx.set_font("11px 'JetBrains Mono', monospace");
                ctx.set_text_align("left");
                for (li, line) in tooltip_lines.iter().enumerate() {
                    let color = if li == 0 {
                        "rgba(148,163,184,0.8)".to_string()
                    } else {
                        let mut ch_idx = 0;
                        let mut count = 0;
                        for c in 0..4 {
                            if ch_en[c] {
                                count += 1;
                                if count == li { ch_idx = c; break; }
                            }
                        }
                        COLORS[ch_idx].to_string()
                    };
                    ctx.set_fill_style_str(&color);
                    let _ = ctx.fill_text(line, tt_x + 6.0, tt_y + 14.0 + li as f64 * line_h);
                }
            }

            // -- Corner overlays --
            ctx.set_font("10px 'JetBrains Mono', monospace");

            // Top-right: configured SPS (from dropdown) + bucket rate indicator.
            // The former "live SPS" computed bucket-arrival rate which is ~100 Hz
            // regardless of ADC rate; that was misleading, so we show the
            // configured rate derived directly from the ADC_RATE_OPTIONS label.
            let bucket_rate = sample_rate.get_untracked(); // buckets per second (≈100 when healthy)
            let first_en = (0..4).find(|&i| ch_en[i]);
            let cfg_lbl = first_en.map(|i| rate_label(adc_rate[i].get_untracked())).unwrap_or_else(|| "— SPS".to_string());
            let rate_overlay = format!("{}   buckets: {}/s", cfg_lbl, bucket_rate);
            ctx.set_fill_style_str("rgba(10,15,30,0.7)");
            let tw = 220.0;
            ctx.fill_rect(w - mr - tw - 4.0, mt + 2.0, tw, 16.0);
            ctx.set_fill_style_str(if running.get_untracked() && bucket_rate > 0 { "#10b981" } else { "rgba(148,163,184,0.7)" });
            ctx.begin_path();
            let _ = ctx.arc(w - mr - tw + 4.0, mt + 10.0, 3.5, 0.0, std::f64::consts::TAU);
            ctx.fill();
            ctx.set_fill_style_str("rgba(226,232,240,0.85)");
            ctx.set_text_align("right");
            let _ = ctx.fill_text(&rate_overlay, w - mr - 6.0, mt + 14.0);

            // Top-left: elapsed capture time
            if let Some(t0) = capture_start_ms.get_untracked() {
                let elapsed_ms = now - t0;
                let lbl = format!("elapsed {}", fmt_hms(elapsed_ms));
                ctx.set_fill_style_str("rgba(10,15,30,0.7)");
                ctx.fill_rect(ml + 4.0, mt + 2.0, 150.0, 16.0);
                ctx.set_fill_style_str("rgba(226,232,240,0.85)");
                ctx.set_text_align("left");
                let _ = ctx.fill_text(&lbl, ml + 10.0, mt + 14.0);
            }

            // Bottom-right: buffer fill bar. Shows per-channel ring fullness
            // (first enabled channel) + equivalent time at configured ADC rate.
            let first_ch_for_buf = first_en.unwrap_or(0);
            let ch_samples = scope_data[first_ch_for_buf].with_untracked(|d| d.len());
            let fill_frac = (ch_samples as f64 / MAX_POINTS as f64).clamp(0.0, 1.0);
            let cfg_sps_first = first_en.map(|i| rate_to_sps(adc_rate[i].get_untracked())).unwrap_or(0);
            let time_str = if cfg_sps_first > 0 {
                let secs = ch_samples as f64 / cfg_sps_first as f64;
                if secs >= 60.0 { format!("~{:.1}m", secs / 60.0) } else { format!("~{:.0}s", secs) }
            } else { "—".to_string() };
            let bar_w = 190.0;
            let bar_x = w - mr - bar_w - 4.0;
            let bar_y = h - mb - 14.0;
            ctx.set_fill_style_str("rgba(10,15,30,0.7)");
            ctx.fill_rect(bar_x - 2.0, bar_y - 2.0, bar_w + 4.0, 12.0);
            ctx.set_fill_style_str("rgba(148,163,184,0.18)");
            ctx.fill_rect(bar_x, bar_y, bar_w, 8.0);
            ctx.set_fill_style_str("#3b82f6");
            ctx.fill_rect(bar_x, bar_y, bar_w * fill_frac, 8.0);
            ctx.set_fill_style_str("rgba(226,232,240,0.85)");
            ctx.set_text_align("right");
            let buf_lbl = format!("buf {:.0}% · {}k/{}k · {}",
                fill_frac * 100.0,
                ch_samples / 1000,
                MAX_POINTS / 1000,
                time_str);
            let _ = ctx.fill_text(&buf_lbl, bar_x - 6.0, bar_y + 8.0);

            // Bottom-left: mode + time base
            let mode_str = match mode { PlotMode::Overlay => "OVERLAY", PlotMode::Stacked => "STACKED" };
            let tb_label = if win_sec >= 60.0 { format!("{:.1} min/div", (win_sec / v_majors as f64) / 60.0) }
                else { format!("{:.2} s/div", win_sec / v_majors as f64) };
            let lbl = format!("{}   {}", mode_str, tb_label);
            ctx.set_fill_style_str("rgba(10,15,30,0.7)");
            ctx.fill_rect(ml + 4.0, h - mb - 16.0, 200.0, 14.0);
            ctx.set_fill_style_str("rgba(226,232,240,0.85)");
            ctx.set_text_align("left");
            let _ = ctx.fill_text(&lbl, ml + 10.0, h - mb - 5.0);

            // -- Cursor delta readout near cursors --
            if let (Some(ta), Some(tb)) = (cur_a_ms, cur_b_ms) {
                let dx = tb - ta;
                let txt = format!("\u{0394}X = {}", fmt_duration_ms(dx));
                ctx.set_font("11px 'JetBrains Mono', monospace");
                ctx.set_text_align("center");
                ctx.set_fill_style_str("rgba(10,15,30,0.88)");
                let tx = (w + ml) / 2.0;
                ctx.fill_rect(tx - 60.0, mt + ph - 22.0, 120.0, 18.0);
                ctx.set_fill_style_str("rgba(226,232,240,0.95)");
                let _ = ctx.fill_text(&txt, tx, mt + ph - 10.0);
            }

            // Perf guard (A5): warn once/sec if a draw exceeds 30 ms — that
            // pushes the 20 FPS budget and likely causes the UI freeze.
            let draw_end = js_sys::Date::now();
            let draw_dt = draw_end - draw_start;
            if draw_dt > 30.0 && draw_end - last_perf_log > 1000.0 {
                last_perf_log = draw_end;
                web_sys::console::warn_1(&JsValue::from_str(
                    &format!("scope: draw {:.1} ms (>30ms budget)", draw_dt)
                ));
            }
        }
    });

    // -- View --
    view! {
        <div class="tab-content scope-tab scope-tab-sci">
            <div class="scope-layout">
                // ============ LEFT RAIL (per-channel cards) ============
                <aside class="scope-left">
                    <div class="scope-left-title">"Channels"</div>
                    {(0..4).map(|ch| {
                        let rate_sig = adc_rate[ch];
                        let range_sig = adc_range[ch];
                        let mux_sig = adc_mux[ch];
                        let label_sig = channel_labels[ch];
                        let yoff_sig = y_offset[ch];
                        let inv_sig = invert[ch];
                        let coll_sig = channel_collapsed[ch];
                        view! {
                            <div class="scope-ch-card" style=format!("--ch-color: {}", COLORS[ch])>
                                <div class="scope-ch-card-head">
                                    <span class="scope-ch-swatch" style=format!("background: {}", COLORS[ch])></span>
                                    <span class="scope-ch-letter">{CH_NAMES[ch]}</span>
                                    <input type="text"
                                        class="scope-ch-name"
                                        prop:value=move || label_sig.get()
                                        on:input=move |e| label_sig.set(event_target_value(&e))
                                    />
                                    <label class="scope-ch-enable">
                                        <input type="checkbox"
                                            prop:checked=move || channels_en.get()[ch]
                                            on:change=move |e| {
                                                let checked: bool = e.target().unwrap().unchecked_into::<web_sys::HtmlInputElement>().checked();
                                                set_channels_en.update(|arr| arr[ch] = checked);
                                            }
                                        />
                                    </label>
                                    <button class="scope-ch-chevron"
                                        on:click=move |_| coll_sig.update(|v| *v = !*v)
                                    >
                                        {move || if coll_sig.get() { "\u{25B8}" } else { "\u{25BE}" }}
                                    </button>
                                </div>
                                <div class="scope-ch-card-body" class:scope-collapsed=move || coll_sig.get()>
                                    <div class="scope-ch-readout">
                                        <span class="scope-ch-big">{move || {
                                            let last = scope_data[ch].with(|d| d.last().map(|p| p.value));
                                            match last {
                                                Some(v) => format!("{:+.4}", v),
                                                None => "—".to_string(),
                                            }
                                        }}</span>
                                        <span class="scope-ch-unit">{move || range_unit(range_sig.get())}</span>
                                    </div>
                                    <div class="scope-ch-row">
                                        <label>"Rate"</label>
                                        <select class="dropdown dropdown-sm"
                                            on:change=move |e| {
                                                let new_rate: u8 = event_target_value(&e).parse().unwrap_or(3);
                                                rate_sig.set(new_rate);
                                                // Push to backend immediately so the ADC retunes even
                                                // while streaming — avoids needing a manual restart.
                                                let mux = mux_sig.get_untracked();
                                                let range = range_sig.get_untracked();
                                                web_sys::console::log_1(&JsValue::from_str(
                                                    &format!("scope: set_adc_config ch={} mux={} range={} rate={}", ch, mux, range, new_rate)
                                                ));
                                                spawn_local(async move {
                                                    #[derive(serde::Serialize)]
                                                    struct AdcCfgArgs { channel: u8, mux: u8, range: u8, rate: u8 }
                                                    let args = serde_wasm_bindgen::to_value(&AdcCfgArgs {
                                                        channel: ch as u8, mux, range, rate: new_rate,
                                                    }).unwrap();
                                                    let _ = invoke("set_adc_config", args).await;
                                                });
                                            }
                                        >
                                            {ADC_RATE_OPTIONS.iter().map(|(code, label)| {
                                                let code = *code;
                                                let label = *label;
                                                view! {
                                                    <option value=code.to_string()
                                                        selected=move || rate_sig.get() == code
                                                    >{label}</option>
                                                }
                                            }).collect::<Vec<_>>()}
                                        </select>
                                    </div>
                                    <div class="scope-ch-row">
                                        <label>"Range"</label>
                                        <select class="dropdown dropdown-sm"
                                            on:change=move |e| {
                                                range_sig.set(event_target_value(&e).parse().unwrap_or(1));
                                            }
                                        >
                                            {ADC_RANGE_OPTIONS.iter().map(|(code, label, _, _)| {
                                                let code = *code;
                                                let label = *label;
                                                view! {
                                                    <option value=code.to_string()
                                                        selected=move || range_sig.get() == code
                                                    >{label}</option>
                                                }
                                            }).collect::<Vec<_>>()}
                                        </select>
                                    </div>
                                    <div class="scope-ch-row">
                                        <label>"Mux"</label>
                                        <select class="dropdown dropdown-sm"
                                            prop:disabled=move || {
                                                state.get().channels.get(ch).map(|c| c.function).unwrap_or(3) != 3
                                            }
                                            on:change=move |e| {
                                                mux_sig.set(event_target_value(&e).parse().unwrap_or(0));
                                            }
                                        >
                                            {ADC_MUX_OPTIONS.iter().map(|(code, label)| {
                                                let code = *code;
                                                let label = *label;
                                                view! {
                                                    <option value=code.to_string()
                                                        selected=move || mux_sig.get() == code
                                                    >{label}</option>
                                                }
                                            }).collect::<Vec<_>>()}
                                        </select>
                                    </div>
                                    <div class="scope-ch-row">
                                        <label>"Y-offset"</label>
                                        <input type="range" class="slider scope-ch-slider"
                                            min="-5" max="5" step="0.1"
                                            prop:value=move || yoff_sig.get().to_string()
                                            on:input=move |e| yoff_sig.set(event_target_value(&e).parse().unwrap_or(0.0))
                                        />
                                        <span class="scope-ch-offset-val">{move || format!("{:+.1}", yoff_sig.get())}</span>
                                    </div>
                                    <div class="scope-ch-row">
                                        <label>"Invert"</label>
                                        <input type="checkbox"
                                            prop:checked=move || inv_sig.get()
                                            on:change=move |e| {
                                                let checked: bool = e.target().unwrap().unchecked_into::<web_sys::HtmlInputElement>().checked();
                                                inv_sig.set(checked);
                                            }
                                        />
                                    </div>
                                    <div class="scope-ch-stats">
                                        {move || {
                                            // Read the throttled cached stats (refresh ~2 Hz).
                                            let stats = cached_stats[ch].get();
                                            view! {
                                                <div class="scope-stat"><span>"Min"</span><b>{if stats.has_data { format!("{:.3}", stats.min) } else { "—".to_string() }}</b></div>
                                                <div class="scope-stat"><span>"Max"</span><b>{if stats.has_data { format!("{:.3}", stats.max) } else { "—".to_string() }}</b></div>
                                                <div class="scope-stat"><span>"Mean"</span><b>{if stats.has_data { format!("{:.3}", stats.mean) } else { "—".to_string() }}</b></div>
                                                <div class="scope-stat"><span>"RMS"</span><b>{if stats.has_data { format!("{:.3}", stats.rms) } else { "—".to_string() }}</b></div>
                                            }
                                        }}
                                    </div>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()}
                </aside>

                // ============ CENTER (toolbar + plot + measurements) ============
                <section class="scope-center">
                    // ---- Top toolbar ----
                    <div class="scope-toolbar scope-toolbar-sci">
                        <button class="scope-pill" class:scope-pill-running=move || running.get()
                            on:click=move |_| {
                                let new = !running.get_untracked();
                                set_running.set(new);
                                if !new { set_sample_rate.set(0); }
                                single_shot.set(false);
                            }
                        >
                            <span class="scope-pill-dot" class:running=move || running.get()></span>
                            {move || if running.get() { "Stop" } else { "Start" }}
                        </button>

                        <button class="scope-btn"
                            title="Capture one window then auto-stop"
                            on:click=move |_| {
                                single_shot.set(true);
                                set_running.set(true);
                            }
                        >"Single"</button>

                        <div class="toolbar-divider"></div>

                        <div class="scope-mode-toggle">
                            <button class="scope-mode-btn"
                                class:scope-mode-active=move || plot_mode.get() == PlotMode::Overlay
                                on:click=move |_| plot_mode.set(PlotMode::Overlay)
                            >"Overlay"</button>
                            <button class="scope-mode-btn"
                                class:scope-mode-active=move || plot_mode.get() == PlotMode::Stacked
                                on:click=move |_| plot_mode.set(PlotMode::Stacked)
                            >"Stacked"</button>
                        </div>

                        <button class="scope-btn"
                            on:click=move |_| {
                                // Pulse Y auto-scale by forcing y_range to "auto"
                                set_y_range.set("auto".to_string());
                                show_toast("Y auto-scale", "ok");
                            }
                        >"Auto Y"</button>

                        <select class="dropdown dropdown-sm"
                            prop:value=move || format!("{}", window_sec.get() as u64)
                            on:change=move |e| set_window_sec.set(event_target_value(&e).parse().unwrap_or(30.0))
                        >
                            <option value="1" selected=move || (window_sec.get() - 1.0).abs() < 0.01>"1 s"</option>
                            <option value="5" selected=move || (window_sec.get() - 5.0).abs() < 0.01>"5 s"</option>
                            <option value="10" selected=move || (window_sec.get() - 10.0).abs() < 0.01>"10 s"</option>
                            <option value="30" selected=move || (window_sec.get() - 30.0).abs() < 0.01>"30 s"</option>
                            <option value="60" selected=move || (window_sec.get() - 60.0).abs() < 0.01>"1 min"</option>
                            <option value="120" selected=move || (window_sec.get() - 120.0).abs() < 0.01>"2 min"</option>
                            <option value="300" selected=move || (window_sec.get() - 300.0).abs() < 0.01>"5 min"</option>
                            <option value="600" selected=move || (window_sec.get() - 600.0).abs() < 0.01>"10 min"</option>
                            <option value="1800" selected=move || (window_sec.get() - 1800.0).abs() < 0.01>"30 min"</option>
                            <option value="3600" selected=move || (window_sec.get() - 3600.0).abs() < 0.01>"1 h"</option>
                            <option value="7200" selected=move || (window_sec.get() - 7200.0).abs() < 0.01>"2 h"</option>
                        </select>

                        <select class="dropdown dropdown-sm"
                            on:change=move |e| set_y_range.set(event_target_value(&e))
                        >
                            <option value="auto" selected>"Y: Auto"</option>
                            <option value="0-12">"0–12 V"</option>
                            <option value="pm12">"±12 V"</option>
                            <option value="0-625m">"0–625 mV"</option>
                            <option value="0-25m">"0–25 mA"</option>
                        </select>

                        <div class="toolbar-divider"></div>

                        <button class="scope-btn"
                            class:scope-btn-active=move || math_a_enabled.get()
                            title="Math: CH A - CH B"
                            on:click=move |_| math_a_enabled.update(|v| *v = !*v)
                        >"Math A−B"</button>

                        <select class="dropdown dropdown-sm"
                            title="Export"
                            on:change=move |e| {
                                let v = event_target_value(&e);
                                match v.as_str() {
                                    "csv" => show_toast("Use Record to capture a BBSC + Export", "info"),
                                    "png" => show_toast("PNG export not implemented yet", "info"),
                                    "json" => show_toast("JSON export not implemented yet", "info"),
                                    _ => {}
                                }
                            }
                        >
                            <option value="" selected>"Export…"</option>
                            <option value="csv">"CSV"</option>
                            <option value="png">"PNG"</option>
                            <option value="json">"JSON"</option>
                        </select>

                        <div class="toolbar-divider"></div>

                        <button class="scope-btn" on:click=move |_| {
                            for ch in 0..4 { scope_data[ch].set(Vec::new()); }
                        }>"Clear"</button>

                        <button class="scope-btn" title="Force reset — use when the scope is stuck"
                            on:click=move |_| {
                                set_running.set(false);
                                set_sample_rate.set(0);
                                for ch in 0..4 { scope_data[ch].set(Vec::new()); }
                                spawn_local(async {
                                    let _ = invoke("stop_scope_stream", wasm_bindgen::JsValue::NULL).await;
                                });
                                show_toast("Scope force reset", "ok");
                            }
                        >"Force Reset"</button>

                        <div class="toolbar-divider"></div>

                        <button class="scope-btn scope-rec-btn"
                            class:scope-btn-csv=move || recording.get()
                            on:click=move |_| {
                                if recording.get_untracked() {
                                    spawn_local(async move {
                                        let result = invoke("stop_recording", wasm_bindgen::JsValue::NULL).await;
                                        if let Ok(count) = serde_wasm_bindgen::from_value::<u64>(result) {
                                            log(&format!("Recording stopped: {} samples", count));
                                        }
                                    });
                                    // Freeze elapsed label for 3 s
                                    if let Some(t0) = recording_start_ms.get_untracked() {
                                        let now = js_sys::Date::now();
                                        recording_freeze_label.set(Some(fmt_elapsed(now - t0)));
                                        recording_stop_ms.set(Some(now));
                                    }
                                    recording_start_ms.set(None);
                                    set_recording.set(false);
                                } else {
                                    let ch_en = channels_en.get_untracked();
                                    let mask: u8 = (0..4).fold(0u8, |m, i| if ch_en[i] { m | (1 << i) } else { m });
                                    let ds = state.get_untracked();
                                    let ranges: Vec<u8> = (0..4).map(|i| {
                                        if i < ds.channels.len() { ds.channels[i].adc_range } else { 0 }
                                    }).collect();
                                    let rate = sample_rate.get_untracked();

                                    spawn_local(async move {
                                        let result = invoke("pick_save_file", wasm_bindgen::JsValue::NULL).await;
                                        if let Ok(opt) = serde_wasm_bindgen::from_value::<Option<String>>(result) {
                                            if let Some(path) = opt {
                                                if !path.is_empty() {
                                                    let bbsc_path = if path.ends_with(".csv") {
                                                        path.replace(".csv", ".bbsc")
                                                    } else if !path.ends_with(".bbsc") {
                                                        format!("{}.bbsc", path)
                                                    } else { path };

                                                    let args = serde_wasm_bindgen::to_value(
                                                        &serde_json::json!({
                                                            "path": bbsc_path,
                                                            "channelMask": mask,
                                                            "adcRanges": ranges,
                                                            "sampleRate": rate.max(20)
                                                        })
                                                    ).unwrap();
                                                    let _ = invoke("start_recording", args).await;
                                                    set_csv_path.set(bbsc_path);
                                                    recording_start_ms.set(Some(js_sys::Date::now()));
                                                    recording_bucket_count.set(0);
                                                    recording_stop_ms.set(None);
                                                    recording_freeze_label.set(None);
                                                    set_recording.set(true);
                                                }
                                            }
                                        }
                                    });
                                }
                            }
                        >
                            <span class="scope-rec-dot" class:rec-on=move || recording.get()></span>
                            {move || if recording.get() { "Stop Rec" } else { "Record" }}
                        </button>

                        <span class="scope-rec-elapsed" class:rec-active=move || recording.get()>
                            {move || {
                                let _ = timer_tick.get();
                                if recording.get() {
                                    if let Some(t0) = recording_start_ms.get() {
                                        fmt_elapsed(js_sys::Date::now() - t0)
                                    } else { "--:--:--.-".to_string() }
                                } else if let Some(lbl) = recording_freeze_label.get() {
                                    lbl
                                } else {
                                    "--:--:--.-".to_string()
                                }
                            }}
                        </span>

                        <button class="scope-btn"
                            title="Export recorded BBSC to CSV"
                            on:click=move |_| {
                                spawn_local(async move {
                                    let result = invoke("pick_save_file", wasm_bindgen::JsValue::NULL).await;
                                    if let Ok(Some(export_csv)) = serde_wasm_bindgen::from_value::<Option<String>>(result) {
                                        if export_csv.is_empty() { return; }
                                        let last_rec = csv_path.get_untracked();
                                        let bbsc = if !last_rec.is_empty() && last_rec.ends_with(".bbsc") {
                                            last_rec
                                        } else {
                                            log("No BBSC file to export. Record first.");
                                            return;
                                        };
                                        let csv = if export_csv.ends_with(".bbsc") {
                                            export_csv.replace(".bbsc", ".csv")
                                        } else { export_csv };
                                        let args = serde_wasm_bindgen::to_value(
                                            &serde_json::json!({ "bbscPath": bbsc, "csvPath": csv })
                                        ).unwrap();
                                        let result = invoke("export_bbsc_to_csv", args).await;
                                        if let Ok(count) = serde_wasm_bindgen::from_value::<u64>(result) {
                                            log(&format!("Exported {} samples to CSV", count));
                                        }
                                    }
                                });
                            }
                        >"Export CSV"</button>
                    </div>

                    // ---- Plot ----
                    <div class="scope-canvas-wrap scope-canvas-wrap-sci">
                        <canvas node_ref=canvas_ref class="scope-canvas scope-canvas-sci"
                            on:click=move |e: web_sys::MouseEvent| {
                                let target = e.target().unwrap();
                                let canvas: HtmlCanvasElement = target.unchecked_into();
                                let rect = canvas.get_bounding_client_rect();
                                let x = e.client_x() as f64 - rect.left();
                                // Time (ms) for cursor placement
                                let ml = 65.0; let mr = 15.0;
                                let w = rect.width();
                                let pw = w - ml - mr;
                                let cursor_frac = ((x - ml) / pw).clamp(0.0, 1.0);
                                let now = js_sys::Date::now();
                                let t_ms = now - window_sec.get_untracked() * 1000.0 + cursor_frac * window_sec.get_untracked() * 1000.0;
                                if e.alt_key() {
                                    cursor_a.set(Some(t_ms));
                                } else if e.shift_key() {
                                    cursor_b.set(Some(t_ms));
                                } else {
                                    set_cursor_x.set(Some(x));
                                }
                            }
                            on:wheel=move |e: web_sys::WheelEvent| {
                                e.prevent_default();
                                let dy = e.delta_y();
                                let cur = window_sec.get_untracked();
                                let new_val = if dy < 0.0 { (cur / 1.2).max(0.5) } else { (cur * 1.2).min(7200.0) };
                                set_window_sec.set(new_val);
                            }
                            on:contextmenu=move |e: web_sys::MouseEvent| {
                                e.prevent_default();
                                set_cursor_x.set(None);
                                cursor_a.set(None);
                                cursor_b.set(None);
                            }
                            on:keydown=move |e: web_sys::KeyboardEvent| {
                                if e.key() == "Escape" {
                                    set_cursor_x.set(None);
                                    cursor_a.set(None);
                                    cursor_b.set(None);
                                }
                            }
                            tabindex="0"
                        ></canvas>
                    </div>

                    // ---- Bottom measurements panel ----
                    <div class="scope-meas-panel">
                        <table class="scope-meas-table">
                            <thead>
                                <tr>
                                    <th>"Ch"</th>
                                    <th>"Current"</th>
                                    <th>"Min"</th>
                                    <th>"Max"</th>
                                    <th>"Vpp"</th>
                                    <th>"Mean"</th>
                                    <th>"RMS"</th>
                                    <th>"Freq"</th>
                                </tr>
                            </thead>
                            <tbody>
                                {(0..4).map(|ch| {
                                    view! {
                                        <tr class:scope-meas-off=move || !channels_en.get()[ch]
                                            style=format!("--ch-color: {}", COLORS[ch])
                                        >
                                            <td><span class="scope-meas-chip">{format!("CH {}", CH_NAMES[ch])}</span></td>
                                            {move || {
                                                // Cached stats (refresh ~2 Hz) — keeps the view
                                                // render cheap; avoids scanning 5k samples ×4 per tick.
                                                let stats = cached_stats[ch].get();
                                                let fmt = |v: f32| if stats.has_data { format!("{:+.4}", v) } else { "—".to_string() };
                                                let freq = if stats.freq > 0.0 { format!("{:.2} Hz", stats.freq) } else { "—".to_string() };
                                                view! {
                                                    <td>{fmt(stats.current)}</td>
                                                    <td>{fmt(stats.min)}</td>
                                                    <td>{fmt(stats.max)}</td>
                                                    <td>{if stats.has_data { format!("{:.4}", stats.vpp) } else { "—".to_string() }}</td>
                                                    <td>{fmt(stats.mean)}</td>
                                                    <td>{if stats.has_data { format!("{:.4}", stats.rms) } else { "—".to_string() }}</td>
                                                    <td>{freq}</td>
                                                }
                                            }}
                                        </tr>
                                    }
                                }).collect::<Vec<_>>()}
                            </tbody>
                        </table>

                        <div class="scope-cursor-row">
                            {move || {
                                let a = cursor_a.get();
                                let b = cursor_b.get();
                                let now = js_sys::Date::now();
                                let fmt_cur = |t: f64| fmt_duration_ms(t - now);
                                view! {
                                    <span class="scope-cursor-pill scope-cursor-a">
                                        "X_A: "{match a { Some(t) => fmt_cur(t), None => "—".to_string() }}
                                    </span>
                                    <span class="scope-cursor-pill scope-cursor-b">
                                        "X_B: "{match b { Some(t) => fmt_cur(t), None => "—".to_string() }}
                                    </span>
                                    <span class="scope-cursor-pill scope-cursor-d">
                                        "\u{0394}X: "{match (a, b) { (Some(ta), Some(tb)) => fmt_duration_ms(tb - ta), _ => "—".to_string() }}
                                    </span>
                                }
                            }}
                        </div>

                        // Recording status strip
                        {move || {
                            let _ = timer_tick.get();
                            if recording.get() {
                                let elapsed = recording_start_ms.get().map(|t0| fmt_elapsed(js_sys::Date::now() - t0)).unwrap_or_else(|| "--:--:--.-".to_string());
                                let path = csv_path.get();
                                let bucket_count = recording_bucket_count.get();
                                let bytes_est = bucket_count * 60;
                                let kb = bytes_est as f64 / 1024.0;
                                let size_str = if kb >= 1024.0 { format!("~{:.1} MB", kb / 1024.0) } else { format!("~{:.0} KB", kb) };
                                Some(view! {
                                    <div class="scope-rec-strip">
                                        <span class="scope-rec-live">
                                            <span class="scope-rec-dot rec-on"></span>"REC"
                                        </span>
                                        <span class="scope-rec-time">{elapsed}</span>
                                        <span class="scope-rec-sep">"|"</span>
                                        <span class="scope-rec-path">{truncate_path(&path, 40)}</span>
                                        <span class="scope-rec-sep">"|"</span>
                                        <span class="scope-rec-size">{size_str}</span>
                                    </div>
                                })
                            } else { None }
                        }}
                    </div>
                </section>
            </div>
        </div>
    }
}

async fn sleep_ms(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window().unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
}
