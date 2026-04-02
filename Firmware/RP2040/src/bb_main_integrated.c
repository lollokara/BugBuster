// =============================================================================
// bb_main_integrated.c — Combined entry point: debugprobe + BugBuster
//
// This replaces debugprobe's main.c to add our bb_cmd_task alongside the
// standard debugprobe FreeRTOS tasks (USB, DAP, UART bridge, autobaud).
// All debugprobe functionality is preserved; we just add one more task.
// =============================================================================

#include "FreeRTOS.h"
#include "task.h"

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#if PICO_SDK_VERSION_MAJOR >= 2
#include "bsp/board_api.h"
#else
#include "bsp/board.h"
#endif
#include "tusb.h"

#include "probe_config.h"
#include "probe.h"
#include "cdc_uart.h"
#include "autobaud.h"
#include "get_serial.h"
#include "tusb_edpt_handler.h"
#include "DAP.h"
#include "hardware/structs/usb.h"

// BugBuster command task entry point (defined in bb_main.c)
extern void bb_cmd_task(void *params);

// Task priorities
#define UART_TASK_PRIO     (tskIDLE_PRIORITY + 3)
#define TUD_TASK_PRIO      (tskIDLE_PRIORITY + 2)
#define DAP_TASK_PRIO      (tskIDLE_PRIORITY + 1)
#define AUTOBAUD_TASK_PRIO (tskIDLE_PRIORITY + 1)
#define BB_CMD_TASK_PRIO   (tskIDLE_PRIORITY + 1)  // Same as DAP — not time-critical

static uint8_t TxDataBuffer[CFG_TUD_HID_EP_BUFSIZE];
static uint8_t RxDataBuffer[CFG_TUD_HID_EP_BUFSIZE];

TaskHandle_t dap_taskhandle, tud_taskhandle, mon_taskhandle;
static TaskHandle_t bb_taskhandle;
static int was_configured;

// =============================================================================
// debugprobe tasks (copied from debugprobe main.c — unchanged)
// =============================================================================

void dev_mon(void *ptr)
{
    uint32_t sof[3];
    int i = 0;
    TickType_t wake;
    wake = xTaskGetTickCount();
    do {
        xTaskDelayUntil(&wake, 100);
        if (tud_connected() && !tud_suspended()) {
            sof[i++] = usb_hw->sof_rd & USB_SOF_RD_BITS;
            i = i % 3;
        } else {
            for (i = 0; i < 3; i++)
                sof[i] = 0;
        }
        if ((sof[0] | sof[1] | sof[2]) != 0) {
            if ((sof[0] == sof[1]) && (sof[1] == sof[2])) {
                probe_info("Watchdog timeout! Resetting USBD\n");
                tud_deinit(0);
                xTaskDelayUntil(&wake, 1);
                tud_init(0);
            }
        }
    } while (1);
}

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void) rhport;
    (void) eventid;
    BaseType_t blah;
    if (in_isr) {
        xTaskNotifyFromISR(tud_taskhandle, 0, 0, &blah);
    } else {
        xTaskNotify(tud_taskhandle, 0, 0);
    }
}

void usb_thread(void *ptr)
{
    uint32_t cmd;
#ifdef PROBE_USB_CONNECTED_LED
    gpio_init(PROBE_USB_CONNECTED_LED);
    gpio_set_dir(PROBE_USB_CONNECTED_LED, GPIO_OUT);
#endif
    TickType_t wake;
    wake = xTaskGetTickCount();
    do {
        tud_task();
#ifdef PROBE_USB_CONNECTED_LED
        if (!gpio_get(PROBE_USB_CONNECTED_LED) && tud_ready())
            gpio_put(PROBE_USB_CONNECTED_LED, 1);
        else
            gpio_put(PROBE_USB_CONNECTED_LED, 0);
#endif
        if (tud_suspended() || !tud_connected())
            xTaskDelayUntil(&wake, 20);
        else if (!tud_task_event_ready())
            xTaskNotifyWait(0, 0xFFFFFFFFu, &cmd, 1);
    } while (1);
}

// =============================================================================
// Main — debugprobe init + BugBuster command task
// =============================================================================

