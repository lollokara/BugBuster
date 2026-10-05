// ESP glue for boot_report.h: reads the reset reason, the previous boot's breadcrumb
// and the flash coredump, and pushes the records into the ring the S3 pulls.
#include "boot_report.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "log_forward.h"

static const char *TAG = "bootrpt";

static RTC_NOINIT_ATTR br_crumb_t s_crumb;
static portMUX_TYPE s_crumb_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t up_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void boot_report_action_begin(uint8_t act, uint16_t run)
{
    portENTER_CRITICAL(&s_crumb_mux);
    br_crumb_begin(&s_crumb, act, run, up_ms());
    portEXIT_CRITICAL(&s_crumb_mux);
}

void boot_report_action_end(void)
{
    portENTER_CRITICAL(&s_crumb_mux);
    br_crumb_end(&s_crumb, up_ms());
    portEXIT_CRITICAL(&s_crumb_mux);
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
static void report_coredump(void)
{
    esp_err_t chk = esp_core_dump_image_check();
    if (chk == ESP_ERR_NOT_FOUND) {
        log_forward_record('W', "p4crash", "no coredump partition on this P4 (wired flash needed)");
        return;
    }
    if (chk != ESP_OK) return;                       /* no dump from the last reset */

    static esp_core_dump_summary_t sum;              /* ~1.2 KB: .bss, not a task stack */
    if (esp_core_dump_get_summary(&sum) != ESP_OK) {
        log_forward_record('W', "p4crash", "coredump present but summary unreadable");
        esp_core_dump_image_erase();
        return;
    }
    char line[LOG_FWD_MSG_MAX + 1];
    br_fmt_crash(line, sizeof line, sum.exc_task, sum.exc_pc, sum.ex_info.ra);
    log_forward_record('W', "p4crash", "%s", line);
    br_fmt_fault(line, sizeof line, sum.ex_info.sp, sum.ex_info.mcause, sum.ex_info.mtval);
    log_forward_record('W', "p4crash", "%s", line);

    uint32_t cand[20];
    size_t n = br_scan_stack(sum.exc_bt_info.stackdump,
                             sum.exc_bt_info.dump_size < sizeof sum.exc_bt_info.stackdump
                                 ? sum.exc_bt_info.dump_size : sizeof sum.exc_bt_info.stackdump,
                             cand, 20);
    br_fmt_elf(line, sizeof line, (const char *)sum.app_elf_sha256, (uint32_t)n);
    log_forward_record('W', "p4crash", "%s", line);

    char why[96];
    if (esp_core_dump_get_panic_reason(why, sizeof why) == ESP_OK) {
        br_fmt_why(line, sizeof line, why);
        log_forward_record('W', "p4crash", "%s", line);
    }
    uint32_t idx = 0, ln = 0;
    while (br_fmt_bt(line, sizeof line, cand, (uint32_t)n, &idx, ln++)) {
        log_forward_record('W', "p4crash", "%s", line);
    }
    esp_core_dump_image_erase();                     /* report each crash once */
}
#else
static void report_coredump(void)
{
    log_forward_record('W', "p4crash", "coredump disabled in this build");
}
#endif

void boot_report_init(void)
{
    br_crumb_t prev;
    int had_prev = br_crumb_boot(&s_crumb, &prev);
    esp_reset_reason_t why = esp_reset_reason();
    char line[LOG_FWD_MSG_MAX + 1];

    br_fmt_reset(line, sizeof line, (int)why, s_crumb.boot);
    log_forward_record(br_reset_abnormal((int)why) ? 'W' : 'I', "p4rst", "%s", line);
    ESP_LOGW(TAG, "%s", line);
    if (had_prev && why != ESP_RST_POWERON) {
        br_fmt_crumb(line, sizeof line, &prev);
        log_forward_record('I', "p4rst", "%s", line);
    }
    if (br_reset_abnormal((int)why)) report_coredump();
}
