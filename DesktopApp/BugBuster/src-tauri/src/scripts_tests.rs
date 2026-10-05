// =============================================================================
// scripts_tests.rs - behavioural tests for scripts.rs against an in-memory device that
// answers like the firmware routes, over both transports (USB tunnel and HTTP).
// =============================================================================

use std::collections::{BTreeMap, HashMap};
use std::sync::Mutex;

use async_trait::async_trait;
use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use serde_json::{json, Value};

use crate::scripts::{
    autorun_timeout_ms, execute, tunnel, tunnel_exchange, BbpSend, Kind, ScriptWire, WireError,
};
use crate::transport::{HttpExchange, HttpReply};

const FILE_PAGE: usize = 3072;
const LOG_PAGE: usize = 4096;

#[derive(Default)]
struct Dev {
    files: BTreeMap<String, Vec<u8>>,
    upload: Option<(String, Vec<u8>)>,
    slot: Option<(String, u64)>,
    next_id: u64,
    log: Vec<u8>,
    log_start: u64,
    lint_busy: bool,
    autorun: Option<String>,
    /// Route names in call order.
    calls: Vec<String>,
    chunk_calls: usize,
    /// 1-based chunk calls whose reply is lost after the device applied them.
    lose_reply_on_chunk: Vec<usize>,
    executed: u32,
    last_persist: Option<bool>,
    resets: u32,
    stops: u32,
}

struct Fake {
    kind: Kind,
    dev: Mutex<Dev>,
    unsupported: bool,
    down: bool,
    http_calls: Mutex<u32>,
}

fn fake(kind: Kind) -> Fake {
    let mut dev = Dev { next_id: 1, ..Default::default() };
    dev.files.insert("old.py".into(), b"print('old')\n".to_vec());
    Fake { kind, dev: Mutex::new(dev), unsupported: false, down: false, http_calls: Mutex::new(0) }
}

fn s(a: &Value, k: &str) -> Option<String> {
    a.get(k).and_then(|v| v.as_str().map(str::to_string))
}
fn n(a: &Value, k: &str) -> Option<u64> {
    a.get(k).and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|t| t.parse().ok())))
}
fn flag(a: &Value, k: &str) -> bool {
    match a.get(k) {
        Some(Value::Bool(b)) => *b,
        Some(Value::String(t)) => t == "1" || t == "true",
        Some(Value::Number(x)) => x.as_u64() == Some(1),
        _ => false,
    }
}
fn err(m: &str) -> Value {
    json!({"ok": false, "error": m})
}
fn busy(d: &Dev, m: &str) -> Value {
    let (name, id) = d.slot.clone().unwrap();
    json!({"ok": false, "error": m, "running": name, "id": id})
}

