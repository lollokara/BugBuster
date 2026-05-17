#pragma once

// =============================================================================
// io_owner.h - IO Ownership Table
//
// 16-slot firmware-resident ownership table.
// IO1..IO12  → indices 0..11  (io_num - 1)
// CH0..CH3   → indices 12..15 (ch + 12)
//
// Cross-core safe via portMUX_TYPE.
// Lease tick must be called from the main loop.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IO_OWNER_NUM_SLOTS  16

// Owner kind — who holds the slot
typedef enum {
    IO_OWNER_NONE     = 0,
    IO_OWNER_USB      = 1,   // BBP client over USB CDC
    IO_OWNER_HTTP     = 2,   // REST/HTTP client
    IO_OWNER_SCRIPT   = 3,   // On-device scripting engine
    IO_OWNER_CLI      = 4,   // Interactive CLI user
    IO_OWNER_INTERNAL = 5,   // Firmware-internal (selftest, calibration)
} io_owner_kind_t;

// Opaque record for one owned slot
typedef struct {
    io_owner_kind_t kind;       // Who owns it
    uint8_t         session_id; // Rolling session counter (0 = not owned)
    uint32_t        token_fp32; // HMAC-SHA256(admin_token, "bb-io-owner-fp")[0..3] LE for HTTP owners
    uint64_t        lease_until_ms; // 0 = infinite; else auto-release after this
} io_owner_slot_t;

// Public API

/** Initialise the table (all slots free). Call once at boot. */
void io_owner_init(void);

/**
 * @brief Tick leases. Call from the main loop every iteration.
 * @param now_ms  esp_timer_get_time() / 1000
 */
void io_owner_tick(uint64_t now_ms);

/**
 * @brief Attempt to acquire a slot.
 * @param slot_idx  0..15
 * @param kind      Requester kind
 * @param session_id   Caller's session byte (used for release matching)
 * @param token_fp32   HTTP token fingerprint (0 for non-HTTP callers)
 * @param lease_ms     Lease duration in ms (0 = infinite)
 * @param now_ms       Current monotonic time
 * @return true on success (slot was free or already owned by same session)
 */
bool io_owner_acquire(uint8_t slot_idx, io_owner_kind_t kind,
                      uint8_t session_id, uint32_t token_fp32,
                      uint32_t lease_ms, uint64_t now_ms);

/**
 * @brief Release a slot.
 * @param slot_idx   0..15
 * @param session_id Must match the owning session (or be 0 to force)
 * @return true if released, false if not owned or session mismatch
 */
bool io_owner_release(uint8_t slot_idx, uint8_t session_id);

/**
 * @brief Force-release a slot regardless of session (admin only).
 */
void io_owner_force_release(uint8_t slot_idx);

/**
 * @brief Release every slot currently owned by (@p kind, @p session_id).
 *        Called from bbpCdcNewConnection() to drop leases of a USB session
 *        that just disappeared (DTR drop / re-enumeration) so stale leases
 *        do not block the next session.
 * @param kind        Owner kind to match.
 * @param session_id  Session byte to match.
 * @return Number of slots released.
 */
uint8_t io_owner_release_session(io_owner_kind_t kind, uint8_t session_id);

/**
 * @brief Release all slots whose kind is not @p preserved_kind and not
 *        IO_OWNER_NONE. Called on USB re-handshake to evict stale
 *        HTTP/SCRIPT/CLI entries that the new host cannot reclaim by
 *        session_id. IO_OWNER_INTERNAL slots (selftest, calibration) are
 *        also preserved so live holds are not nuked.
 * @param preserved_kind  Kind to keep (pass IO_OWNER_INTERNAL to preserve
 *                        firmware-internal holds).
 * @return Number of slots released.
 */
uint8_t io_owner_release_all_except(io_owner_kind_t preserved_kind);

/**
 * @brief Check if a slot is free.
 */
bool io_owner_is_free(uint8_t slot_idx);

/**
 * @brief Get a copy of the current slot state.
 */
io_owner_slot_t io_owner_get(uint8_t slot_idx);

/**
 * @brief Copy all 16 slot records into @p out.
 */
void io_owner_get_all(io_owner_slot_t out[IO_OWNER_NUM_SLOTS]);

/**
 * @brief Compute the 32-bit token fingerprint for HTTP ownership records.
 *        Returns HMAC-SHA256(admin_token, "bb-io-owner-fp")[0..3] as u32 LE.
 *        Uses mbedtls; safe to call from any task context.
 * @param admin_token  NUL-terminated admin token string.
 * @return Fingerprint as u32 LE (4 bytes of HMAC output).
 */
uint32_t io_owner_compute_token_fp(const char *admin_token);

// -----------------------------------------------------------------------------
// BBP caller context — set by bbp.cpp before dispatch, cleared after.
// Handlers in the registry can call io_owner_get_current_bbp_caller() to
// discover which BBP session triggered them (no caller context in handler sig).
// -----------------------------------------------------------------------------

typedef struct {
    io_owner_kind_t kind;
    uint8_t         session_id;
    uint32_t        token_fp32;
} io_owner_t;

void io_owner_set_current_bbp_caller(const io_owner_t *caller);
void io_owner_clear_current_bbp_caller(void);
io_owner_t io_owner_get_current_bbp_caller(void);

// -----------------------------------------------------------------------------
// Guard helper used by BBP command handlers and HTTP handlers.
//
// If BB_IO_OWNERSHIP is enabled:
//   - If slot is free: auto-claim with 30s lease (backward compat for v4 clients)
//   - If slot is owned by same session: OK
//   - If slot is owned by different session: return -CMD_ERR_IO_OWNERSHIP
//
// Returns 0 on success, -CMD_ERR_IO_OWNERSHIP on rejection.
// -----------------------------------------------------------------------------

#if defined(BB_IO_OWNERSHIP) && BB_IO_OWNERSHIP
int io_owner_guard_or_auto(uint8_t slot_idx, io_owner_kind_t kind,
                            uint8_t session_id, uint32_t token_fp32,
                            uint64_t now_ms);

/**
 * @brief Retrieve details of the most recent ownership rejection.
 *        Set atomically inside io_owner_guard_or_auto on rejection.
 * @param slot_out       Receives the rejected slot index.
 * @param owner_kind_out Receives the current owner's kind.
 */
void io_owner_get_last_rejection(uint8_t *slot_out, io_owner_kind_t *owner_kind_out);
#else
// Feature disabled: always succeed
static inline int io_owner_guard_or_auto(uint8_t slot_idx, io_owner_kind_t kind,
                                          uint8_t session_id, uint32_t token_fp32,
                                          uint64_t now_ms)
{
    (void)slot_idx; (void)kind; (void)session_id; (void)token_fp32; (void)now_ms;
    return 0;
}
static inline void io_owner_get_last_rejection(uint8_t *slot_out, io_owner_kind_t *owner_kind_out)
{
    if (slot_out) *slot_out = 0;
    if (owner_kind_out) *owner_kind_out = IO_OWNER_NONE;
}
#endif

// Convenience: convert io number (1..12) to slot index
static inline uint8_t io_owner_slot_for_io(uint8_t io_num)
{
    return (uint8_t)(io_num - 1);  // 0..11
}

// Convenience: convert channel (0..3) to slot index
static inline uint8_t io_owner_slot_for_ch(uint8_t ch)
{
    return (uint8_t)(ch + 12);  // 12..15
}

#ifdef __cplusplus
}
#endif
