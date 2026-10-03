#include "hub_config.h"

#include <string.h>
#include "nvs.h"
#include "hub_policy.h"
#include "hub_log.h"

static const char *NS = "hub";
static volatile uint32_t s_generation;

void hub_config_load(hub_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->hub_enabled = true;
    c->log_level = 'W';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;       // namespace not created yet: defaults
    size_t n = sizeof(c->hub_url);
    if (nvs_get_str(h, "hub_url", c->hub_url, &n) != ESP_OK) c->hub_url[0] = '\0';
    uint8_t v = 0;
    if (nvs_get_u8(h, "hub_on", &v) == ESP_OK) c->hub_enabled = v != 0;
    if (nvs_get_u8(h, "log_lvl", &v) == ESP_OK && hub_level_enabled((char)v, 'V')) c->log_level = (char)v;
    nvs_close(h);
}

static bool commit_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = (val && val[0]) ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;             // clearing an unset key is a success
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool commit_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool hub_config_set_url(const char *url)
{
    char norm[HUB_URL_MAX] = "";
    if (url && url[0] && !hub_url_normalize(url, norm, sizeof norm)) return false;
    if (!commit_str("hub_url", norm)) return false;
    s_generation++;
    return true;
}

bool hub_config_set_enabled(bool on)
{
    if (!commit_u8("hub_on", on ? 1 : 0)) return false;
    s_generation++;
    return true;
}

bool hub_config_set_level(char level)
{
    if (!hub_level_enabled(level, 'D')) return false;           // E W I D only
    if (!commit_u8("log_lvl", (uint8_t)level)) return false;
    s_generation++;
    return true;
}

bool hub_config_cached_get(char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) { out[0] = '\0'; return false; }
    size_t n = cap;
    bool ok = nvs_get_str(h, "hub_cache", out, &n) == ESP_OK;
    if (!ok) out[0] = '\0';
    nvs_close(h);
    return ok;
}

void hub_config_cached_set(const char *url)
{
    char have[HUB_URL_MAX];
    if (hub_config_cached_get(have, sizeof have) && strcmp(have, url) == 0) return;   // spare the flash
    commit_str("hub_cache", url);
}

uint32_t hub_config_generation(void) { return s_generation; }
