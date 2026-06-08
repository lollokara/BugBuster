#include "bb_fw_update.h"

#include <string.h>

#include "bb_config.h"
#include "bb_la.h"
#include "bb_la_usb.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/error.h"
#include "pico/flash.h"
#include "pico/platform.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/uart.h"
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"

#define RP2040_FLASH_SIZE_BYTES   (2u * 1024u * 1024u)
#define FW_STAGE_OFFSET           0x180000u
#define FW_STAGE_SIZE             0x70000u
#define FW_CAL_RESERVED_OFFSET    (RP2040_FLASH_SIZE_BYTES - 8192u)
#define FW_PAGE_SIZE              FLASH_PAGE_SIZE

_Static_assert(FW_STAGE_OFFSET + FW_STAGE_SIZE <= FW_CAL_RESERVED_OFFSET,
               "RP2040 firmware staging overlaps calibration flash journal");

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    uint32_t len;
} FlashOp;

static BbFwUpdateState s_state = BB_FW_UPDATE_IDLE;
static uint32_t s_image_size = 0;
static uint32_t s_expected_crc32 = 0;
static uint32_t s_actual_crc32 = 0;
static uint32_t s_bytes_written = 0;
static uint8_t s_last_error = 0;
static uint8_t s_page[FW_PAGE_SIZE];
static uint32_t s_page_base = 0;
static uint16_t s_page_fill = 0;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static void __not_in_flash_func(flash_erase_cb)(void *param)
{
    FlashOp *op = (FlashOp *)param;
    flash_range_erase(op->offset, op->len);
}

static void __not_in_flash_func(flash_program_cb)(void *param)
{
    FlashOp *op = (FlashOp *)param;
    flash_range_program(op->offset, op->data, op->len);
}

static bool __not_in_flash_func(flash_erase_range_safe)(uint32_t offset, uint32_t len)
{
    FlashOp op = { .offset = offset, .data = NULL, .len = len };
    return flash_safe_execute(flash_erase_cb, &op, 10000) == PICO_OK;
}

static bool __not_in_flash_func(flash_program_safe)(uint32_t offset, const uint8_t *data, uint32_t len)
{
    FlashOp op = { .offset = offset, .data = data, .len = len };
    return flash_safe_execute(flash_program_cb, &op, 1000) == PICO_OK;
}

static bool flush_page(void)
{
    if (s_page_fill == 0) return true;
    memset(s_page + s_page_fill, 0xFF, FW_PAGE_SIZE - s_page_fill);
    bool ok = flash_program_safe(FW_STAGE_OFFSET + s_page_base, s_page, FW_PAGE_SIZE);
    s_page_fill = 0;
    s_page_base += FW_PAGE_SIZE;
    if (!ok) {
        s_state = BB_FW_UPDATE_FAILED;
        s_last_error = HAT_ERR_BUSY;
    }
    return ok;
}

void bb_fw_update_init(void)
{
    s_state = BB_FW_UPDATE_IDLE;
    s_image_size = 0;
    s_expected_crc32 = 0;
    s_actual_crc32 = 0;
    s_bytes_written = 0;
    s_last_error = 0;
    s_page_base = 0;
    s_page_fill = 0;
    memset(s_page, 0xFF, sizeof(s_page));
}

