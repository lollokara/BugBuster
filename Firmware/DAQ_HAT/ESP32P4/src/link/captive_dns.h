#pragma once

// =============================================================================
// captive_dns.h — minimal fast-fail DNS responder for the DAQ softAP.
//
// Purpose: makes the phone's (or any OS's) internet-reachability probe on this
// softAP fail immediately and deterministically (NXDOMAIN) instead of timing
// out slowly, so the OS correctly and quickly classifies the network as
// "joined, no internet" and keeps its default/general traffic on cellular --
// while the DAQ TCP stream still reaches the P4 fine since it connects by
// literal IP (never needing DNS). See .mex/patterns/daq-hat-ios-wifi-streaming.md.
//
// Always paired with wifi_ap_start()/wifi_ap_stop(): started right after the
// softAP comes up, stopped in both the STOP path and any failure-rollback path.
// =============================================================================

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bind a raw UDP:53 responder that answers every query with NXDOMAIN. */
esp_err_t captive_dns_start(void);

/** @brief Tear down the UDP:53 responder. Safe to call if not started. */
void captive_dns_stop(void);

#ifdef __cplusplus
}
#endif