impl Fake {
    /// Firmware route semantics shared by both fronts. `text` is the raw HTTP body.
    fn api(&self, route: &str, a: &Value, text: Option<&str>) -> Value {
        let mut d = self.dev.lock().unwrap();
        d.calls.push(route.to_string());
        match route {
            "status" => json!({
                "running": d.slot.is_some(),
                "state": if d.slot.is_some() { "running" } else { "idle" },
                "fileSlotName": d.slot.as_ref().map(|x| x.0.clone()).unwrap_or_default(),
                "fileSlotId": d.slot.as_ref().map(|x| x.1).unwrap_or(0),
                "lastExit": "none", "totalRuns": 3,
            }),
            "files" => json!({"ok": true, "files": d.files.keys().cloned().collect::<Vec<_>>()}),
            "storage" => json!({"totalBytes": 1000.0, "usedBytes": 100.0, "freeBytes": 900.0,
                "scriptCount": d.files.len(), "maxScriptBytes": 32768, "maxScripts": 64}),
            "files/delete" => match d.files.remove(&s(a, "name").unwrap_or_default()) {
                Some(_) => json!({"ok": true}),
                None => err("script not found"),
            },
            "files/chunk" => {
                d.chunk_calls += 1;
                let name = s(a, "name").unwrap_or_default();
                let off = n(a, "off").unwrap_or(0) as usize;
                let raw = B64.decode(s(a, "b64").unwrap_or_default()).unwrap();
                if off == 0 {
                    d.upload = Some((name.clone(), Vec::new()));
                }
                let Some((un, buf)) = d.upload.as_mut() else { return err("offset mismatch: expected 0") };
                if *un != name || buf.len() != off {
                    return err(&format!("offset mismatch: expected {}", buf.len()));
                }
                buf.extend_from_slice(&raw);
                let total = buf.len();
                if flag(a, "final") {
                    let (nm, bytes) = d.upload.take().unwrap();
                    d.files.insert(nm, bytes);
                }
                json!({"ok": true, "received": total, "final": flag(a, "final")})
            }
            "files/save" => {
                let t = text.unwrap_or("");
                d.files.insert(s(a, "name").unwrap(), t.as_bytes().to_vec());
                json!({"ok": true})
            }
            "run-file" => {
                let name = s(a, "name").unwrap_or_default();
                if d.slot.is_some() && !flag(a, "replace") {
                    return busy(&d, "a script is running; pass replace=1 to stop it");
                }
                if !d.files.contains_key(&name) {
                    return err("script not found");
                }
                let id = d.next_id;
                d.next_id += 1;
                d.slot = Some((name.clone(), id));
                d.executed += 1;
                json!({"ok": true, "id": id, "name": name, "background": flag(a, "background")})
            }
            "eval" => {
                if d.slot.is_some() {
                    return busy(&d, "a script is running; eval is refused until it ends");
                }
                let id = d.next_id;
                d.next_id += 1;
                d.executed += 1;
                d.last_persist = Some(flag(a, "persist"));
                json!({"ok": true, "id": id})
            }
            "lint" => {
                let src = s(a, "src").or_else(|| text.map(str::to_string)).or_else(|| {
                    s(a, "name").and_then(|nm| d.files.get(&nm).map(|b| String::from_utf8_lossy(b).into_owned()))
                });
                let Some(src) = src else { return err("script not found") };
                if d.lint_busy {
                    json!({"ok": false, "err": "Interpreter is busy running a script"})
                } else if src.contains("SYNTAX_ERROR") {
                    json!({"ok": false, "err": "  File \"<string>\", line 1\nSyntaxError: invalid syntax"})
                } else {
                    json!({"ok": true})
                }
            }
            "stop" => {
                d.stops += 1;
                d.slot = None;
                json!({"ok": true})
            }
            "reset" => {
                d.resets += 1;
                json!({"ok": true})
            }
            "autorun/status" => json!({
                "enabled": d.autorun.is_some(), "has_script": d.autorun.is_some(),
                "io12_high": true, "last_run_ok": false, "last_run_id": 0,
                "scriptName": d.autorun.clone().unwrap_or_default(), "ranThisBoot": false, "running": false,
            }),
            "autorun/enable" => match s(a, "name") {
                Some(nm) if d.files.contains_key(&nm) => {
                    d.autorun = Some(nm);
                    json!({"ok": true})
                }
                _ => err("script not found"),
            },
            "autorun/disable" => {
                d.autorun = None;
                json!({"ok": true})
            }
            "autorun/run" => {
                if d.slot.is_some() {
                    return busy(&d, "a script is running; autorun run-now is refused until it ends");
                }
                d.executed += 1;
                json!({"ok": true, "id": 99})
            }
            other => err(&format!("unknown path {other}")),
        }
    }

    fn log_page(&self, since: u64) -> (Vec<u8>, u64) {
        let d = self.dev.lock().unwrap();
        let end = d.log_start + d.log.len() as u64;
        let from = since.max(d.log_start).min(end);
        let take = ((end - from) as usize).min(LOG_PAGE);
        let off = (from - d.log_start) as usize;
        (d.log[off..off + take].to_vec(), from + take as u64)
    }
}

fn json_reply(v: &Value) -> HttpReply {
    let body = serde_json::to_vec(v).unwrap();
    HttpReply { status: crate::scripts::tunnel_status(&body), headers: HashMap::new(), body }
}

#[async_trait]
impl ScriptWire for Fake {
    fn kind(&self) -> Kind {
        self.kind
    }

