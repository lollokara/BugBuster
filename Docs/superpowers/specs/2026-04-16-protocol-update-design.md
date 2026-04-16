# Spec: BugBuster Protocol Update (v1.6 / BBP v4)

**Date:** 2026-04-16
**Status:** Approved
**Reference Firmware:** v1.2.1

## 1. Overview
The BugBuster Binary Protocol (BBP) and the associated REST API have evolved to support new hardware features, specifically the expanded HAT Logic Analyzer capabilities, the 4-device MUX Matrix, and enhanced security for admin tasks. This spec synchronizes `BugBusterProtocol.md` with the current firmware implementation.

## 2. Protocol Versioning
- **BBP Protocol Version:** 4
- **Specification Version:** 1.6

## 3. BBP Command & Event Updates

### 3.1 Status Payloads (Expanded)
- **0x01 GET_STATUS:**
    - Appends a 4-byte **MUX State** section (offset 155–158).
    - Each byte represents one ADGS2414D device in the daisy-chain (Bit 0 = S1, Bit 7 = S8).
- **0xC5 HAT_GET_STATUS:**
    - Expanded to 34 bytes (from ~15).
    - Includes: `fw_major/minor`, `config_confirmed`, `ext1..4_func`, `connector_a/b` status (3 bytes each: enabled, current_ma, fault), `io_voltage_mv`, `hvpak_part`, `hvpak_ready`, `hvpak_last_error`, `dap_connected`, `target_detected`, `target_dpidr`.
- **0xD7 HAT_LA_STATUS:**
    - Includes USB connection/mounting status and streaming error counters (`stream_overrun_count`, `stream_short_write_count`).

### 3.2 New BBP Commands
- **0x74 GET_ADMIN_TOKEN:** Retrieves the transient admin token. USB (CDC #0) only.
- **0xEB HAT_LA_LOG_ENABLE:** Enables/disables the relay of RP2040 UART logs as BBP events.
- **0xED HAT_LA_USB_RESET:** Reset the RP2040 vendor bulk endpoint.
- **0xEE HAT_LA_STREAM_START:** Initiate continuous LA streaming over USB.

### 3.3 New BBP Event
- **0xEC LA_LOG:** Pushes a length-prefixed string message from the RP2040 HAT.

### 3.4 MUX Switch Matrix (0x90–0x92)
- **0x90 MUX_SET_ALL:** Set all 32 switches (4 bytes). Includes 100ms safety interlock.
- **0x91 MUX_GET_ALL:** Read all 32 switch states (4 bytes).
- **0x92 MUX_SET_SWITCH:** Set single switch `[dev:u8, sw:u8, closed:bool]`.

## 4. Enum & Error Updates
- **HatPinFunction:** Marked codes 1–4 as **RESERVED** (SWD pins are now dedicated).
- **Error Codes:** Added `ERR_TIMEOUT` (0x11) and `HVPAK` specific errors (0x0B–0x10).

## 5. REST API Updates
- **GET /api/debug:** Combined status of all I2C buses and attached devices (DS4424, HUSB238, PCA9535).
- **Full /api/mux/* Path:** Document the REST equivalent of the binary MUX commands.

## 6. Implementation Plan
1. Update version headers and transport notes.
2. Revise `6.2 Status & Information` (0x01, 0xC5).
3. Revise `6.6 GPIO Control` (Add 0x74).
4. Revise `6.13 HAT Expansion Board` (0xC5, 0xC6, 0xC7).
5. Add `6.13c HAT Logic Analyzer` missing commands (0xEB, 0xED, 0xEE).
6. Complete `6.15 MUX Switch Matrix` (0x90-0x92).
7. Update `6.10 Enum Reference Tables` and `Error Codes` section.
8. Add `/api/debug` to REST section.
9. Add `LA_LOG` (0xEC) to Events section.
