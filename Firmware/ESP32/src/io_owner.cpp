// =============================================================================
// io_owner.cpp - IO Ownership Table implementation
// =============================================================================

#include "io_owner.h"
#include "cmd_errors.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "mbedtls/md.h"

#include <string.h>

static const char *TAG = "io_owner";

static_assert(IO_OWNER_NUM_SLOTS == 16, "IO owner table must have exactly 16 slots");

// Cross-core spinlock
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// The table
static io_owner_slot_t s_slots[IO_OWNER_NUM_SLOTS];

// Rolling session counter (never 0 when active)
static uint8_t s_session_counter = 0;

// BBP caller context (set by bbp.cpp around dispatch)
static io_owner_t s_current_bbp_caller = { IO_OWNER_NONE, 0, 0 };

// Auto-claim lease duration for legacy v4 clients (ms)
#define IO_OWNER_AUTO_LEASE_MS  30000u

// Last rejection details for BBP event emission (FIX 8)
static uint8_t          s_last_rejected_slot      = 0;
static io_owner_kind_t  s_last_rejected_owner_kind = IO_OWNER_NONE;

void io_owner_init(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_slots, 0, sizeof(s_slots));
    s_session_counter = 0;
    s_current_bbp_caller = { IO_OWNER_NONE, 0, 0 };
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "IO ownership table initialised (%d slots)", IO_OWNER_NUM_SLOTS);
}