    async fn http(&self, req: HttpExchange) -> Result<HttpReply, WireError> {
        *self.http_calls.lock().unwrap() += 1;
        if self.down {
            return Err(WireError::Transport("connection reset".into()));
        }
        let mut args = json!({});
        for (k, v) in &req.query {
            args[k] = json!(v);
        }
        let text = req.body.as_ref().map(|(b, _)| String::from_utf8(b.clone()).unwrap());
        let route = req.path.trim_start_matches("/api/scripts/").to_string();
        match (req.method, route.as_str()) {
            ("GET", "files/get") => {
                let name = s(&args, "name").unwrap_or_default();
                self.dev.lock().unwrap().calls.push("files/get".into());
                let file = self.dev.lock().unwrap().files.get(&name).cloned();
                return Ok(match file {
                    Some(b) => HttpReply { status: 200, headers: HashMap::new(), body: b },
                    None => HttpReply { status: 404, headers: HashMap::new(), body: br#"{"error":"script not found"}"#.to_vec() },
                });
            }
            ("GET", "logs") => {
                self.dev.lock().unwrap().calls.push("logs".into());
                let (data, next) = self.log_page(n(&args, "since").unwrap_or(0));
                let headers = HashMap::from([("x-bugbuster-log-next".to_string(), next.to_string())]);
                return Ok(HttpReply { status: 200, headers, body: data });
            }
            ("POST", "files") => return Ok(json_reply(&self.api("files/save", &args, text.as_deref()))),
            _ => {}
        }
        let v = self.api(&route, &args, text.as_deref());
        Ok(json_reply(&v))
    }

    async fn tunnel(&self, op: tunnel::Op, request: &[u8]) -> Result<Vec<u8>, WireError> {
        if self.unsupported {
            return Err(WireError::Unsupported("firmware has no USB scripting tunnel".into()));
        }
        if self.down {
            return Err(WireError::Transport("Command timeout (cmd=0xFD)".into()));
        }
        let a: Value = serde_json::from_slice(request).unwrap();
        let route = match op {
            tunnel::Op::Status => "status",
            tunnel::Op::Logs => "logs",
            tunnel::Op::Stop => "stop",
            tunnel::Op::Files => "files",
            tunnel::Op::Storage => "storage",
            tunnel::Op::Get => "files/get",
            tunnel::Op::Delete => "files/delete",
            tunnel::Op::Chunk => "files/chunk",
            tunnel::Op::Run => "run-file",
            tunnel::Op::Eval => "eval",
            tunnel::Op::Lint => "lint",
            tunnel::Op::AutorunStatus => "autorun/status",
            tunnel::Op::AutorunEnable => "autorun/enable",
            tunnel::Op::AutorunDisable => "autorun/disable",
            tunnel::Op::AutorunRun => "autorun/run",
            tunnel::Op::Reset => "reset",
            tunnel::Op::Caps => unreachable!(),
        };
        let v = match route {
            "files/get" => {
                self.dev.lock().unwrap().calls.push("files/get".into());
                let name = s(&a, "name").unwrap_or_default();
                let file = self.dev.lock().unwrap().files.get(&name).cloned();
                match file {
                    None => err("script not found"),
                    Some(b) => {
                        let off = (n(&a, "off").unwrap_or(0) as usize).min(b.len());
                        let len = (n(&a, "len").unwrap_or(FILE_PAGE as u64) as usize).min(FILE_PAGE);
                        let end = (off + len).min(b.len());
                        json!({"ok": true, "name": name, "size": b.len(), "off": off, "n": end - off,
                               "data": B64.encode(&b[off..end])})
                    }
                }
            }
            "logs" => {
                self.dev.lock().unwrap().calls.push("logs".into());
                let since = n(&a, "since").unwrap_or(0);
                let (data, next) = self.log_page(since);
                let start = next - data.len() as u64;
                json!({"ok": true, "n": data.len(), "next": next,
                       "dropped": start.saturating_sub(since), "data": B64.encode(&data)})
            }
            _ => self.api(route, &a, None),
        };
        Ok(serde_json::to_vec(&v).unwrap())
    }
}

fn noprog(_: &str, _: usize, _: usize) {}

async fn run(w: &Fake, op: &str, args: Value) -> Value {
    execute(w, op, &args, &noprog).await.unwrap()
}

const BOTH: [Kind; 2] = [Kind::Usb, Kind::Http];

fn calls(w: &Fake) -> Vec<String> {
    w.dev.lock().unwrap().calls.clone()
}

#[tokio::test]
async fn save_then_get_round_trips_on_both_transports() {
    // 'x' + 1000 x 2-byte chars: a 1536-byte chunk boundary splits a character.
    let source = format!("x{}", "\u{e9}".repeat(1000));
    for kind in BOTH {
        let w = fake(kind);
        let r = run(&w, "save", json!({"name": "new.py", "source": source})).await;
        assert_eq!(r["ok"], true, "{kind:?} {r}");
        assert_eq!(w.dev.lock().unwrap().files["new.py"], source.as_bytes());
        let g = run(&w, "get", json!({"name": "new.py"})).await;
        assert_eq!(g["source"], source, "{kind:?}");
        assert_eq!(g["size"], source.len());
        assert_eq!(g["lossy"], false);
        let chunks = calls(&w).iter().filter(|c| *c == "files/chunk").count();
        assert_eq!(chunks, if kind == Kind::Usb { 2 } else { 0 }, "{kind:?}");
    }
}

#[tokio::test]
async fn large_file_pages_through_get_and_matches_exactly() {
    let source: String = (0..2600).map(|i| format!("line_{i:04} = {i}\n")).collect();
    assert!(source.len() > 32 * 1024 / 2);
    let w = fake(Kind::Usb);
    w.dev.lock().unwrap().files.insert("big.py".into(), source.as_bytes().to_vec());
    let g = run(&w, "get", json!({"name": "big.py"})).await;
    assert_eq!(g["source"], source);
    let pages = calls(&w).iter().filter(|c| *c == "files/get").count();
    assert_eq!(pages, source.len().div_ceil(FILE_PAGE));
}

#[tokio::test]
async fn save_validates_before_touching_the_wire() {
    for kind in BOTH {
        let w = fake(kind);
        let bad_name = run(&w, "save", json!({"name": "../x.py", "source": "x"})).await;
        let empty = run(&w, "save", json!({"name": "a.py", "source": ""})).await;
        let huge = run(&w, "save", json!({"name": "a.py", "source": "x".repeat(32 * 1024 + 1)})).await;
        // The limit is in UTF-8 bytes, not characters.
        let multibyte = run(&w, "save", json!({"name": "a.py", "source": "\u{e9}".repeat(16 * 1024 + 1)})).await;
        assert_eq!(bad_name["kind"], "invalid");
        assert_eq!(empty["kind"], "invalid");
        assert_eq!(huge["kind"], "too_large");
        assert_eq!(multibyte["kind"], "too_large");
        assert!(calls(&w).is_empty(), "{kind:?} touched the wire: {:?}", calls(&w));
        let exact = run(&w, "save", json!({"name": "a.py", "source": "x".repeat(32 * 1024)})).await;
        assert_eq!(exact["ok"], true, "{kind:?} {exact}");
    }
}

#[tokio::test]
async fn usb_save_restarts_once_after_a_lost_reply_and_keeps_the_old_file_when_it_fails() {
    // The 3000-byte body needs 2 chunks; the first reply is lost, the restart succeeds.
    let source = "a".repeat(3000);
    let w = fake(Kind::Usb);
    w.dev.lock().unwrap().lose_reply_on_chunk = vec![1];
    // The fake loses replies by failing the tunnel call after applying it.
    let r = run_lossy(&w, "save", json!({"name": "new.py", "source": source})).await;
    assert_eq!(r["ok"], true, "{r}");
    assert_eq!(w.dev.lock().unwrap().files["new.py"], source.as_bytes());
    assert_eq!(w.dev.lock().unwrap().chunk_calls, 3);

    let w = fake(Kind::Usb);
    w.dev.lock().unwrap().lose_reply_on_chunk = vec![1, 2];
    let r = run_lossy(&w, "save", json!({"name": "old.py", "source": source})).await;
    assert_eq!(r["ok"], false);
    assert_eq!(r["kind"], "transport");
    assert_eq!(w.dev.lock().unwrap().files["old.py"], b"print('old')\n", "incomplete upload replaced the file");
}

/// Same as `execute`, but chunk replies listed in `lose_reply_on_chunk` fail after the device applied them.
async fn run_lossy(w: &Fake, op: &str, args: Value) -> Value {
    struct Lossy<'a>(&'a Fake);
    #[async_trait]
    impl ScriptWire for Lossy<'_> {
        fn kind(&self) -> Kind {
            self.0.kind
        }
        async fn http(&self, req: HttpExchange) -> Result<HttpReply, WireError> {
            self.0.http(req).await
        }
        async fn tunnel(&self, op: tunnel::Op, request: &[u8]) -> Result<Vec<u8>, WireError> {
            let out = self.0.tunnel(op, request).await?;
            if op == tunnel::Op::Chunk {
                let d = self.0.dev.lock().unwrap();
                if d.lose_reply_on_chunk.contains(&d.chunk_calls) {
                    return Err(WireError::Transport("Command timeout (cmd=0xFD)".into()));
                }
            }
            Ok(out)
        }
    }
    execute(&Lossy(w), op, &args, &noprog).await.unwrap()
}