int main(void)
{
    bi_decl_config();
    board_init();
    usb_serial_init();
    cdc_uart_init();
    tusb_init();
    // stdio_uart_init() disabled — UART0 reserved for BugBuster command bus
    // Debug printf goes to USB CDC or nowhere
    DAP_Setup();

    probe_info("Welcome to BugBuster HAT (debugprobe + extensions)!\n");

    // Standard debugprobe tasks
    xTaskCreate(usb_thread, "TUD", configMINIMAL_STACK_SIZE, NULL, TUD_TASK_PRIO, &tud_taskhandle);
#if PICO_RP2040
    xTaskCreate(dev_mon, "WDOG", configMINIMAL_STACK_SIZE, NULL, TUD_TASK_PRIO, &mon_taskhandle);
#endif

    // BugBuster UART command handler task — runs independently of USB/DAP
    xTaskCreate(bb_cmd_task, "BBCMD", 2048, NULL, BB_CMD_TASK_PRIO, &bb_taskhandle);

    vTaskStartScheduler();

    return 0;  // Never reached
}

// =============================================================================
// USB callbacks — same as debugprobe
// =============================================================================

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void) itf; (void) report_id; (void) report_type; (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* RxDataBuffer, uint16_t bufsize)
{
    uint32_t response_size = TU_MIN(CFG_TUD_HID_EP_BUFSIZE, bufsize);
    (void) itf; (void) report_id; (void) report_type;
    DAP_ProcessCommand(RxDataBuffer, TxDataBuffer);
    tud_hid_report(0, TxDataBuffer, response_size);
}

#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
extern uint8_t const desc_ms_os_20[];
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    switch (request->bmRequestType_bit.type) {
        case TUSB_REQ_TYPE_VENDOR:
            switch (request->bRequest) {
                case 1:
                    if (request->wIndex == 7) {
                        uint16_t total_len;
                        memcpy(&total_len, desc_ms_os_20+8, 2);
                        return tud_control_xfer(rhport, request, (void*) desc_ms_os_20, total_len);
                    } else {
                        return false;
                    }
                default: break;
            }
            break;
        default: break;
    }
    return false;
}
#endif

void tud_suspend_cb(bool remote_wakeup_en)
{
    probe_info("Suspended\n");
    if (was_configured) {
        vTaskSuspend(uart_taskhandle);
        vTaskSuspend(dap_taskhandle);
        if (autobaud_running) autobaud_wait_stop();
        vTaskSuspend(autobaud_taskhandle);
    }
}

void tud_resume_cb(void)
{
    probe_info("Resumed\n");
    if (was_configured) {
        vTaskResume(uart_taskhandle);
        vTaskResume(dap_taskhandle);
        vTaskResume(autobaud_taskhandle);
    }
}

void tud_unmount_cb(void)
{
    probe_info("Disconnected\n");
    vTaskSuspend(uart_taskhandle);
    vTaskSuspend(dap_taskhandle);
    vTaskDelete(uart_taskhandle);
    vTaskDelete(dap_taskhandle);
    if (autobaud_running) autobaud_wait_stop();
    vTaskSuspend(autobaud_taskhandle);
    vTaskDelete(autobaud_taskhandle);
    was_configured = 0;
}

void tud_mount_cb(void)
{
    probe_info("Connected, Configured\n");
    if (!was_configured) {
        xTaskCreate(cdc_thread, "UART", configMINIMAL_STACK_SIZE, NULL, UART_TASK_PRIO, &uart_taskhandle);
        xTaskCreate(dap_thread, "DAP", configMINIMAL_STACK_SIZE, NULL, DAP_TASK_PRIO, &dap_taskhandle);
        xTaskCreate(autobaud_thread, "ABR", configMINIMAL_STACK_SIZE, NULL, AUTOBAUD_TASK_PRIO, &autobaud_taskhandle);
#if(configNUMBER_OF_CORES > 1)
        vTaskCoreAffinitySet(autobaud_taskhandle, (1 << 1));
        vTaskCoreAffinitySet(dap_taskhandle, (1 << 1));
        vTaskCoreAffinitySet(uart_taskhandle, (1 << 0));
#endif
        was_configured = 1;
    }
}

void vApplicationTickHook(void) {}

void vApplicationStackOverflowHook(TaskHandle_t Task, char *pcTaskName)
{
    panic("stack overflow (not the helpful kind) for %s\n", *pcTaskName);
}

void vApplicationMallocFailedHook(void)
{
    panic("Malloc Failed\n");
}