void io_owner_tick(uint64_t now_ms)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < IO_OWNER_NUM_SLOTS; i++) {
        io_owner_slot_t *sl = &s_slots[i];
        if (sl->kind == IO_OWNER_NONE) continue;
        if (sl->lease_until_ms == 0) continue;  // infinite
        if (now_ms >= sl->lease_until_ms) {
            ESP_LOGD(TAG, "Slot %d lease expired (kind=%d sid=%d)",
                     i, (int)sl->kind, (int)sl->session_id);
            memset(sl, 0, sizeof(*sl));
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

bool io_owner_acquire(uint8_t slot_idx, io_owner_kind_t kind,
                      uint8_t session_id, uint32_t token_fp32,
                      uint32_t lease_ms, uint64_t now_ms)
{
    if (slot_idx >= IO_OWNER_NUM_SLOTS) return false;

    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    io_owner_slot_t *sl = &s_slots[slot_idx];

    if (sl->kind == IO_OWNER_NONE) {
        // Free — take it
        sl->kind           = kind;
        sl->session_id     = session_id;
        sl->token_fp32     = token_fp32;
        sl->lease_until_ms = (lease_ms == 0) ? 0 : (now_ms + lease_ms);
        ok = true;
    } else if (sl->session_id == session_id && sl->kind == kind) {
        // Same owner refreshing / re-acquiring
        if (lease_ms != 0) {
            sl->lease_until_ms = now_ms + lease_ms;
        }
        ok = true;
    }
    // else: different owner — deny
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool io_owner_release(uint8_t slot_idx, uint8_t session_id)
{
    if (slot_idx >= IO_OWNER_NUM_SLOTS) return false;

    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    io_owner_slot_t *sl = &s_slots[slot_idx];
    if (sl->kind != IO_OWNER_NONE &&
        (session_id == 0 || sl->session_id == session_id)) {
        memset(sl, 0, sizeof(*sl));
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void io_owner_force_release(uint8_t slot_idx)
{
    if (slot_idx >= IO_OWNER_NUM_SLOTS) return;
    portENTER_CRITICAL(&s_mux);
    memset(&s_slots[slot_idx], 0, sizeof(s_slots[slot_idx]));
    portEXIT_CRITICAL(&s_mux);
}

uint8_t io_owner_release_session(io_owner_kind_t kind, uint8_t session_id)
{
    if (kind == IO_OWNER_NONE) return 0;
    uint8_t released = 0;
    portENTER_CRITICAL(&s_mux);
    for (uint8_t i = 0; i < IO_OWNER_NUM_SLOTS; i++) {
        if (s_slots[i].kind == kind && s_slots[i].session_id == session_id) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            released++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return released;
}

uint8_t io_owner_release_all_except(io_owner_kind_t preserved_kind)
{
    uint8_t released = 0;
    portENTER_CRITICAL(&s_mux);
    for (uint8_t i = 0; i < IO_OWNER_NUM_SLOTS; i++) {
        if (s_slots[i].kind != IO_OWNER_NONE &&
            s_slots[i].kind != preserved_kind &&
            s_slots[i].kind != IO_OWNER_INTERNAL) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            released++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return released;
}

bool io_owner_is_free(uint8_t slot_idx)
{
    if (slot_idx >= IO_OWNER_NUM_SLOTS) return false;
    portENTER_CRITICAL(&s_mux);
    bool free = (s_slots[slot_idx].kind == IO_OWNER_NONE);
    portEXIT_CRITICAL(&s_mux);
    return free;
}

io_owner_slot_t io_owner_get(uint8_t slot_idx)
{
    io_owner_slot_t out = {};
    if (slot_idx >= IO_OWNER_NUM_SLOTS) return out;
    portENTER_CRITICAL(&s_mux);
    out = s_slots[slot_idx];
    portEXIT_CRITICAL(&s_mux);
    return out;
}

void io_owner_get_all(io_owner_slot_t out[IO_OWNER_NUM_SLOTS])
{
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_slots, sizeof(s_slots));
    portEXIT_CRITICAL(&s_mux);
}

// -----------------------------------------------------------------------------
// BBP caller context
// -----------------------------------------------------------------------------

void io_owner_set_current_bbp_caller(const io_owner_t *caller)
{
    portENTER_CRITICAL(&s_mux);
    s_current_bbp_caller = *caller;
    portEXIT_CRITICAL(&s_mux);
}

void io_owner_clear_current_bbp_caller(void)
{
    portENTER_CRITICAL(&s_mux);
    s_current_bbp_caller = { IO_OWNER_NONE, 0, 0 };
    portEXIT_CRITICAL(&s_mux);
}

io_owner_t io_owner_get_current_bbp_caller(void)
{
    portENTER_CRITICAL(&s_mux);
    io_owner_t out = s_current_bbp_caller;
    portEXIT_CRITICAL(&s_mux);
    return out;
}

// -----------------------------------------------------------------------------
// Token fingerprint — HMAC-SHA256(admin_token, "bb-io-owner-fp")[0..3] as u32 LE
// -----------------------------------------------------------------------------

uint32_t io_owner_compute_token_fp(const char *admin_token)
{
    if (!admin_token || admin_token[0] == '\0') return 0;

    const char *label = "bb-io-owner-fp";
    uint8_t hmac_out[32] = {};

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return 0;

    mbedtls_md_hmac(md,
                    (const uint8_t *)admin_token, strlen(admin_token),
                    (const uint8_t *)label,       strlen(label),
                    hmac_out);

    // Return first 4 bytes as u32 LE
    uint32_t fp = ((uint32_t)hmac_out[0])
                | ((uint32_t)hmac_out[1] << 8)
                | ((uint32_t)hmac_out[2] << 16)
                | ((uint32_t)hmac_out[3] << 24);
    return fp;
}

// -----------------------------------------------------------------------------
// Guard helper
// -----------------------------------------------------------------------------

#if defined(BB_IO_OWNERSHIP) && BB_IO_OWNERSHIP

// Generate a new unique session id (never 0)
static uint8_t next_session_id(void)
{
    s_session_counter++;
    if (s_session_counter == 0) s_session_counter = 1;
    return s_session_counter;
}

int io_owner_guard_or_auto(uint8_t slot_idx, io_owner_kind_t kind,
                            uint8_t session_id, uint32_t token_fp32,
                            uint64_t now_ms)
{
    if (slot_idx >= IO_OWNER_NUM_SLOTS) return 0;  // invalid slot → don't block

    portENTER_CRITICAL(&s_mux);
    io_owner_slot_t *sl = &s_slots[slot_idx];

    if (sl->kind == IO_OWNER_NONE) {
        // Auto-claim with 30s lease for backward-compat with legacy clients
        uint8_t sid = (session_id != 0) ? session_id : next_session_id();
        sl->kind           = kind;
        sl->session_id     = sid;
        sl->token_fp32     = token_fp32;
        sl->lease_until_ms = now_ms + IO_OWNER_AUTO_LEASE_MS;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGD(TAG, "Slot %d auto-claimed by kind=%d sid=%d", slot_idx, (int)kind, (int)sid);
        return 0;
    }

    if (sl->session_id == session_id && sl->kind == kind) {
        // Same owner — allow
        portEXIT_CRITICAL(&s_mux);
        return 0;
    }

    // Different owner — reject; record for BBP event emission
    s_last_rejected_slot       = slot_idx;
    s_last_rejected_owner_kind = sl->kind;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "Slot %d owned by kind=%d sid=%d, rejected kind=%d sid=%d",
             slot_idx, (int)sl->kind, (int)sl->session_id, (int)kind, (int)session_id);
    return -CMD_ERR_IO_OWNERSHIP;
}

void io_owner_get_last_rejection(uint8_t *slot_out, io_owner_kind_t *owner_kind_out)
{
    portENTER_CRITICAL(&s_mux);
    if (slot_out)       *slot_out       = s_last_rejected_slot;
    if (owner_kind_out) *owner_kind_out = s_last_rejected_owner_kind;
    portEXIT_CRITICAL(&s_mux);
}

#endif // BB_IO_OWNERSHIP