#[tokio::test]
async fn upload_progress_reaches_the_total() {
    let source = "b".repeat(4000);
    let seen = Mutex::new(Vec::new());
    let progress = |name: &str, sent: usize, total: usize| seen.lock().unwrap().push((name.to_string(), sent, total));
    let w = fake(Kind::Usb);
    execute(&w, "save", &json!({"name": "p.py", "source": source}), &progress).await.unwrap();
    let seen = seen.into_inner().unwrap();
    assert_eq!(seen.last().unwrap(), &("p.py".to_string(), 4000, 4000));
    assert!(seen.windows(2).all(|p| p[0].1 < p[1].1));
}

#[tokio::test]
async fn lint_separates_syntax_busy_and_missing() {
    for kind in BOTH {
        let w = fake(kind);
        let ok = run(&w, "lint", json!({"source": "print(1)\n"})).await;
        assert_eq!((ok["ok"].clone(), ok["valid"].clone()), (json!(true), json!(true)), "{kind:?}");

        let bad = run(&w, "lint", json!({"source": "SYNTAX_ERROR"})).await;
        assert_eq!(bad["ok"], true);
        assert_eq!(bad["valid"], false);
        assert!(bad["message"].as_str().unwrap().contains("SyntaxError"), "{kind:?}");

        w.dev.lock().unwrap().files.insert("bad.py".into(), b"SYNTAX_ERROR".to_vec());
        let by_name = run(&w, "lint", json!({"name": "bad.py"})).await;
        assert_eq!(by_name["valid"], false, "{kind:?} {by_name}");

        let missing = run(&w, "lint", json!({"name": "nope.py"})).await;
        assert_eq!(missing["kind"], "not_found", "{kind:?}");

        w.dev.lock().unwrap().lint_busy = true;
        let busy = run(&w, "lint", json!({"source": "print(1)"})).await;
        assert_eq!((busy["ok"].clone(), busy["kind"].clone()), (json!(false), json!("busy")), "{kind:?}");

        assert_eq!(w.dev.lock().unwrap().executed, 0, "lint must never execute code");
        let none = execute(&w, "lint", &json!({}), &noprog).await.unwrap();
        assert_eq!(none["kind"], "invalid");
    }
}