bool bb_fw_update_begin(uint32_t image_size, uint32_t expected_crc32)
{
    if (bb_la_usb_is_streaming() || bb_la_usb_has_pending_data()) {
        s_last_error = HAT_ERR_BUSY;
        return false;
    }
    LaStatus la_st;
    bb_la_get_status(&la_st);
    if (la_st.state == LA_STATE_ARMED || la_st.state == LA_STATE_CAPTURING ||
        la_st.state == LA_STATE_STREAMING) {
        s_last_error = HAT_ERR_BUSY;
        return false;
    }
    if (image_size == 0 || image_size > FW_STAGE_SIZE) {
        s_last_error = HAT_ERR_INVALID_FUNC;
        return false;
    }

    uint32_t erase_len = (image_size + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
    if (!flash_erase_range_safe(FW_STAGE_OFFSET, erase_len)) {
        s_state = BB_FW_UPDATE_FAILED;
        s_last_error = HAT_ERR_BUSY;
        return false;
    }

    s_state = BB_FW_UPDATE_RECEIVING;
    s_image_size = image_size;
    s_expected_crc32 = expected_crc32;
    s_actual_crc32 = 0;
    s_bytes_written = 0;
    s_last_error = 0;
    s_page_base = 0;
    s_page_fill = 0;
    memset(s_page, 0xFF, sizeof(s_page));
    return true;
}

bool bb_fw_update_chunk(uint32_t offset, const uint8_t *data, uint8_t len)
{
    if (s_state != BB_FW_UPDATE_RECEIVING || !data || len == 0) {
        s_last_error = HAT_ERR_FRAME;
        return false;
    }
    if (offset != s_bytes_written || offset + len > s_image_size) {
        s_last_error = HAT_ERR_FRAME;
        return false;
    }

    uint8_t remaining = len;
    while (remaining > 0) {
        uint16_t space = FW_PAGE_SIZE - s_page_fill;
        uint8_t take = (remaining < space) ? remaining : (uint8_t)space;
        memcpy(s_page + s_page_fill, data, take);
        s_page_fill += take;
        data += take;
        remaining -= take;
        if (s_page_fill == FW_PAGE_SIZE && !flush_page()) {
            return false;
        }
    }

    s_actual_crc32 = crc32_update(s_actual_crc32, data - len, len);
    s_bytes_written += len;
    if (s_bytes_written == s_image_size) {
        if (!flush_page()) return false;
        s_state = (s_actual_crc32 == s_expected_crc32) ? BB_FW_UPDATE_READY : BB_FW_UPDATE_FAILED;
        if (s_state == BB_FW_UPDATE_FAILED) {
            s_last_error = HAT_ERR_CRC;
            return false;
        }
    }
    return true;
}

static bool verify_staged_crc(void)
{
    const uint8_t *p = (const uint8_t *)(XIP_BASE + FW_STAGE_OFFSET);
    uint32_t crc = 0;
    uint32_t remaining = s_image_size;
    while (remaining > 0) {
        uint32_t n = remaining > 1024u ? 1024u : remaining;
        crc = crc32_update(crc, p, n);
        p += n;
        remaining -= n;
    }
    s_actual_crc32 = crc;
    return crc == s_expected_crc32;
}

static uint8_t s_commit_sector[FLASH_SECTOR_SIZE];

static void __not_in_flash_func(raw_uart_putc)(char c)
{
    while (uart_get_hw(BB_UART)->fr & UART_UARTFR_TXFF_BITS) {
        __asm("nop");
    }
    uart_get_hw(BB_UART)->dr = c;
}

static void __not_in_flash_func(raw_uart_log)(const char *msg)
{
    uint8_t len = 0;
    while (msg[len] && len < 240) {
        len++;
    }

    // Send SYNC (0xAA)
    raw_uart_putc(0xAA);
    // Send LEN
    raw_uart_putc(len);
    // Send CMD (HAT_RSP_LA_LOG = 0x89)
    raw_uart_putc(HAT_RSP_LA_LOG);

    // Send Payload
    for (uint8_t i = 0; i < len; i++) {
        raw_uart_putc(msg[i]);
    }

    // Compute CRC over CMD (HAT_RSP_LA_LOG) + payload
    uint8_t crc_val = 0;
    crc_val ^= HAT_RSP_LA_LOG;
    for (int j = 0; j < 8; j++) {
        crc_val = (crc_val & 0x80) ? (uint8_t)((crc_val << 1) ^ 0x07) : (uint8_t)(crc_val << 1);
    }
    for (uint8_t i = 0; i < len; i++) {
        crc_val ^= (uint8_t)msg[i];
        for (int j = 0; j < 8; j++) {
            crc_val = (crc_val & 0x80) ? (uint8_t)((crc_val << 1) ^ 0x07) : (uint8_t)(crc_val << 1);
        }
    }
    raw_uart_putc(crc_val);
}

static void __not_in_flash_func(raw_uint32_to_hex)(uint32_t val, char *buf)
{
    buf[0] = '0';
    buf[1] = 'x';
    int pos = 2;
    bool leading_zero = true;
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (val >> shift) & 0xF;
        if (nibble != 0 || !leading_zero || shift == 0) {
            if (nibble < 10) {
                buf[pos++] = (char)('0' + nibble);
            } else {
                buf[pos++] = (char)('A' + (nibble - 10));
            }
            leading_zero = false;
        }
    }
    buf[pos] = '\0';
}

static void __not_in_flash_func(watchdog_trigger_reboot_direct)(void)
{
    // Clear enable and set scratch register to 0 (normal boot)
    watchdog_hw->ctrl = 0;
    watchdog_hw->scratch[4] = 0;

    // Reset everything apart from ROSC and XOSC
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);

    // Trigger reboot immediately (delay = 0) by setting the trigger bit
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;

    // Spin until reboot
    for (;;) {
        __asm("nop");
    }
}

