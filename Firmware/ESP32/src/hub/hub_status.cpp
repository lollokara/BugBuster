#include "hub_status.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static hub_status_t s_st;
static volatile bool s_resync;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void hub_status_get(hub_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

void hub_status_set_conn(const char *url, hub_urlsrc_t src)
{
    portENTER_CRITICAL(&s_mux);
    strncpy(s_st.url, url ? url : "", sizeof(s_st.url) - 1);
    s_st.source = src;
    portEXIT_CRITICAL(&s_mux);
}

void hub_status_note_push(bool ok, uint32_t unix_s, const char *err)
{
    portENTER_CRITICAL(&s_mux);
    s_st.ok = ok;
    if (ok) {
        s_st.last_push = unix_s;
        s_st.last_error[0] = '\0';
    } else {
        strncpy(s_st.last_error, err ? err : "", sizeof(s_st.last_error) - 1);
    }
    portEXIT_CRITICAL(&s_mux);
}

void hub_status_set_backlog(uint32_t logs, uint32_t samples)
{
    portENTER_CRITICAL(&s_mux);
    s_st.backlog_logs = logs;
    s_st.backlog_samples = samples;
    portEXIT_CRITICAL(&s_mux);
}

void hub_status_set_runs(uint16_t known, uint16_t synced)
{
    portENTER_CRITICAL(&s_mux);
    s_st.runs_known = known;
    s_st.runs_synced = synced;
    portEXIT_CRITICAL(&s_mux);
}

void hub_resync_request(void) { s_resync = true; }

bool hub_resync_take(void)
{
    bool r = s_resync;
    s_resync = false;
    return r;
}