#[tokio::test]
async fn run_reports_busy_distinctly_and_is_never_retried() {
    for kind in BOTH {
        let w = fake(kind);
        w.dev.lock().unwrap().slot = Some(("held.py".into(), 7));
        let r = run(&w, "run", json!({"name": "old.py"})).await;
        assert_eq!(r["ok"], false);
        assert_eq!(r["kind"], "busy", "{kind:?}");
        assert_eq!((r["running"].clone(), r["id"].clone()), (json!("held.py"), json!(7)));
        assert_eq!(calls(&w), ["run-file"], "{kind:?} retried a run");

        let replaced = run(&w, "run", json!({"name": "old.py", "replace": true, "background": true})).await;
        assert_eq!(replaced["ok"], true, "{kind:?} {replaced}");
        assert_eq!(replaced["name"], "old.py");
        assert_eq!(replaced["background"], true);
        assert_eq!(w.dev.lock().unwrap().slot.as_ref().unwrap().0, "old.py");

        w.dev.lock().unwrap().slot = None;
        assert_eq!(run(&w, "run", json!({"name": "ghost.py"})).await["kind"], "not_found");
        assert_eq!(run(&w, "run", json!({"name": "bad name"})).await["kind"], "invalid");
    }
}

#[tokio::test]
async fn eval_is_refused_while_a_file_script_holds_the_interpreter() {
    for kind in BOTH {
        let w = fake(kind);
        let ok = run(&w, "eval", json!({"source": "1+1"})).await;
        assert_eq!(ok["ok"], true, "{kind:?}");
        assert_eq!(w.dev.lock().unwrap().last_persist, Some(true), "persist defaults on like the iOS REPL");
        run(&w, "eval", json!({"source": "1+1", "persist": false})).await;
        assert_eq!(w.dev.lock().unwrap().last_persist, Some(false));

        w.dev.lock().unwrap().slot = Some(("held.py".into(), 4));
        let busy = run(&w, "eval", json!({"source": "1+1"})).await;
        assert_eq!(busy["kind"], "busy");
        assert_eq!(busy["running"], "held.py");
        assert_eq!(run(&w, "eval", json!({"source": ""})).await["kind"], "invalid");
    }
}

#[tokio::test]
async fn logs_are_cursor_based_non_draining_and_report_dropped_bytes() {
    for kind in BOTH {
        let w = fake(kind);
        {
            let mut d = w.dev.lock().unwrap();
            d.log_start = 100;
            d.log = (0..50u8).map(|i| b'a' + i % 26).collect();
        }
        let first = run(&w, "logs", json!({"since": 0})).await;
        assert_eq!((first["next"].clone(), first["dropped"].clone(), first["n"].clone()), (json!(150), json!(100), json!(50)), "{kind:?}");
        let again = run(&w, "logs", json!({"since": 0})).await;
        assert_eq!(first["data"], again["data"], "{kind:?} drained the ring");
        let tail = run(&w, "logs", json!({"since": 120})).await;
        assert_eq!((tail["n"].clone(), tail["dropped"].clone()), (json!(30), json!(0)));
        assert_eq!(B64.decode(tail["data"].as_str().unwrap()).unwrap(), w.dev.lock().unwrap().log[20..]);
        let caught_up = run(&w, "logs", json!({"since": 150})).await;
        assert_eq!(caught_up["restarted"], false);
        // A cursor ahead of the device counter means it rebooted.
        let rebooted = run(&w, "logs", json!({"since": 9000})).await;
        assert_eq!((rebooted["restarted"].clone(), rebooted["n"].clone(), rebooted["next"].clone()), (json!(true), json!(0), json!(150)));
        assert_eq!((caught_up["n"].clone(), caught_up["next"].clone(), caught_up["more"].clone()), (json!(0), json!(150), json!(false)));

        w.dev.lock().unwrap().log = vec![b'z'; 5000];
        w.dev.lock().unwrap().log_start = 0;
        let page = run(&w, "logs", json!({"since": 0})).await;
        assert_eq!((page["n"].clone(), page["next"].clone(), page["more"].clone()), (json!(4096), json!(4096), json!(true)));
    }
}

