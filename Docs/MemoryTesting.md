# BugBuster ESP32 Memory Safety Guide

> How to detect SRAM exhaustion, stack overflows, and heap fragmentation before shipping firmware.

**Live figures — fw 5.0.0, measured on hardware under load (2026-08-06):**
internal SRAM 54.4 KB free of 250.0 KB, min-ever 44.5 KB, largest contiguous
block 36.0 KB, fragmentation 33.8 %. Static DRAM 84.8 KB against the 160 KB
gate; build RAM 26.5 %, flash 50.1 %.

---

## Memory Layout (ESP32-S3 N16R8)

```
┌─ Internal SRAM (320 KB addressable, ~250 KB in the app heap pool) ─┐
│                                                                    │
│  FreeRTOS kernel + TCBs          ~5–8 KB                           │
│  WiFi/LWIP state (Core 0)        ~30 KB                            │
│  BBP static frame buffers        ~5.2 KB   msg/rsp/cobs/rx/decoded │
│  BBP ADC stream ring             5 124 B                           │
│  g_deviceState                   4 152 B                           │
│  Waveform sine LUT               1 024 B                           │
│  Static task stacks (.bss):                                        │
│    mainLoop   5 120 B    bbpCli     5 120 B                        │
│  Heap-allocated task stacks (Core 1):                              │
│    adcPoll    2 560 B    faultMon   2 560 B                        │
│    cmdProc    3 072 B    wavegen    2 048 B                        │
│  SPIRAM_MALLOC_RESERVE (floor)    49 KB                            │
│  ──────────────────────────────────────                            │
│  Free under load                 ~54 KB                            │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
         ↓ malloc ≥ 1 KB goes here ↓
┌─ PSRAM (8 MB octal, mapped at 0x3C……) ──────────────────────────────┐
│  MicroPython heap                1 MB (allocated once at boot)     │
│  g_scopeBuf                      14 400 B   EXT_RAM_BSS_ATTR       │
│  ws_stream + repl_ws TX rings    12 288 B   EXT_RAM_BSS_ATTR       │
│  CLI history, MP log ring, script name tables                      │
│  HTTP/TLS buffers                dynamic                           │
│  PSRAM task stacks: wifi_rc, cli_update, repl_tx                   │
│  Free typically                  ~7 MB                             │
└────────────────────────────────────────────────────────────────────┘

Flash: 16 MB — partitions.csv layout:
  app0 / app1 (OTA A/B)   4 MB each   (firmware is ~2.1 MB → 1.9 MB headroom)
  spiffs (web UI)          4 MB
  scripts (MicroPython)    3 MB
  nvs + otadata + coredump remainder
```

Task stack sizes are defined once in `Firmware/ESP32/src/tasks.h`
(`TASK_STACK_*`) and pinned by `tests/unit/test_direct_daq_ota.py`. The comment
block above those macros records the measured peak justifying each size — read
it before changing one.

**Critical rule — PSRAM stacks are unsafe during OTA/NVS/SPIFFS writes.**  
D-cache is disabled for those operations, which silently corrupts PSRAM-backed stacks on both cores regardless of whether the task touches flash itself. Tasks that run during OTA (`wifi_rc`, `cli_update`, `repl_tx`) accept this risk; measurement tasks (`adcPoll`, `faultMon`, `wavegen`, `cmdProc`) must use internal SRAM stacks. The constraint applies to **stacks**, not data buffers — payload staging lives in PSRAM safely.

---

## Where internal DRAM actually goes

Before optimising, dump the symbols and check the address. Internal DRAM is
`0x3FC……` (decimal `107……`); `0x3C……` (decimal `100……`) is PSRAM and costs no
internal RAM:

```powershell
$NM="$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp-elf\bin\xtensa-esp-elf-nm.exe"
$ELF="Firmware\ESP32\.pio\build\esp32s3\firmware.elf"
& $NM --print-size --size-sort --radix=d $ELF |
    Where-Object { $_ -match '\s[bBdD]\s' -and $_ -match '^107' } |
    Select-Object -Last 40
```

Two traps have each cost several KB on this project:

**`EXT_RAM_BSS_ATTR` is silently ignored on function-scope statics.** A
`static uint8_t buf[1500];` declared *inside* a function lands in internal DRAM
regardless of the attribute. In an `nm` dump these appear as mangled
`_ZZL<function>E<name>` symbols — one at a `0x3FC……` address is this bug. Move
the buffer to file scope. Four instances were found this way, 8 216 B combined.

**A `const` table that gets `memcpy`'d loses its free ride.** Per-subsystem
`static const CmdDescriptor[]` blocks live in flash `.rodata` at zero internal
cost — until `cmd_registry.cpp` copied all of them into a `CmdDescriptor[256]`
in `.bss`, 8 192 B for 143 commands. Store pointers to const tables; do not copy
them.

Both are pinned by `tests/unit/test_memory_telemetry.py`.

---

## Pre-Ship Memory Checks

### 1. Binary Size Gate (post-build, automatic)

`Firmware/tools/check_memory.py` runs automatically after every `pio run` build. It:

