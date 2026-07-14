// =============================================================================
// pd_manager.cpp — USB-C PD profile manager (firmware-side)
//
// Selects the lowest USB-C PD profile that satisfies all active consumers,
// negotiates via the HUSB238 sink controller, and verifies the new contract.
//
// Sequencing is the caller's responsibility — see pd_manager.h.
// =============================================================================

#include "pd_manager.h"
#include "husb238.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pd_mgr";

// Time to wait after issuing GO_SELECT_PDO before reading back the new contract.
// USB PD negotiation (SOP messaging + contract) typically finishes in 200–400 ms;
// 500 ms gives a comfortable margin.
#define PD_RENEGOTIATE_WAIT_MS  500u

// ── Profile table ─────────────────────────────────────────────────────────
// Ascending voltage order.  Must stay in sync with Husb238Voltage enum.
typedef struct {
    Husb238Voltage code;
    float          volts;
} PdProfile;

static const PdProfile kProfiles[] = {
    { HUSB238_V_5V,  5.0f  },
    { HUSB238_V_9V,  9.0f  },
    { HUSB238_V_12V, 12.0f },
    { HUSB238_V_15V, 15.0f },
    { HUSB238_V_18V, 18.0f },
    { HUSB238_V_20V, 20.0f },
};
static const int kProfileCount = (int)(sizeof(kProfiles) / sizeof(kProfiles[0]));

// Map profile table index → PDO info pointer in the HUSB238 state struct.
static const Husb238PdoInfo *pdo_for_idx(const Husb238State *st, int idx)
{
    switch (idx) {
        case 0: return &st->pdo_5v;
        case 1: return &st->pdo_9v;
        case 2: return &st->pdo_12v;
        case 3: return &st->pdo_15v;
        case 4: return &st->pdo_18v;
        case 5: return &st->pdo_20v;
        default: return NULL;
    }
}

// ── Consumer registry ─────────────────────────────────────────────────────
typedef struct {
    float          output_v;  // 0 = idle/inactive
    PdConsumerType type;
} ConsumerState;

static ConsumerState     s_consumers[PD_MAX_CONSUMERS];
static SemaphoreHandle_t s_mutex = NULL;

// ── Internal helpers ──────────────────────────────────────────────────────

// Minimum PD input voltage (V) required by a single consumer.
static float consumer_pd_need(const ConsumerState *c)
{
    if (c->output_v <= 0.0f) return 0.0f;
    if (c->type == PD_TYPE_BUCK) {
        // Buck: input must exceed output by at least PD_DCDC_HEADROOM_V.
        return c->output_v + PD_DCDC_HEADROOM_V;
    }
    // Buck-boost: prefer pd_v ≥ out_v so the converter runs in buck mode;
    // the hardware can boost above the input when absolutely necessary.
    return c->output_v;
}

// Minimum PD voltage (V) the board requests even when all consumers are idle.
// 9 V gives the on-board regulators comfortable headroom and avoids the very
// narrow operating window of 5 V profiles on marginal cables/chargers.
#define PD_BOARD_MIN_V  9.0f

// Global minimum PD bus voltage (V) that satisfies every active consumer.
// Never drops below PD_BOARD_MIN_V even when all consumers are idle.
static float calc_required_pd(void)
{
    float max_v = PD_BOARD_MIN_V;
    for (int i = 0; i < PD_MAX_CONSUMERS; i++) {
        float need = consumer_pd_need(&s_consumers[i]);
        if (need > max_v) max_v = need;
    }
    return max_v;
}

// Choose the lowest profile index whose voltage covers required_v and that
// the source currently offers.  Falls back to the highest offered profile
// when nothing covers it (handles the buck-boost "need 30 V, best we have is 20 V" case).
static int select_profile(const Husb238State *st, float required_v)
{
    for (int i = 0; i < kProfileCount; i++) {
        if (kProfiles[i].volts >= required_v - 0.5f) {
            const Husb238PdoInfo *pdo = pdo_for_idx(st, i);
            if (pdo && pdo->detected) return i;
        }
    }
    // Nothing covers required_v — return the highest offered profile.
    for (int i = kProfileCount - 1; i >= 0; i--) {
        const Husb238PdoInfo *pdo = pdo_for_idx(st, i);
        if (pdo && pdo->detected) return i;
    }
    return 0;  // last-resort: 5 V
}

// ── Public API ────────────────────────────────────────────────────────────

void pd_manager_init(void)
{
    memset(s_consumers, 0, sizeof(s_consumers));
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex — PD manager will operate without lock");
    }
    ESP_LOGI(TAG, "PD manager ready (%d consumer slots)", PD_MAX_CONSUMERS);

    // The HUSB238 hardware defaults to 20 V at boot even without I2C commands.
    // With all consumers idle the minimum needed is 5 V.  Renegotiate down now
    // so we only draw what we actually need; consumers will negotiate up via
    // pd_manager_ensure() as they are activated.
    char boot_warn[128] = {0};
    bool ok = pd_manager_ensure(PD_CONSUMER_VADJ1, 0.0f, PD_TYPE_BUCK,
                                boot_warn, sizeof(boot_warn));
    if (!ok && boot_warn[0])
        ESP_LOGW(TAG, "Boot PD optimization: %s", boot_warn);
}