#[tokio::test]
async fn http_logs_without_a_cursor_header_mean_old_firmware() {
    struct NoHeader(Fake);
    #[async_trait]
    impl ScriptWire for NoHeader {
        fn kind(&self) -> Kind {
            Kind::Http
        }
        async fn http(&self, _: HttpExchange) -> Result<HttpReply, WireError> {
            Ok(HttpReply { status: 200, headers: HashMap::new(), body: b"drained".to_vec() })
        }
        async fn tunnel(&self, op: tunnel::Op, r: &[u8]) -> Result<Vec<u8>, WireError> {
            self.0.tunnel(op, r).await
        }
    }
    let v = execute(&NoHeader(fake(Kind::Http)), "logs", &json!({"since": 0}), &noprog).await.unwrap();
    assert_eq!(v["kind"], "unsupported");
}

#[tokio::test]
async fn unsupported_usb_firmware_is_explicit_and_never_uses_http() {
    let mut w = fake(Kind::Usb);
    w.unsupported = true;
    for op in ["status", "files", "storage"] {
        let r = run(&w, op, json!({})).await;
        assert_eq!((r["ok"].clone(), r["kind"].clone()), (json!(false), json!("unsupported")), "{op}");
    }
    assert_eq!(*w.http_calls.lock().unwrap(), 0);

    let h = fake(Kind::Http);
    let r = run(&h, "autorun_run", json!({})).await;
    assert_eq!(r["kind"], "unsupported");
    assert!(calls(&h).is_empty(), "run-now has no Wi-Fi route and must not be attempted");
}

#[tokio::test]
async fn transport_failures_flag_unknown_outcome_only_for_state_changing_requests() {
    for kind in BOTH {
        let mut w = fake(kind);
        w.down = true;
        let run_r = run(&w, "run", json!({"name": "old.py"})).await;
        assert_eq!((run_r["kind"].clone(), run_r["outcomeUnknown"].clone()), (json!("transport"), json!(true)), "{kind:?}");
        let eval_r = run(&w, "eval", json!({"source": "1"})).await;
        assert_eq!(eval_r["outcomeUnknown"], true);
        let status = run(&w, "status", json!({})).await;
        assert_eq!(status["kind"], "transport");
        assert!(status.get("outcomeUnknown").is_none());
        let lint = run(&w, "lint", json!({"source": "1"})).await;
        assert!(lint.get("outcomeUnknown").is_none());
    }
}

#[tokio::test]
async fn status_files_storage_autorun_stop_reset_and_delete_shapes() {
    for kind in BOTH {
        let w = fake(kind);
        let st = run(&w, "status", json!({})).await;
        assert_eq!((st["ok"].clone(), st["state"].clone(), st["totalRuns"].clone()), (json!(true), json!("idle"), json!(3)));
        assert_eq!(run(&w, "files", json!({})).await["files"], json!(["old.py"]));
        assert_eq!(run(&w, "storage", json!({})).await["maxScriptBytes"], 32768);

        assert_eq!(run(&w, "autorun_enable", json!({"name": "old.py"})).await["ok"], true, "{kind:?}");
        let a = run(&w, "autorun_status", json!({})).await;
        assert_eq!((a["enabled"].clone(), a["scriptName"].clone()), (json!(true), json!("old.py")));
        assert_eq!(run(&w, "autorun_enable", json!({"name": "ghost.py"})).await["kind"], "not_found");
        assert_eq!(run(&w, "autorun_enable", json!({"name": "x"})).await["kind"], "invalid");
        run(&w, "autorun_disable", json!({})).await;
        assert_eq!(run(&w, "autorun_status", json!({})).await["enabled"], false);

        w.dev.lock().unwrap().slot = Some(("old.py".into(), 2));
        assert_eq!(run(&w, "stop", json!({})).await["ok"], true);
        assert!(w.dev.lock().unwrap().slot.is_none());
        assert_eq!(run(&w, "reset", json!({})).await["ok"], true);
        assert_eq!(w.dev.lock().unwrap().resets, 1);

        assert_eq!(run(&w, "delete", json!({"name": "old.py"})).await["ok"], true);
        assert_eq!(run(&w, "delete", json!({"name": "old.py"})).await["kind"], "not_found");
        assert_eq!(run(&w, "get", json!({"name": "old.py"})).await["kind"], "not_found");
    }
}