- Reads the firmware ELF via `xtensa-esp-elf-size -A`
- Checks `.dram0.data` + `.dram0.bss` static SRAM usage against a configurable limit
- Checks `firmware.bin` size against the OTA partition size from `partitions.csv`
- Prints a memory report and **fails the build** if either limit is breached

To run manually:

```bash
cd Firmware/ESP32
pio run -e esp32s3          # build triggers the check automatically
# Or run directly after a build:
python tools/check_memory.py .pio/build/esp32s3/firmware.elf partitions.csv
```

Thresholds (edit at the top of `check_memory.py`):

| Check | Default limit | Rationale |
|-------|--------------|-----------|
| Static DRAM (`.data` + `.bss`) | 160 KB | Current baseline is 84.8 KB; gate catches large regressions |
| Firmware binary vs OTA partition | 90% of partition | ~1.9 MB headroom at the 4 MB partition |

### 2. Live memory telemetry (runtime, all surfaces)

`tasks_get_registry()` in `tasks.h`/`tasks.cpp` is the **single** task table read
by all three surfaces — if they ever disagree, that is a bug worth chasing
before trusting any of them:

| Surface | How |
|---|---|
| Serial CLI | `heap` and `stack_hwm` |
| BBP (USB) | `MEM_STATUS` (0x0C) → `client.get_memory_status()` |
| HTTP | `GET /api/system/memory` (admin token) |
| MCP | `device_memory` tool |
| Dashboard / CI | `tests/tools/mem_watch.py` |

```powershell
# Live dashboard, under load, with a pass/fail gate
PYTHONPATH=python python tests/tools/mem_watch.py --device-usb COM6 --stress `
    --duration 30 --fail-under-kb 24 --fail-largest-under-kb 8 --fail-task-pct 80
```

`MemoryStatus` (`python/bugbuster/memory.py`) exposes per-pool `free_bytes`,
`min_ever_bytes`, `largest_block_bytes`, `total_bytes` plus derived `used_pct`,
`fragmentation_pct`, `worst_task` and `warnings()`.

**`--stress` matters. Idle numbers lie** — it starts the ADC and scope streams
while sampling.

### 3. Stack High-Water Mark Reporting (runtime)

`tasks_log_stack_hwm()` calls `uxTaskGetStackHighWaterMark()` on every task
handle and logs the result. Reading the output:

```
  task        declared   unused     peak-used
  adcPoll       2560     1304     1256
  faultMon      2560     1236     1324
  cmdProc       3072     2248      824
  wavegen       2048     1180      868
  mainLoop      5120     3044     2076
  bbpCli        5120     2920     2200
