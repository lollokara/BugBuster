# BugBuster ESP32 Memory Safety Guide

> How to detect SRAM exhaustion, stack overflows, and heap fragmentation before shipping firmware.

---

## Memory Layout (ESP32-S3 N16R8)

```
┌─ Internal SRAM (320 KB addressable, ~192 KB available to app) ─┐
│                                                                  │
│  FreeRTOS kernel + TCBs          ~5–8 KB                       │
│  WiFi/LWIP state (Core 0)        ~30 KB                        │
│  BBP static buffers              ~5.8 KB                       │
│  Waveform LUT                    ~1 KB                         │
│  Scope ring buffer               ~18 KB                        │
│  Task stacks (Core 1):                                          │
│    adcPoll    4 096 B                                           │
│    faultMon   4 096 B                                           │
│    cmdProc    8 192 B                                           │
│    wavegen    4 096 B                                           │
│  SPIRAM_MALLOC_RESERVE (floor)    49 KB                         │
│  ──────────────────────────────────────                         │
│  Free at clean boot              ~78 KB                         │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
         ↓ malloc ≥ 1 KB goes here ↓
┌─ PSRAM (8 MB octal) ────────────────────────────────────────────┐
│  MicroPython heap                1 MB (allocated once at boot)  │
│  HTTP/TLS buffers                dynamic                        │
│  PSRAM task stacks: wifi_rc, cli_update, repl_tx               │
│  Free typically                  1–7 MB                         │
└──────────────────────────────────────────────────────────────────┘

Flash: 16 MB — partitions.csv layout:
  app0 / app1 (OTA A/B)   4 MB each   (firmware is ~1.8 MB → 2.2 MB headroom)
  spiffs (web UI)          4 MB
  scripts (MicroPython)    3 MB
  nvs + otadata + coredump remainder
```

**Critical rule — PSRAM stacks are unsafe during OTA/NVS/SPIFFS writes.**  
D-cache is disabled for those operations, which silently corrupts PSRAM-backed stacks on both cores regardless of whether the task touches flash itself. Tasks that run during OTA (`wifi_rc`, `cli_update`, `repl_tx`) accept this risk; measurement tasks (`adcPoll`, `faultMon`, `wavegen`, `cmdProc`) must use internal SRAM stacks.

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
| Static DRAM (`.data` + `.bss`) | 160 KB | Current baseline is ~144 KB; gate catches +16 KB regressions |
| Firmware binary vs OTA partition | 90% of partition | 400 KB headroom at 4 MB partition |

### 2. Stack High-Water Mark Reporting (runtime)

`tasks_log_stack_hwm()` in `tasks.cpp` calls `uxTaskGetStackHighWaterMark()` on every task handle and logs the result with `ESP_LOGI`. Call it:

- From the serial CLI (`stack_hwm` command if wired) 
- From the on-device web diagnostics page
- Automatically at startup after 60 s (enough time for all tasks to reach steady state)

Reading the output:

```
I (60000) tasks: Stack HWM (words free): adcPoll=312 faultMon=401 cmdProc=1024 wavegen=288
```

**Red flags:**
- Any task below **128 words** (512 bytes) free → increase its stack by 2 KB
- Any task below **64 words** (256 bytes) → immediate overflow risk, ship-blocker

### 3. Stack Overflow Hook (compile-time canary)

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

### 4. Heap Fragmentation Monitoring

`esp_get_minimum_free_heap_size()` returns the all-time low-water mark of internal free heap since boot. Log it regularly:

```c
ESP_LOGI("heap", "Internal free: %lu KB  min-ever: %lu KB  largest block: %lu KB",
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
    esp_get_minimum_free_heap_size() / 1024,
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
```

**Fragmentation alarm:** if `largest_free_block` drops below 8 KB while `free_size` is above 30 KB, the heap is fragmented and a `xTaskCreate` may fail even though total free looks healthy. This has happened before (see `sdkconfig.defaults` `SPIRAM_MALLOC_ALWAYSINTERNAL=0` history).

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
| Static DRAM headroom | `check_memory.py` `.data + .bss` | `< 120 KB` |
| Heap free after tasks start | boot log `main.cpp` | `≥ 40 KB internal free` |
| Heap largest block after tasks | boot log | `≥ 8 KB` (no fragmentation) |
| Stack HWM for each task | `tasks_log_stack_hwm()` after 60 s | All tasks `≥ 128 words` free |
| No stack canary trips | `sdkconfig CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` | No abort during bench soak |
| OTA + SPIFFS write under load | manual bench test | No PSRAM corruption, no watchdog |

---

## Common Failure Modes and Fixes

### `xTaskCreate` returns `pdFAIL` at boot

The kernel tried to allocate a TCB + stack from the internal heap and failed. Either:
- Static SRAM is exhausted (check `check_memory.py` output)
- Heap is fragmented (check `largest_free_block` log)

Fix: reduce an existing task's stack, move a non-critical task to PSRAM (`xTaskCreateWithCaps(... MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT ...)`), or reduce `SPIRAM_MALLOC_RESERVE_INTERNAL` by 4–8 KB.

### Watchdog during OTA / NVS write

OTA disables D-cache for flash writes. PSRAM accesses with D-cache off stall indefinitely → Core 0 watchdog fires. This has two causes:
1. A task on Core 0 is blocked on a PSRAM read inside the OTA window — move it to Core 1 or use `MALLOC_CAP_INTERNAL` for its hot path.
2. A PSRAM-backed stack is being used during the flash window — move that task's stack to internal SRAM.

### Stack overflow during deep BBP command chain

`cmdProc` (8 KB stack) handles all BBP commands. Some commands (`SELFTEST_MEASURE_SUPPLY`, `IDAC_CALIBRATE`, large `SCRIPT_EVAL`) trigger deep call chains. If `uxTaskGetStackHighWaterMark(cmdProc)` is below 256 words after running those commands, increase its stack to 12 KB.

### Heap minimum drops to < 20 KB under sustained WiFi + ADC streaming

WiFi TCP stacks and mbedTLS record buffers are allocated on-demand from internal heap (because they use `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`). Under sustained streaming + active HTTP connections the internal pool can fragment severely. Mitigations:
- Reduce `CONFIG_LWIP_TCP_MSS` from 1460 → 536 in `sdkconfig.defaults`
- Set `CONFIG_LWIP_TCP_WND_DEFAULT` to 2920 (2× MSS)
- Enforce a maximum of 2 simultaneous HTTP connections in `webserver.cpp`

---

## Further Reading

- `Firmware/ESP32/sdkconfig.defaults` — all memory-critical knobs with inline comments
- `Firmware/ESP32/partitions.csv` — partition layout with sizes
- `Firmware/tools/check_memory.py` — post-build size gate source
- ESP-IDF docs: *Heap Memory Allocation*, *FreeRTOS Stack Overflow Detection*
