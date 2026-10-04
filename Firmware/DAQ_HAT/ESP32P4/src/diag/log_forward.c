#include "log_forward.h"
#include <stdarg.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_ring.h"

static log_ring_t s_ring;                 /* 64 slots x ~100 B, file scope: .bss, not a task stack */
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void push(char level, const char *tag, const char *msg)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) return;   /* never block a logging task */
    log_ring_push(&s_ring, now_ms(), level, tag, msg);
    xSemaphoreGive(s_lock);
}

static int hook(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
    char line[200];                                   /* on the logging task's stack: keep it small */
    vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    char level, tag[LOG_TAG_MAX + 1], msg[LOG_MSG_MAX + 1];
    if (log_ring_parse_line(line, &level, tag, sizeof tag, msg, sizeof msg) && level == 'E') {
        push('E', tag, msg);
    }
    return n;
}

void log_forward_init(void)
{
    if (s_lock) return;
    log_ring_init(&s_ring);
    s_lock = xSemaphoreCreateMutex();
    s_prev = esp_log_set_vprintf(hook);
}

void log_forward_important(const char *tag, const char *fmt, ...)
{
    char msg[LOG_MSG_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    push('I', tag, msg);
}

size_t log_forward_pull(uint32_t after_seq, uint8_t *out, size_t cap)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    size_t n = log_ring_pull(&s_ring, after_seq, now_ms(), out, cap);
    xSemaphoreGive(s_lock);
    return n;
}
