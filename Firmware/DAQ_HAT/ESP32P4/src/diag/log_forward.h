#pragma once

// log_forward.h - routes P4 ERROR logs and LOG_IMPORTANT events into the ring the S3 pulls.
#include <stddef.h>
#include <stdint.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install the esp_log vprintf hook (keeps the previous sink). Call first thing in app_main. */
void   log_forward_init(void);
/** Used by the S3 link: encode records with seq > after_seq. 0 = nothing / error. */
size_t log_forward_pull(uint32_t after_seq, uint8_t *out, size_t cap);
void   log_forward_important(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

/** Normal ESP_LOGI plus a copy in the ring the S3 ships to the hub: run start/stop, faults, calibration. */
#define LOG_IMPORTANT(tag, fmt, ...) \
    do { ESP_LOGI(tag, fmt, ##__VA_ARGS__); log_forward_important(tag, fmt, ##__VA_ARGS__); } while (0)