float pd_manager_consumer_v(PdConsumerId id)
{
    if ((int)id < 0 || (int)id >= PD_MAX_CONSUMERS) return 0.0f;
    return s_consumers[(int)id].output_v;  // float read is atomic on Xtensa/ESP32
}

float pd_manager_required_v(void)
{
    bool locked = s_mutex && (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE);
    float v = calc_required_pd();
    if (locked) xSemaphoreGive(s_mutex);
    return v;
}

bool pd_manager_ensure(PdConsumerId id, float output_v, PdConsumerType type,
                       char *warn, size_t warn_len)
{
    if (warn && warn_len > 0) warn[0] = '\0';

    if ((int)id < 0 || (int)id >= PD_MAX_CONSUMERS) {
        if (warn && warn_len > 0)
            snprintf(warn, warn_len, "pd_manager: invalid consumer id %d", (int)id);
        return false;
    }

    // Take the mutex.  Generous timeout: negotiation itself blocks ~500 ms, so
    // a concurrent caller will naturally queue up here.
    bool have_lock = s_mutex && (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1500)) == pdTRUE);

    // ── 1. Update consumer registry ───────────────────────────────────────
    s_consumers[(int)id].output_v = (output_v > 0.0f) ? output_v : 0.0f;
    s_consumers[(int)id].type     = type;

    // ── 2. Refresh HUSB238 state ──────────────────────────────────────────
    husb238_update();
    const Husb238State *st = husb238_get_state();

    if (!st->present) {
        // Running from data-USB 5 V — no PD controller to negotiate with.
        float needed = calc_required_pd();
        bool ok = (needed <= 5.5f);
        if (!ok && warn && warn_len > 0) {
            snprintf(warn, warn_len,
                "USB-C PD controller not detected; %.1f V required but unavailable "
                "(check USB-C cable / charger)",
                (double)needed);
        }
        if (have_lock) xSemaphoreGive(s_mutex);
        return ok;
    }

    if (!st->attached) {
        if (warn && warn_len > 0)
            snprintf(warn, warn_len,
                "USB-C not attached; cannot negotiate PD profile");
        if (have_lock) xSemaphoreGive(s_mutex);
        return false;
    }

    // ── 3. Find the target profile and check whether we're already there ───
    float required_pd = calc_required_pd();
    float current_v   = st->voltage_v;

    // Select the minimum offered profile that covers the global requirement.
    // This is the single source of truth — used for both upgrades (current < target)
    // AND downgrades (current > target, e.g. boot at 20 V with no consumers active).
    int   target_idx = select_profile(st, required_pd);
    float target_v   = kProfiles[target_idx].volts;

    if (fabsf(current_v - target_v) < 0.5f) {
        ESP_LOGD(TAG, "PD %.0fV already at target %.0fV — no-op",
                 (double)current_v, (double)target_v);
        if (have_lock) xSemaphoreGive(s_mutex);
        return true;
    }

    // ── 4. Negotiate to the target profile (up or down) ──────────────────

    ESP_LOGI(TAG,
        "%s PD %.0fV → %.0fV  "
        "(consumer %d: %.1fV out, require ≥%.1fV PD)",
        (target_v > current_v) ? "Upgrading" : "Downgrading",
        (double)current_v, (double)target_v,
        (int)id, (double)output_v, (double)required_pd);

    // ── 5. Write profile selection and issue GO_SELECT_PDO ────────────────
    bool ok = husb238_select_pdo(kProfiles[target_idx].code)
           && husb238_go_command(HUSB238_GO_SELECT_PDO);

    if (!ok) {
        if (warn && warn_len > 0)
            snprintf(warn, warn_len,
                "PD profile write/GO command failed (HUSB238 I2C error)");
        ESP_LOGE(TAG, "HUSB238 I2C error during PDO selection");
        if (have_lock) xSemaphoreGive(s_mutex);
        return false;
    }

    // Release lock during the wait so other tasks are not starved.
    if (have_lock) xSemaphoreGive(s_mutex);

    // ── 6. Wait for renegotiation ─────────────────────────────────────────
    vTaskDelay(pdMS_TO_TICKS(PD_RENEGOTIATE_WAIT_MS));

    // ── 7. Verify the new contract ────────────────────────────────────────
    husb238_update();
    st = husb238_get_state();
    float new_v = st->voltage_v;

    bool sufficient = (new_v >= required_pd - 0.5f);
    if (sufficient) {
        ESP_LOGI(TAG, "PD now at %.0fV (required ≥%.1fV) — OK",
                 (double)new_v, (double)required_pd);
    } else {
        if (warn && warn_len > 0) {
            snprintf(warn, warn_len,
                "PD renegotiated to %.0fV but %.1fV was required; "
                "source may not offer a sufficient profile",
                (double)new_v, (double)required_pd);
        }
        ESP_LOGW(TAG,
            "PD settled at %.0fV, required %.1fV — "
            "source may not support a higher profile",
            (double)new_v, (double)required_pd);
    }

    return sufficient;
}