static void __not_in_flash_func(commit_task_func)(void *param)
{
    (void)param;

    // Initialize stack arrays with strings before disabling flash/cache/interrupts
    char msg_spawn[] = "[COMMIT] Start Core 0";
    char msg_halt[] = "[COMMIT] Core 1 halted";
    char msg_prefix[] = "[COMMIT] Copy ";
    char msg_done[] = "[COMMIT] Done, rebooting";

    // 1. Halt Core 1 immediately (keeps it in a safe bootrom handshake loop)
    raw_uart_log(msg_spawn);
    multicore_reset_core1();

    // 2. Disable interrupts globally on Core 0
    raw_uart_log(msg_halt);
    (void)save_and_disable_interrupts();

    // 3. Copy staged image sector by sector
    uint32_t committed = 0;
    while (committed < s_image_size) {
        uint32_t n = s_image_size - committed;
        if (n > FLASH_SECTOR_SIZE) n = FLASH_SECTOR_SIZE;

        // Log progress dynamically without flash reads or division
        char msg[64];
        char num_buf[16];
        char *p = msg;
        const char *prefix_ptr = msg_prefix;
        while (*prefix_ptr) {
            *p++ = *prefix_ptr++;
        }
        raw_uint32_to_hex(committed, num_buf);
        char *nb = num_buf;
        while (*nb) {
            *p++ = *nb++;
        }
        *p++ = '/';
        raw_uint32_to_hex(s_image_size, num_buf);
        nb = num_buf;
        while (*nb) {
            *p++ = *nb++;
        }
        *p = '\0';
        raw_uart_log(msg);

        // Volatile byte-by-byte initialization to prevent compiler optimizing to flash-resident memset
        volatile uint8_t *dest = s_commit_sector;
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
            dest[i] = 0xFF;
        }

        const uint8_t *src = (const uint8_t *)(XIP_BASE + FW_STAGE_OFFSET + committed);
        for (uint32_t i = 0; i < n; i++) {
            dest[i] = src[i];
        }

        // Call raw flash functions (which are in RAM) directly rather than
        // the _safe wrappers since the scheduler is disabled and the other
        // core is halted.
        flash_range_erase(committed, FLASH_SECTOR_SIZE);
        flash_range_program(committed, (const uint8_t *)s_commit_sector, FLASH_SECTOR_SIZE);

        committed += FLASH_SECTOR_SIZE;
    }

    raw_uart_log(msg_done);

    // 4. Trigger watchdog reboot directly from RAM
    watchdog_trigger_reboot_direct();
}

bool __not_in_flash_func(bb_fw_update_commit_verified)(void)
{
    if (s_state != BB_FW_UPDATE_READY || s_bytes_written != s_image_size) {
        s_last_error = HAT_ERR_FRAME;
        return false;
    }
    if (!verify_staged_crc()) {
        s_state = BB_FW_UPDATE_FAILED;
        s_last_error = HAT_ERR_CRC;
        return false;
    }

    s_state = BB_FW_UPDATE_COMMITTING;

    // Create a high-priority task pinned to Core 0 to perform the commit.
    // Core 1 (this task) will be reset by Core 0 shortly after creation.
    TaskHandle_t task_handle;
    BaseType_t ok = xTaskCreateAffinitySet(
        commit_task_func,
        "commit",
        1024,
        NULL,
        configMAX_PRIORITIES - 1,
        (1 << 0),
        &task_handle
    );

    if (ok != pdPASS) {
        s_state = BB_FW_UPDATE_FAILED;
        s_last_error = HAT_ERR_BUSY;
        return false;
    }

    // Spin Core 1 indefinitely waiting for Core 0 to reset us.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void bb_fw_update_get_status(uint8_t *state, uint32_t *bytes_written,
                             uint32_t *image_size, uint32_t *expected_crc32,
                             uint32_t *actual_crc32, uint8_t *last_error)
{
    if (state) *state = (uint8_t)s_state;
    if (bytes_written) *bytes_written = s_bytes_written;
    if (image_size) *image_size = s_image_size;
    if (expected_crc32) *expected_crc32 = s_expected_crc32;
    if (actual_crc32) *actual_crc32 = s_actual_crc32;
    if (last_error) *last_error = s_last_error;
}
