#pragma once

// =============================================================================
// hub_config.h - NVS-backed hub streaming settings (spec 2026-10-03 section 6):
// hub_url (empty = discovery), hub_enabled (default on), log_level_s3 (default W),
// plus the last hub URL that registered, cached for the next boot.
// NVS namespace "hub": keys hub_url, hub_on, log_lvl, hub_cache.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_URL_MAX 96
#define HUB_DEFAULT_URL "http://192.168.3.87:8080"

typedef struct {
    char hub_url[HUB_URL_MAX];      /* "" = use discovery */
    bool hub_enabled;
    char log_level;                 /* 'E' 'W' 'I' 'D' */
} hub_config_t;

void     hub_config_load(hub_config_t *c);
/** "" clears the override. False if the url is not http://host[:port] or NVS failed. */
bool     hub_config_set_url(const char *url);
bool     hub_config_set_enabled(bool on);
bool     hub_config_set_level(char level);
bool     hub_config_cached_get(char *out, size_t cap);
void     hub_config_cached_set(const char *url);
/** Bumped by every successful set: the hub task reloads and re-resolves when it changes. */
uint32_t hub_config_generation(void);

#ifdef __cplusplus
}
#endif