#[tokio::test]
async fn autorun_run_now_is_usb_only_and_respects_the_busy_slot() {
    let w = fake(Kind::Usb);
    let ok = run(&w, "autorun_run", json!({})).await;
    assert_eq!((ok["ok"].clone(), ok["id"].clone()), (json!(true), json!(99)));
    w.dev.lock().unwrap().slot = Some(("held.py".into(), 3));
    let busy = run(&w, "autorun_run", json!({})).await;
    assert_eq!(busy["kind"], "busy");
}

#[tokio::test]
async fn unknown_operations_are_a_caller_error() {
    let w = fake(Kind::Usb);
    assert!(execute(&w, "format_flash", &json!({}), &noprog).await.is_err());
}

#[test]
fn only_state_changing_requests_get_the_long_run_now_timeout() {
    assert_eq!(autorun_timeout_ms(&[6, tunnel::Op::AutorunRun as u8, 1, 0]), 40_000);
    assert_eq!(autorun_timeout_ms(&[6, tunnel::Op::Lint as u8, 1, 0]), 15_000);
    assert_eq!(autorun_timeout_ms(&[7, 1, 0, 0]), 2_000);
    assert_eq!(autorun_timeout_ms(&[0]), 2_000);
    assert_eq!(autorun_timeout_ms(&[]), 2_000);
}

// --- tunnel framing against a port of the firmware state machine -------------------------

type Handler = fn(u8, &str) -> Option<String>;

struct FakeBbp {
    st: Mutex<BbpSt>,
    handler: Handler,
    /// Error text returned for every Nth (1-based) frame sent.
    fail_on_frame: Option<(usize, String)>,
    sent: Mutex<Vec<Vec<u8>>>,
    corrupt_fetch_token: bool,
}

#[derive(Default)]
struct BbpSt {
    req: Option<(u8, usize, Vec<u8>)>,
    rsp: Option<Vec<u8>>,
    token: u8,
}

fn frame(state: u8, token: u8, a: u16, b: u16, data: &[u8]) -> Vec<u8> {
    let mut v = vec![state, token];
    v.extend_from_slice(&a.to_le_bytes());
    v.extend_from_slice(&b.to_le_bytes());
    v.extend_from_slice(data);
    v
}

impl FakeBbp {
    fn new(handler: Handler) -> Self {
        Self { st: Mutex::default(), handler, fail_on_frame: None, sent: Mutex::default(), corrupt_fetch_token: false }
    }
    fn page(st: &BbpSt, off: usize) -> Vec<u8> {
        let rsp = st.rsp.as_ref().unwrap();
        let n = (rsp.len() - off).min(1000);
        frame(tunnel::ST_DONE, st.token, rsp.len() as u16, n as u16, &rsp[off..off + n])
    }
}

#[async_trait]
impl BbpSend for FakeBbp {
    async fn bbp(&self, cmd: u8, payload: &[u8]) -> anyhow::Result<Vec<u8>> {
        assert_eq!(cmd, crate::bbp::CMD_SCRIPT_AUTORUN);
        assert!(payload.len() <= 1018, "frame exceeds the BBP payload limit");
        self.sent.lock().unwrap().push(payload.to_vec());
        let idx = self.sent.lock().unwrap().len();
        if let Some((n, msg)) = &self.fail_on_frame {
            if *n == idx {
                return Err(anyhow::anyhow!(msg.clone()));
            }
        }
        let mut st = self.st.lock().unwrap();
        if payload[0] == tunnel::SUB_FETCH {
            let (token, off) = (payload[1], u16::from_le_bytes([payload[2], payload[3]]) as usize);
            if st.rsp.is_none() || token != st.token {
                return Ok(frame(tunnel::ST_REJECT, st.token, tunnel::REJ_STALE, 0, &[]));
            }
            let mut page = Self::page(&st, off);
            if self.corrupt_fetch_token {
                page[1] = page[1].wrapping_add(1);
            }
            return Ok(page);
        }
        let (op, total, off) = (payload[1], u16::from_le_bytes([payload[2], payload[3]]) as usize, u16::from_le_bytes([payload[4], payload[5]]) as usize);
        let chunk = &payload[6..];
        if off == 0 {
            st.token = st.token.wrapping_add(1);
            st.rsp = None;
            st.req = Some((op, total, Vec::new()));
        }
        let token = st.token;
        let Some((rop, rtotal, buf)) = st.req.as_mut() else {
            return Ok(frame(tunnel::ST_REJECT, token, tunnel::REJ_NO_REQUEST, 0, &[]));
        };
        if *rop != op || *rtotal != total || buf.len() != off {
            let have = buf.len() as u16;
            return Ok(frame(tunnel::ST_REJECT, token, tunnel::REJ_OFFSET, have, &[]));
        }
        buf.extend_from_slice(chunk);
        if buf.len() < total {
            return Ok(frame(tunnel::ST_MORE, token, buf.len() as u16, 0, &[]));
        }
        let text = String::from_utf8(buf.clone()).unwrap();
        st.req = None;
        match (self.handler)(op, &text) {
            None => Ok(frame(tunnel::ST_REJECT, token, tunnel::REJ_UNKNOWN_OP, op as u16, &[])),
            Some(r) => {
                st.rsp = Some(r.into_bytes());
                Ok(Self::page(&st, 0))
            }
        }
    }
}