```

> **Units trap that cost a real bug:** on ESP-IDF,
> `uxTaskGetStackHighWaterMark()` returns **bytes**, not words. The thresholds
> were once written against word-scaled values (`< 128` / `< 256`), so a task
> with 380 bytes free — 81 % of its stack consumed — logged as healthy.
> Thresholds are now 512 / 1024 **bytes**.

**Red flags:**
- Any task below **1024 bytes** free → investigate before shipping
- Any task below **512 bytes** → immediate overflow risk, ship-blocker

> **High-water marks grow late.** A reading taken 5 s after boot is not the
> peak. `cmdProc` measured 824 B at a fresh boot, 1256 B after ordinary BBP
> traffic, and 1672 B after a single USB connect/disconnect cycle — two earlier
> "right-sizing" passes recorded 832 B and left the stack 376 B from empty.
> Exercise the deep paths (connect/disconnect, HTTP, the CLI `tui`, a
> `DEVICE_RESET`, an OTA query) before trusting a number.

> **Getting a real reboot:** `client.reset()` and the CLI `reset` pulse the
> **AD74416H** reset pin — they do not restart the ESP32 (uptime keeps
> climbing), so high-water marks never clear. To force a genuine reboot for a
> clean measurement, re-run an OTA upload; the device reboots after a 1 s grace
> period. Confirm via `MemoryStatus.uptime_ms`.

### 4. Stack Overflow Hook (compile-time canary)

`sdkconfig.defaults` enables `CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER=y` which wraps task functions and catches some corruption, but for hard stack overflows you need the FreeRTOS overflow hook.

Enable in `sdkconfig.defaults`:

```
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
```

This writes a canary word at the bottom of every task stack and checks it in the context-switch ISR. On overflow it calls:

```c
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    ESP_LOGE("FAULT", "STACK OVERFLOW in task: %s", pcTaskName);
    abort();
}
```

The `abort()` captures a core dump to the `coredump` partition (256 KB reserved in `partitions.csv`). The dump is readable over serial with `esptool.py`.

### 5. Heap Fragmentation Monitoring

`esp_get_minimum_free_heap_size()` returns the all-time low-water mark of internal free heap since boot. Log it regularly:

```c
ESP_LOGI("heap", "Internal free: %lu KB  min-ever: %lu KB  largest block: %lu KB",
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
    esp_get_minimum_free_heap_size() / 1024,
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
```

**Fragmentation alarm:** if `largest_free_block` drops below 8 KB while `free_size` is above 30 KB, the heap is fragmented and a `xTaskCreate` may fail even though total free looks healthy. This has happened before: the OTA update worker needs a 12 KB **contiguous** internal block, and a fragmented pool produced `Failed to start update task` while total free still read healthy.

`MemoryStatus.fragmentation_pct` (`1 - largest_block / free`) makes this a single
number; `mem_watch.py --fail-largest-under-kb` gates on it.

The existing heap log points are:
- `main.cpp` — once at boot
- `webserver.cpp` — in `/api/status` response
- `cli_menu.cpp` — in the TUI diagnostics panel

A fourth log point in `tasks.cpp` (after all tasks are created) was added in this session; it prints the post-creation heap headroom and each task's initial stack allocation.

---

## CI Memory Gate

`.github/workflows/esp32-firmware.yml` includes a memory check step that runs after the build artifact is produced:

```yaml
- name: Memory size check
  run: python Firmware/tools/check_memory.py
         Firmware/ESP32/.pio/build/esp32s3/firmware.elf
         Firmware/ESP32/partitions.csv
```

The step fails the workflow if static SRAM exceeds the threshold or the binary won't fit in the OTA partition, blocking the release.

---

## Overflow Prediction Checklist (before every release)

| Check | How | Pass criterion |
|-------|-----|----------------|
| Binary fits in OTA partition | `check_memory.py` | `< 90%` of 4 MB |
| Static DRAM headroom | `check_memory.py` `.data + .bss` | `< 160 KB` (baseline 84.8 KB) |
| Heap free under load | `mem_watch.py --stress` | `≥ 24 KB internal free` |
| Largest block under load | `mem_watch.py --stress` | `≥ 8 KB` (no fragmentation) |
| Stack HWM for each task | `stack_hwm` after exercising deep paths | All tasks `≥ 1024 bytes` free |
| Host-side regressions | `pytest tests/unit/test_memory_telemetry.py` | green |
| No stack canary trips | `sdkconfig CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` | No abort during bench soak |
| OTA + SPIFFS write under load | manual bench test | No PSRAM corruption, no watchdog |

---

## Common Failure Modes and Fixes

### `xTaskCreate` returns `pdFAIL` at boot

The kernel tried to allocate a TCB + stack from the internal heap and failed. Either:
- Static SRAM is exhausted (check `check_memory.py` output)
- Heap is fragmented (check `largest_free_block` log)

Fix: reduce an existing task's stack, move a **data buffer** (never a stack) to PSRAM with `EXT_RAM_BSS_ATTR` at file scope, or reduce `SPIRAM_MALLOC_RESERVE_INTERNAL` by 4–8 KB. Before any of that, run the `nm` dump above — the two traps in *Where internal DRAM actually goes* have each been worth several KB.

### Watchdog during OTA / NVS write

OTA disables D-cache for flash writes. PSRAM accesses with D-cache off stall indefinitely → Core 0 watchdog fires. This has two causes:
1. A task on Core 0 is blocked on a PSRAM read inside the OTA window — move it to Core 1 or use `MALLOC_CAP_INTERNAL` for its hot path.
2. A PSRAM-backed stack is being used during the flash window — move that task's stack to internal SRAM.

### Stack overflow on the command processor

`cmdProc` (3 072 B) drains the `g_cmdQueue` command queue. Its depth is set by
the deepest **queued** handler, not by the traffic you are sending — a producer
you did not think about is the usual cause. `tasks_apply_channel_function()`
(SPI stop/restart, `ADC_CONFIG` read-back, MUX routing, `clearAllAlerts()`)
costs 1 672 B and reaches this task from `BBP_CMD_DEVICE_RESET` and from the
DSP stream's rate change. The equivalent BBP `SET_CH_FUNC` handler contributes
nothing, because it runs synchronously on `bbpCli` instead.

To attribute a peak on a worker task, diff the high-water mark across a single
operation **inside one session**; a between-session reading folds in the
disconnect path as well.

### Heap minimum drops to < 20 KB under sustained WiFi + ADC streaming

WiFi TCP stacks and mbedTLS record buffers are allocated on-demand from internal heap (because they use `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`). Under sustained streaming + active HTTP connections the internal pool can fragment severely. Mitigations:
- Reduce `CONFIG_LWIP_TCP_MSS` from 1460 → 536 in `sdkconfig.defaults`
- Set `CONFIG_LWIP_TCP_WND_DEFAULT` to 2920 (2× MSS)
- Enforce a maximum of 2 simultaneous HTTP connections in `webserver.cpp`

---

## Further Reading

- `Firmware/ESP32/src/tasks.h` — `TASK_STACK_*` with the measured peak justifying each size
- `Firmware/ESP32/sdkconfig.defaults` — all memory-critical knobs with inline comments
- `Firmware/ESP32/partitions.csv` — partition layout with sizes
- `Firmware/tools/check_memory.py` — post-build size gate source
- `tests/tools/mem_watch.py` — live dashboard and CI threshold gate
- `python/bugbuster/memory.py` — `MemoryStatus`, derived metrics, warnings
- `tests/unit/test_memory_telemetry.py` — host-side regression guards
- ESP-IDF docs: *Heap Memory Allocation*, *FreeRTOS Stack Overflow Detection*
