// MicroPython port configuration for BugBuster ESP32-S3
// Phase 0: link-only, nothing executed.

#ifndef MICROPY_INCLUDED_BUGBUSTER_MPCONFIGPORT_H
#define MICROPY_INCLUDED_BUGBUSTER_MPCONFIGPORT_H

#include <stdint.h>
#include <alloca.h>
// Guard IDF headers behind NO_QSTR so makeqstrdefs.py pp can preprocess
// this file without needing the full IDF SDK include paths.
#ifndef NO_QSTR
#include "esp_system.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#endif

// ── Object representation & NLR ───────────────────────────────────────────────
#define MICROPY_OBJ_REPR                    (MICROPY_OBJ_REPR_A)
// Xtensa LX7 requires setjmp-based NLR (no native asm NLR for LX7)
#define MICROPY_NLR_SETJMP                  (1)

// ── ROM config level ──────────────────────────────────────────────────────────
#ifndef MICROPY_CONFIG_ROM_LEVEL
#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#endif

// ── Memory ────────────────────────────────────────────────────────────────────
#define MICROPY_ALLOC_PATH_MAX              (128)
#ifndef MICROPY_GC_INITIAL_HEAP_SIZE
#define MICROPY_GC_INITIAL_HEAP_SIZE        (64 * 1024)
#endif
#define MICROPY_GC_SPLIT_HEAP               (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO          (1)

// ── Emitters — disable all native emitters; bytecode only ─────────────────────
#define MICROPY_EMIT_XTENSAWIN              (0)
#define MICROPY_EMIT_XTENSA                 (0)
#define MICROPY_PERSISTENT_CODE_LOAD        (1)

// ── Optimisations ─────────────────────────────────────────────────────────────
#ifndef MICROPY_OPT_COMPUTED_GOTO
#define MICROPY_OPT_COMPUTED_GOTO           (1)
#endif

// ── ROM text compression — disable (v1.24.1 mkrules.cmake doesn't generate it)
#define MICROPY_ROM_TEXT_COMPRESSION        (0)

// ── Python internals ──────────────────────────────────────────────────────────
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_STACK_CHECK                 (1)
#define MICROPY_STACK_CHECK_MARGIN          (1024)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_WARNINGS                    (1)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_STREAMS_POSIX_API           (1)
#define MICROPY_USE_INTERNAL_ERRNO          (0)
#define MICROPY_USE_INTERNAL_PRINTF         (0)
#define MICROPY_SCHEDULER_DEPTH             (8)
// VFS deliberately DISABLED in Phase 6a.
// Initial attempt enabled MICROPY_VFS=1 + MICROPY_VFS_POSIX=1 to expose import
// + open() against /spiffs/, but that pulls in extmod/vfs_fat.c (FAT VFS) and
// extmod/vfs_posix.c which require <poll.h> and FFCONF_H — neither available
// under our ESP-IDF newlib/no-fat config. Phase 6a only needs RUN-by-name,
// which is satisfied by script_storage.cpp + scripting_run_file (file is read
// by C fopen + fed to the existing eval queue). Future phase can revisit MP
// VFS if user-script `import` from /spiffs/ becomes a requirement; that will
// need vfs_fat exclusion + a poll.h shim.
#define MICROPY_VFS                         (0)
#define MICROPY_READER_VFS                  (0)

// ── Threading — disabled in Phase 0 (no mpthreadport.h needed) ───────────────
#define MICROPY_PY_THREAD                   (0)

// ── Disable modules that require port-specific C source files ─────────────────
// These are enabled in the official ESP32 port but we don't include those files.
#define MICROPY_PY_MACHINE                  (0)
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_ESPNOW                   (0)
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_WEBSOCKET                (0)
#define MICROPY_PY_WEBREPL                  (0)
#define MICROPY_PY_ONEWIRE                  (0)
#define MICROPY_HW_ENABLE_SDCARD            (0)
#define MICROPY_HW_ENABLE_USBDEV            (0)

// ── builtins ──────────────────────────────────────────────────────────────────
#define MICROPY_PY_STR_BYTES_CMP_WARN       (1)
#define MICROPY_PY_ALL_INPLACE_SPECIAL_METHODS (1)
#define MICROPY_PY_IO_BUFFEREDWRITER        (1)
// Disable time functions that require port implementations (mp_time_localtime_get etc.)
// Enable in a later phase when time HAL is implemented.
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (0)
#define MICROPY_PY_TIME_TIME_TIME_NS        (0)

// ── fatfs ─────────────────────────────────────────────────────────────────────
#define MICROPY_FATFS_ENABLE_LFN            (1)
#define MICROPY_FATFS_RPATH                 (2)
#define MICROPY_FATFS_MAX_SS                (4096)
#define MICROPY_FATFS_LFN_CODE_PAGE         437

// ── Random seed ───────────────────────────────────────────────────────────────
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (esp_random())

// ── Port state ────────────────────────────────────────────────────────────────
// Map the port state struct to the VM state (no port-specific fields needed).
#define MP_STATE_PORT MP_STATE_VM

// ── Type aliases ──────────────────────────────────────────────────────────────
typedef int mp_int_t;
typedef unsigned int mp_uint_t;
typedef long mp_off_t;

// ── Task stack ────────────────────────────────────────────────────────────────
#ifndef MICROPY_TASK_STACK_SIZE
#define MICROPY_TASK_STACK_SIZE             (16 * 1024)
#endif

// ── MP_SSIZE_MAX — avoid pulling in SSIZE_MAX from limits.h ──────────────────
// mp_int_t is int (32-bit) on ESP32-S3, so MP_SSIZE_MAX = INT_MAX.
#define MP_SSIZE_MAX                        (0x7FFFFFFF)

// ── Board / platform identity ─────────────────────────────────────────────────
// Required by mpconfig.h for MICROPY_BANNER_MACHINE and sys.platform.
#define MICROPY_HW_BOARD_NAME               "BugBuster-ESP32S3"
#define MICROPY_HW_MCU_NAME                 "ESP32-S3"
#define MICROPY_PY_SYS_PLATFORM             "esp32"

// ── Cooperative stop: VM polls stop flag at every back-edge ──────────────────
// scripting_vm_hook() is defined in scripting.cpp (extern "C").
// These macros are expanded inside MicroPython's bytecode dispatch loop (.c files).
extern void scripting_vm_hook(void);
#define MICROPY_VM_HOOK_INIT    scripting_vm_hook();
#define MICROPY_VM_HOOK_POLL    scripting_vm_hook();
#define MICROPY_VM_HOOK_LOOP    scripting_vm_hook();
#define MICROPY_VM_HOOK_RETURN

#endif // MICROPY_INCLUDED_BUGBUSTER_MPCONFIGPORT_H