fn echo(op: u8, req: &str) -> Option<String> {
    (op < 17).then(|| format!("{{\"op\":{op},\"echo\":{req}}}"))
}

#[tokio::test]
async fn tunnel_stages_a_multi_frame_request_and_pages_the_reply() {
    let bbp = FakeBbp::new(echo);
    let body = format!("\"{}\"", "q".repeat(2498));
    let out = tunnel_exchange(&bbp, tunnel::Op::Eval, body.as_bytes()).await.unwrap();
    assert_eq!(String::from_utf8(out).unwrap(), format!("{{\"op\":10,\"echo\":{body}}}"));
    // 3 CALL frames (1000 + 1000 + 500 bytes) then 2 FETCH pages for the 2517-byte reply.
    let sent = bbp.sent.lock().unwrap();
    let subs: Vec<u8> = sent.iter().map(|p| p[0]).collect();
    assert_eq!(subs, [6, 6, 6, 7, 7]);
    assert_eq!(&sent[0][..6], &[6, 10, 0xC4, 0x09, 0, 0]);       // total 2500 = 0x09C4, off 0
    assert_eq!(&sent[1][4..6], &1000u16.to_le_bytes());
    assert_eq!(&sent[3][2..4], &1000u16.to_le_bytes());          // fetch starts after the first page
}

#[tokio::test]
async fn tunnel_single_frame_needs_no_fetch() {
    let bbp = FakeBbp::new(echo);
    let out = tunnel_exchange(&bbp, tunnel::Op::Status, b"{}").await.unwrap();
    assert_eq!(out, br#"{"op":1,"echo":{}}"#);
    assert_eq!(bbp.sent.lock().unwrap().len(), 1);
}

#[tokio::test]
async fn tunnel_maps_old_firmware_and_reject_reasons() {
    let mut old = FakeBbp::new(echo);
    old.fail_on_frame = Some((1, "invalid parameter (error 0x03, cmd 0xFD)".into()));
    let e = tunnel_exchange(&old, tunnel::Op::Status, b"{}").await.unwrap_err();
    assert!(matches!(e, WireError::Unsupported(_)), "{e:?}");

    // The same BBP error on a later frame is a framing bug, not missing support.
    let mut later = FakeBbp::new(echo);
    later.fail_on_frame = Some((2, "invalid parameter (error 0x03, cmd 0xFD)".into()));
    let e = tunnel_exchange(&later, tunnel::Op::Eval, "x".repeat(1500).as_bytes()).await.unwrap_err();
    assert!(matches!(e, WireError::Transport(_)), "{e:?}");

    let mut timeout = FakeBbp::new(echo);
    timeout.fail_on_frame = Some((1, "Command timeout (cmd=0xFD)".into()));
    assert!(matches!(tunnel_exchange(&timeout, tunnel::Op::Status, b"{}").await.unwrap_err(), WireError::Transport(_)));

    let unknown = FakeBbp::new(|_, _| None);
    assert!(matches!(tunnel_exchange(&unknown, tunnel::Op::Reset, b"{}").await.unwrap_err(), WireError::Unsupported(_)));
}

#[tokio::test]
async fn tunnel_rejects_oversize_requests_without_sending() {
    let bbp = FakeBbp::new(echo);
    let e = tunnel_exchange(&bbp, tunnel::Op::Eval, &vec![b'x'; 65536]).await.unwrap_err();
    assert!(matches!(e, WireError::TooLarge(_)));
    assert!(matches!(tunnel_exchange(&bbp, tunnel::Op::Eval, &[]).await.unwrap_err(), WireError::TooLarge(_)));
    assert!(bbp.sent.lock().unwrap().is_empty());
}

#[tokio::test]
async fn tunnel_detects_a_page_from_another_reply() {
    let mut bbp = FakeBbp::new(echo);
    bbp.corrupt_fetch_token = true;
    let e = tunnel_exchange(&bbp, tunnel::Op::Eval, "y".repeat(1500).as_bytes()).await.unwrap_err();
    assert!(matches!(e, WireError::Protocol(_)), "{e:?}");
}

#[test]
fn run_and_eval_share_a_single_in_flight_slot() {
    // The command layer's guard: a second launch while one is outstanding is refused.
    let first = crate::scripts::launch_guard_for_test();
    assert!(first.is_some());
    assert!(crate::scripts::launch_guard_for_test().is_none());
    drop(first);
    assert!(crate::scripts::launch_guard_for_test().is_some());
}
