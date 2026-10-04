# Hub HEALTH record (format v1)

The S3 sends one hardware-health record to the ESPFleet hub: **60 s after the hub
link first comes up** (baseline for every firmware build), then **once an hour**
(`HUB_HEALTH_INTERVAL_MS` in `Firmware/ESP32/src/hub/hub_health.h`). If the hub
is offline the record stays in the log ring and is backfilled with the logs.
Nothing new is added to the hub transport.

## Transport

The record rides the existing log shipper (`POST /api/v1/ingest/logs`), exactly
like the boot report's `BOOTRPT` lines (`Docs/http-api.md`, section 2b). Each
record becomes 1..n log entries:

| Entry field | Value |
| :--- | :--- |
| `source` | `s3` |
| `level` | `I` (always shipped: not subject to the S3 log-level threshold or the per-message rate limiter) |
| `tag` | `health` |
| `msg` | `HEALTH <boot> <seq> <i>/<n> <json-part>` |

* `<boot>` boot counter, `<seq>` 1-based record number within the boot (the pair
  identifies a record), `<i>/<n>` 1-based part index / part count.
* `<json-part>` is at most 360 bytes of the JSON document; concatenate parts
  `1..n` in order to get the document. A record is normally 2 parts (~600-700 B).
* Entry `ts` is the wall-clock time the entry was queued (the hub's normal log
  timestamp). Use it when the JSON `ts` is `0`.
* A record is lost only if the 64 KB log ring overflows while the hub is offline
  (oldest entries go first, and a ring-drop warning is logged).

## JSON document

```json
{"kind":"health","v":1,"seq":1,"trig":"boot","ts":1790000000,"up_ms":61234,
 "dev":"7c9ebd123456","fw":"2.4.1","elf":"a1b2c3d4e5f60718","idf":"v5.4.1","boot":7,
 "reset":{"reason":"POWERON","code":1,"abnormal":false},
 "heap":{"int":{"free":112000,"min":98000,"big":65536},"psram":{"free":7000000,"min":6900000,"big":6500000}},
 "stacks":{"adcPoll":1800,"faultMon":900,"cmdProc":1200,"wavegen":1400,"mainLoop":2100,"bbpCli":2300,"uPython":9000,"hub":2500},
 "cnt":{"coredump":0,"hat_to":0,"hat_streak":0,"hat_degraded":0,"hub_fail":0,"log_drop":0,"script_drop":0,"wifi_reconn":0}}
```

| Key | Meaning |
| :--- | :--- |
| `kind`, `v` | `"health"`, schema version (`1`). New keys may be added within a version; parsers must ignore unknown keys. |
| `seq` | Record number within this boot, from 1. |
| `trig` | `"boot"` for the baseline record (seq 1), `"hourly"` afterwards. |
| `ts` | Unix seconds when the record was built, `0` if the wall clock was not set yet. |
| `up_ms` | S3 uptime in ms when built (monotonic within a boot; resets on reboot). |
| `dev` | Device id: 12 lowercase hex, the STA MAC (same as `X-Device-Id`). |
| `fw`, `elf`, `idf` | Same as boot report `sys`: firmware version, ELF sha256 prefix (build id: compare firmware builds with this), ESP-IDF version. |
| `boot` | Boot counter (RTC breadcrumb, cleared on power-on). |
| `reset` | Last reset: `reason` name, raw `code`, `abnormal` (panic/WDT/brownout). |
| `heap.int` / `heap.psram` | Bytes: `free` now, `min` lowest ever, `big` largest free block. Internal = 8-bit-capable internal RAM. |
| `stacks` | Task name -> minimum free stack bytes ever (FreeRTOS high-water mark). `null` = task not running. Tasks: the registry in `tasks.cpp` (`adcPoll faultMon cmdProc wavegen mainLoop bbpCli uPython`) plus `hub`. |
| `cnt.coredump` | 1 if a valid coredump is stored in flash. |
| `cnt.hat_to`, `hat_streak`, `hat_degraded` | DAQ HAT UART command timeouts since boot, current consecutive timeouts, link degraded flag. |
| `cnt.hub_fail` | Failed hub exchanges since boot. |
| `cnt.log_drop` | Log entries lost in the shipper (ring overwrite + producer busy) since boot. |
| `cnt.script_drop` | MicroPython log lines dropped (busy) since boot. |
| `cnt.wifi_reconn` | Wi-Fi STA disconnect events since boot. |

All `cnt` values are cumulative since boot; chart deltas per `boot`.

## Not included (follow-up)

DAQ HAT **P4 / C6** health (heap, task stacks, resets) is not exposed over the
existing S3-P4 link (only the P4 uptime in the log pull and the S3-side link
counters above), so it is not in v1. It would need a new HAT status command.

## Tests

`tests/firmware_host/test_hub_health.py` compiles `hub_health.c` on the host and
checks the document, chunking and schedule.
