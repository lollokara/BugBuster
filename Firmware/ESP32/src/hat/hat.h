#pragma once

// =============================================================================
// hat.h - HAT (Hardware Attached on Top) Expansion Board Driver
//
// Manages detection, communication, and configuration of HAT expansion boards
// connected via the HAT header on the BugBuster PCB.
//
// Physical interface:
//   GPIO47              - HAT detect: binary strap
//                         HIGH = no HAT
//                         LOW  = HAT present
//   GPIO43 (TXD0)       - UART TX to HAT (BugBuster is master)
//   GPIO44 (RXD0)       - UART RX from HAT
//   GPIO15 (open-drain) - Shared interrupt line (bidirectional)
//
// HAT connector signals:
//   EXP_EXT_1..4        - 4 configurable I/O lines routed through HAT
//                         Each can be independently assigned to a function
//
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Pin Definitions
// -----------------------------------------------------------------------------
#define HAT_NO_DETECT       0
#define PIN_HAT_DETECT      GPIO_NUM_47   // Digital detect strap: HIGH=no HAT, LOW=HAT
#define PIN_HAT_TX          GPIO_NUM_43   // UART TX to HAT
#define PIN_HAT_RX          GPIO_NUM_44   // UART RX from HAT
#define PIN_HAT_IRQ         GPIO_NUM_15   // Shared open-drain interrupt (also carries LA-done signal on PCB)
// On the PCB the dedicated LA-done wire is retired: GPIO18 is now SPI SDO for
// the AD74416H, and the RP2040 LA-done pulse is delivered via PIN_HAT_IRQ
// (shared with other HAT interrupts). hat.cpp guards (pin >= 0), so NC is safe.
#define PIN_HAT_LA_DONE_IRQ (-1)

#define HAT_UART_NUM        UART_NUM_0
#define HAT_UART_BAUD       921600
#define HAT_UART_BUF_SIZE   512
#define HAT_DEFAULT_IO_VOLTAGE_MV 3300

// -----------------------------------------------------------------------------
// HAT Detection — Binary strap
// -----------------------------------------------------------------------------
// High level: no HAT
// Low level:  HAT present

typedef enum {
    HAT_TYPE_NONE = 0,      // No HAT detected (GPIO47 high)
    HAT_TYPE_SWD_GPIO,      // HAT detected (GPIO47 low)
    HAT_TYPE_DAQ_POWER = 0x10, // DAQ HAT (ESP32-P4 power analyzer); reported via GET_INFO
    // Future HAT types:
    // HAT_TYPE_ANALOG,     // e.g. 4.7kΩ pull-down (~1.06V)
    // HAT_TYPE_PROTOCOL,   // e.g. 22kΩ pull-down (~2.27V)
    HAT_TYPE_UNKNOWN = 0xFF,
} HatType;

// -----------------------------------------------------------------------------
// EXP_EXT Pin Functions
// -----------------------------------------------------------------------------

typedef enum {
    HAT_FUNC_DISCONNECTED = 0,  // Pin not assigned
    // Slots 1..4 are RESERVED for wire-protocol compatibility.
    // They used to be SWDIO, SWCLK, TRACE1, TRACE2 but SWD now has a
    // dedicated 3-pin connector (new HAT PCB, 2026-04-09) driven directly
    // by the RP2040 debugprobe pins. These function codes are no longer
    // assignable to EXP_EXT pins — hat_set_pin() rejects them with
    // HAT_ERR_INVALID_FUNC. See
    // .omc/specs/deep-interview-swd-exp-ext-cleanup-2026-04-09.md.
    HAT_FUNC_RESERVED_1 = 1,    // formerly SWDIO
    HAT_FUNC_RESERVED_2 = 2,    // formerly SWCLK
    HAT_FUNC_RESERVED_3 = 3,    // formerly TRACE1
    HAT_FUNC_RESERVED_4 = 4,    // formerly TRACE2
    HAT_FUNC_GPIO1      = 5,    // General-purpose I/O 1
    HAT_FUNC_GPIO2      = 6,    // General-purpose I/O 2
    HAT_FUNC_GPIO3      = 7,    // General-purpose I/O 3
    HAT_FUNC_GPIO4      = 8,    // General-purpose I/O 4
    HAT_FUNC_COUNT      = 9,
} HatPinFunction;

#define HAT_NUM_EXT_PINS    4   // EXP_EXT_1 through EXP_EXT_4

// -----------------------------------------------------------------------------
// HAT UART Protocol — Command/Response Framing
// -----------------------------------------------------------------------------
// Frame: [SYNC(0xAA)] [LEN(1)] [CMD(1)] [PAYLOAD(0..N)] [CRC8(1)]
// SYNC = 0xAA, LEN = payload length (excluding SYNC, LEN, CMD, CRC)
// CRC-8 polynomial 0x07 over CMD + PAYLOAD bytes
//
// BugBuster (master) sends commands, HAT (slave) sends responses.
// HAT can also assert IRQ to signal unsolicited status change.

#define HAT_FRAME_SYNC      0xAA
#define HAT_FRAME_MAX_LEN   32      // Max payload length

// -----------------------------------------------------------------------------
// DAQ HAT telemetry push (HAT_CMD_DAQ_TELEMETRY 0x5A)
// -----------------------------------------------------------------------------
// S3 mainboard telemetry forwarded to the DAQ HAT (ESP32-P4) once per second so
// the P4 can relay it to the C6 Diagnostics menu. Compact fixed-point, little-
// endian, <= HAT_FRAME_MAX_LEN bytes. MUST stay byte-for-byte identical to the
// P4-side mirror in Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h.
#define HAT_DAQ_TLM_NA      ((int16_t)0x7FFF)  // sentinel: value unreadable
#define HAT_DAQ_TLM_F_PD    0x01u  // USB-PD attached / contract valid
#define HAT_DAQ_TLM_F_RAILS 0x02u  // VADJ1/VADJ2/VLOGIC fields valid
#define HAT_DAQ_TLM_F_DIE   0x04u  // die_temp_c10 valid

typedef struct __attribute__((packed)) {
    int16_t  die_temp_c10;   // AD74416H die temp, 0.1 C (HAT_DAQ_TLM_NA if invalid)
    uint16_t pd_mv;          // USB-PD negotiated voltage, mV (0 if unattached)
    uint16_t pd_ma;          // USB-PD negotiated current cap, mA
    uint16_t vadj1_mv;       // VADJ1_BUCK rail, mV (0 if unavailable)
    uint16_t vadj2_mv;       // VADJ2_BUCK rail, mV
    uint16_t vlogic_mv;      // 3V3_ADJ / VLOGIC rail, mV
    uint8_t  flags;          // HAT_DAQ_TLM_F_*
} hat_daq_telemetry_t;       // 13 bytes

// HAT_CMD_DAQ_ARM (0x5B) payload — MUST stay byte-for-byte identical to the
// P4-side mirror s3link_daq_arm_t in Firmware/DAQ_HAT/.../link/s3_link.h.
typedef struct __attribute__((packed)) {
    uint8_t  armed;          // 1 = arm trigger latch, 0 = free-run/disarm
    uint8_t  trig_logic;     // 0 = none, 1 = OR, 2 = AND (info only; S3 evaluates)
    uint16_t _pad;
    uint32_t pre_samples;    // requested pre-trigger depth (fused samples)
} hat_daq_arm_t;

// HAT_CMD_DAQ_MARK (0x5C) payload — MUST stay byte-for-byte identical to the
// P4-side mirror s3link_daq_mark_t. Sent when an S3 IO edge event fires.
#define HAT_DAQ_MARK_KIND_FLAG     0u  // informational flag (vertical line)
#define HAT_DAQ_MARK_KIND_TRIGGER  1u  // acquisition trigger fired (defines t=0)
typedef struct __attribute__((packed)) {
    uint8_t  channel;        // S3 IO number (1..12)
    uint8_t  edge;           // 0 = falling, 1 = rising
    uint8_t  kind;           // HAT_DAQ_MARK_KIND_*
    uint8_t  _pad;
} hat_daq_mark_t;

// -----------------------------------------------------------------------------
// DAQ WiFi streaming trigger (HAT_CMD_DAQ_WIFI_STREAM_START/STOP/INFO)
// -----------------------------------------------------------------------------
// Lets the iOS app (over BLE) trigger the P4 to bring up a softAP for direct
// WiFi streaming of DAQ data. The S3 relays START/STOP and polls INFO for the
// resulting connection credentials, chunked over the HAT UART link.
#define HAT_WIFI_STREAM_INFO_HDR   3u     // [status][seq][flags]
#define HAT_WIFI_STREAM_LAST       0x01u  // flags bit: final chunk

#define HAT_WIFI_STREAM_STARTING  0u  // softAP not up yet, no data this chunk
#define HAT_WIFI_STREAM_READY     1u  // this is the final chunk (LAST flag set), data is valid
#define HAT_WIFI_STREAM_FAILED    2u  // P4 failed to bring up the softAP

// Local S3-side tracking state for hat_daq_wifi_stream_info_t.state (distinct
// from the HAT_WIFI_STREAM_* wire status byte values above).
typedef enum {
    HAT_DAQ_WIFI_STREAM_IDLE = 0,
    HAT_DAQ_WIFI_STREAM_STARTING,
    HAT_DAQ_WIFI_STREAM_READY,
    HAT_DAQ_WIFI_STREAM_FAILED,
} HatDaqWifiStreamState;

// Wire blob layout for the reassembled HAT_CMD_DAQ_WIFI_STREAM_INFO payload.
// Fixed-width, 104 bytes total. MUST stay byte-for-byte identical to the
// P4-side mirror.
//   offset 0   : ssid[33]      (32 chars + NUL)
//   offset 33  : password[65]  (64 chars + NUL)
//   offset 98  : port u16 LE
//   offset 100 : host[4]       (raw IPv4 octets)
#define HAT_WIFI_STREAM_BLOB_LEN  104u

typedef struct {
    HatDaqWifiStreamState state;
    char    ssid[33];
    char    password[65];
    uint16_t port;
    uint8_t host[4];        // raw IPv4 octets, e.g. {192,168,4,1}
} hat_daq_wifi_stream_info_t;

// -----------------------------------------------------------------------------
// Mainboard settings tunnel (HAT_CMD_MB_POLL / HAT_CMD_MB_RESULT)
// -----------------------------------------------------------------------------
// The C6 Main Board Settings menu reads/writes S3 rails/efuses through the P4.
// The S3 polls the P4 for a pending C6 request and returns the result. Request
// types + the power struct MUST match ddp_proto.h (DDP_MB_* / ddp_mb_power_t).
#define HAT_MB_POWER        0x01u  // read rail setpoints + efuse status
#define HAT_MB_SET_RAIL     0x02u  // args: u8 rail (0=VLOGIC,1=VADJ1,2=VADJ2), u16 mv
#define HAT_MB_SET_EFUSE    0x03u  // args: u8 idx (0..3), u8 on
#define HAT_MB_SCRIPTS      0x04u  // (increment 2) script list + status
#define HAT_MB_SCRIPT_RUN   0x05u
#define HAT_MB_SCRIPT_STOP  0x06u
#define HAT_MB_SET_RAIL_EN  0x07u  // args: u8 rail (0=VLOGIC/lshift,1=VADJ1,2=VADJ2), u8 on
#define HAT_MB_ST_OK        0x00u
#define HAT_MB_ST_BUSY      0x01u
#define HAT_MB_ST_ERR       0x02u

// MicroPython engine states (first byte of the scripts result blob). MUST match
// ddp_proto.h DDP_MB_SCR_*.
#define HAT_MB_SCR_IDLE     0u
#define HAT_MB_SCR_RUNNING  1u
#define HAT_MB_SCR_CRASHED  2u
#define HAT_MB_SCR_EXITED   3u

// HAT_CMD_MB_RESULT chunk framing: [u8 type][u8 status][u8 seq][u8 flags][data].
// The result blob is split into <=(HAT_FRAME_MAX_LEN-4)-byte chunks over the
// 32-byte HAT link; the P4 reassembles them before relaying to the C6.
#define HAT_MB_RSLT_HDR     4u
#define HAT_MB_RSLT_LAST    0x01u   // flags bit: final chunk

typedef struct __attribute__((packed)) {
    uint16_t vlogic_mv;      // VLOGIC setpoint (DS4424 ch0), mV
    uint16_t vadj1_mv;       // VADJ1 setpoint  (DS4424 ch1), mV
    uint16_t vadj2_mv;       // VADJ2 setpoint  (DS4424 ch2), mV
    uint8_t  efuse_en;       // bit i (0..3) = e-fuse (i+1) enabled
    uint8_t  efuse_flt;      // bit i (0..3) = e-fuse (i+1) fault active
    uint8_t  rail_en;        // bit0=VLOGIC/level-shifter OE, bit1=VADJ1_EN, bit2=VADJ2_EN
    uint8_t  rail_pg;        // bit1=VADJ1 power-good, bit2=VADJ2 power-good
} hat_mb_power_t;

// Commands (master → slave): Core (0x01–0x0F)
#define HAT_CMD_PING            0x01
#define HAT_CMD_GET_INFO        0x02
#define HAT_CMD_SET_PIN_CONFIG  0x03
#define HAT_CMD_GET_PIN_CONFIG  0x04
#define HAT_CMD_RESET           0x05
#define HAT_CMD_GET_CAPS        0x06

// Commands: Power Management (0x10–0x1F)
#define HAT_CMD_SET_POWER       0x10  // Enable/disable connector power
#define HAT_CMD_GET_POWER_STATUS 0x11 // Read power state + current
#define HAT_CMD_SET_IO_VOLTAGE  0x12  // Set IO level (mV)
#define HAT_CMD_GET_IO_VOLTAGE  0x13  // Read current I/O voltage setting

// Commands: SWD Management (0x20–0x2F)
#define HAT_CMD_GET_DAP_STATUS  0x20  // Is debugprobe USB connected? Target detected?
#define HAT_CMD_GET_TARGET_INFO 0x21  // DPIDR, SWD clock
#define HAT_CMD_SET_SWD_CLOCK   0x22  // Adjust SWD clock speed

// Commands: Logic Analyzer (0x30–0x3F)
#define HAT_CMD_LA_CONFIG       0x30  // Configure capture
#define HAT_CMD_LA_SET_TRIGGER  0x31  // Set trigger condition
#define HAT_CMD_LA_ARM          0x32  // Arm trigger
#define HAT_CMD_LA_FORCE        0x33  // Force immediate capture
#define HAT_CMD_LA_GET_STATUS   0x34  // Capture state + sample count
#define HAT_CMD_LA_READ_DATA    0x35  // Read captured data chunk
#define HAT_CMD_LA_STOP         0x36  // Abort capture
#define HAT_CMD_LA_STREAM_START 0x37  // Start streaming over vendor bulk EP
#define HAT_CMD_LA_LOG_ENABLE   0x39  // Enable/disable log relay to host
#define HAT_CMD_LA_USB_RESET    0x3A  // Reinitialize vendor bulk endpoint
// DAQ HAT (ESP32-P4) — channel-status LEDs (4 colour codes -> C6 neopixels).
#define HAT_CMD_SET_CH_LEDS     0x55
// DAQ HAT (ESP32-P4) — S3 mainboard telemetry push for the C6 Diagnostics menu
// (die temp, USB-PD contract, VADJ/VLOGIC rails). Fire-and-forget; P4 caches it
// and relays it to the C6 inside ddp_diag_t. Payload = hat_daq_telemetry_t.
#define HAT_CMD_DAQ_TELEMETRY   0x5A
// DAQ HAT (ESP32-P4) — trigger / flag marker support. The S3 owns the 12
// mainboard IOs and detects edge events; the P4 owns the sample clock and emits
// USB MARKER records aligned to the live sample index.
#define HAT_CMD_DAQ_ARM         0x5B  // arm/disarm pre-roll latch (hat_daq_arm_t)
#define HAT_CMD_DAQ_MARK        0x5C  // IO event -> emit MARKER (hat_daq_mark_t)
#define HAT_CMD_MB_POLL         0x5D  // poll P4 for a pending C6 mainboard request -> RSP_MB_REQ
#define HAT_CMD_MB_RESULT       0x5E  // [req_type][status][data] result -> P4 relays to C6
#define HAT_CMD_STAGE_READ      0x75u // read a chunk from the P4 `staging` partition -> RSP_STAGE_DATA
                                       // must match P4 HATP_CMD_STAGE_READ (s3_link.h) exactly

// VDUT (DAQ HAT programmable DUT power supply, P4 smu.{c,h}) request/reply
// commands. S3-initiated request + P4 reply, modeled on HAT_CMD_STAGE_READ
// above (NOT the one-way HAT_CMD_DAQ_TELEMETRY push pattern) -- the S3 doesn't
// own this hardware, it has to ask the P4 for status and issue writes.
// MUST match P4 s3_link.h HATP_CMD_DAQ_VDUT_* exactly.
#define HAT_CMD_DAQ_VDUT_STATUS   0x76u   // no payload -> HAT_RSP_DAQ_VDUT_STATUS
#define HAT_CMD_DAQ_VDUT_ENABLE   0x77u   // payload: u8 enable -> OK/ERROR
#define HAT_CMD_DAQ_VDUT_SETPOINT 0x78u   // payload: hat_vdut_setpoint_t -> OK/ERROR

// VDUT hardware limits, mirrored from the P4's authoritative
// Firmware/DAQ_HAT/ESP32P4/include/config.h (SMU_VDUT_MIN/MAX,
// SMU_ILIMIT_MIN_A/FULLSCALE_A) so the S3 API layer (api_core.cpp) can
// bounds-check a setpoint request before ever sending it over the HAT link,
// per the /api/daq/vdut/setpoint contract (reject out-of-range rather than
// silently clamp). The P4 re-validates against its own constants regardless.
#define HAT_DAQ_VDUT_MIN_V         1.76f
#define HAT_DAQ_VDUT_MAX_V         19.94f
#define HAT_DAQ_VDUT_ILIMIT_MIN_A  0.05f
#define HAT_DAQ_VDUT_ILIMIT_MAX_A  2.636f
#define HAT_CMD_LA_SET_ROUTE    0x3B  // Select low-speed/high-speed LA route

// Commands: HAT v2 Supplies / LEDs (0x40-0x4F)
#define HAT_CMD_GET_RAIL_STATUS 0x40
#define HAT_CMD_SET_RAIL_ENABLE 0x41
#define HAT_CMD_SET_LED_STATE   0x42
#define HAT_CMD_CALIBRATE_START  0x43
#define HAT_CMD_CALIBRATE_STATUS 0x44
#define HAT_CMD_CALIBRATE_IMPORT 0x45
#define HAT_CMD_SET_IO_BANK      0x46
#define HAT_CMD_SET_LEVEL_SHIFT  0x47
#define HAT_CMD_SET_RAIL_VOLTAGE 0x48
#define HAT_CMD_FW_BEGIN         0x49
#define HAT_CMD_FW_CHUNK         0x4A
#define HAT_CMD_FW_COMMIT        0x4B
#define HAT_CMD_FW_STATUS        0x4C
#define HAT_CMD_CALIBRATE_EXPORT 0x4D  // Read back stored cal points (paginated)

#define HAT_CMD_DAQ_WIFI_STREAM_START  0x5F  // S3->P4: start DAQ WiFi streaming (empty payload) -> 1-byte accept/reject
#define HAT_CMD_DAQ_WIFI_STREAM_STOP   0x67  // S3->P4: stop DAQ WiFi streaming (empty payload) -> 1-byte ack
#define HAT_CMD_DAQ_WIFI_STREAM_INFO   0x68  // S3->P4: poll for wifi-stream credentials -> chunked response

// Responses (slave → master)
#define HAT_RSP_OK              0x80
#define HAT_RSP_ERROR           0x81
#define HAT_RSP_INFO            0x82
#define HAT_RSP_POWER_STATUS    0x83
#define HAT_RSP_DAP_STATUS      0x84
#define HAT_RSP_LA_STATUS       0x85
#define HAT_RSP_LA_DATA         0x86
#define HAT_RSP_CAPS            0x87
#define HAT_RSP_RAIL_STATUS     0x88
#define HAT_RSP_LA_LOG          0x89  // Log message relay from RP2040
#define HAT_RSP_CALIBRATE_STATUS 0x8A
#define HAT_RSP_CALIBRATE_EXPORT 0x8B  // Paginated stored cal points
#define HAT_RSP_DAQ_WIFI_STREAM_INFO 0x8C  // Response cmd byte for HAT_CMD_DAQ_WIFI_STREAM_INFO poll
#define HAT_RSP_DAQ_VDUT_STATUS 0x98u  // Response to HAT_CMD_DAQ_VDUT_STATUS: hat_vdut_status_t.
                                        // Must match P4 HATP_RSP_DAQ_VDUT_STATUS (s3_link.h) exactly.
#define HAT_RSP_MB_REQ          0x96  // Pending C6 mainboard request from the P4
#define HAT_RSP_STAGE_DATA      0x97u // Response to HAT_CMD_STAGE_READ: firmware bytes read from
                                       // the P4 `staging` partition (0 bytes = end of staged image).
                                       // Must match P4 HATP_RSP_STAGE_DATA (s3_link.h) exactly.

// HAT_CMD_STAGE_READ request payload: ask the P4 for up to @len bytes from its
// `staging` partition at @offset. NOTE: the actual S3<->P4 UART link caps
// payloads (both directions) at HAT_FRAME_MAX_LEN (32) bytes per frame — see
// hat_send_command()/hat_command_internal() in hat.cpp. This is smaller than
// the P4-side HATP_OTA_CHUNK_MAX (236, sized for the P4's larger HATP_MAX_PAYLOAD
// frame budget), so @len must be capped to HAT_FRAME_MAX_LEN by the caller.
typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint8_t  len;
} hat_stage_read_req_t;

// HAT_CMD_DAQ_VDUT_SETPOINT payload. MUST stay byte-for-byte identical to the
// P4-side mirror s3link_vdut_setpoint_t in Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h.
typedef struct __attribute__((packed)) {
    float vdut_v;    // target DUT voltage (V)
    float ilimit_a;  // target current limit (A)
} hat_vdut_setpoint_t;

// HAT_RSP_DAQ_VDUT_STATUS payload: response to HAT_CMD_DAQ_VDUT_STATUS. MUST
// stay byte-for-byte identical to the P4-side mirror s3link_vdut_status_t.
typedef struct __attribute__((packed)) {
    uint8_t  present;
    uint8_t  enabled;
    uint8_t  fault;
    uint8_t  _pad;
    float    vdut_set_v;
    float    ilimit_set_a;
    float    meas_v;
    float    meas_i;
} hat_vdut_status_t;

// Error codes
#define HAT_ERR_INVALID_CMD     0x01
#define HAT_ERR_INVALID_PIN     0x02
#define HAT_ERR_INVALID_FUNC    0x03
#define HAT_ERR_BUSY            0x04
#define HAT_ERR_CRC             0x05
#define HAT_ERR_FRAME           0x06
#define HAT_ERR_NOT_CONNECTED   0x07
#define HAT_ERR_POWER_FAULT     0x08
#define HAT_ERR_UNSUPPORTED            0x12
#define HAT_ERR_CAL_INVALID            0x13

// HAT v2 capability flags
#define HAT_CAP_RAILS             (1u << 0)
#define HAT_CAP_LEDS              (1u << 1)
#define HAT_CAP_LA_LOW_SPEED      (1u << 2)
#define HAT_CAP_LA_HIGH_SPEED     (1u << 3)
#define HAT_CAP_SHIFTED_IO        (1u << 4)

// HAT v2 rail IDs
#define HAT_RAIL_3V3_ADJ        0
#define HAT_RAIL_VADJ3          1
#define HAT_RAIL_VADJ4          2
#define HAT_RAIL_COUNT          3

// HAT v2 LA route IDs
#define HAT_LA_ROUTE_LOW_SPEED  0
#define HAT_LA_ROUTE_HIGH_SPEED 1

// -----------------------------------------------------------------------------
// Connector / Power Types
// -----------------------------------------------------------------------------

typedef enum {
    HAT_CONNECTOR_A = 0,    // Target 1: powered by VADJ1
    HAT_CONNECTOR_B = 1,    // Target 2: powered by VADJ2
} HatConnector;

typedef struct {
    bool     enabled;           // Connector power is on
    float    current_ma;        // Measured current (if shunt present)
    bool     fault;             // Overcurrent fault detected
} HatConnectorStatus;

typedef struct {
    uint8_t  hw_revision;
    uint32_t flags;
    uint8_t  rail_count;
    uint8_t  led_count;
    uint8_t  shifted_io_count;
    uint8_t  la_routes;
    uint8_t  fw_major;
    uint8_t  fw_minor;
} HatCaps;

typedef struct {
    uint8_t  rail_id;
    bool     enabled;
    uint16_t voltage_mv;
    uint16_t current_ma;
    uint8_t  status;
    uint16_t target_mv;  // DS4424 configured target (cached on ESP32, not from RP2040)
} HatRailStatus;

// -----------------------------------------------------------------------------
// HAT State
// -----------------------------------------------------------------------------

typedef struct {
    // Detection & connection
    bool         detected;                          // HAT physically present (ADC detect)
    bool         connected;                         // UART communication established
    HatType      type;                              // Detected HAT type
    float        detect_voltage;                    // Raw ADC voltage on detect pin
    uint8_t      fw_version_major;                  // HAT firmware version
    uint8_t      fw_version_minor;

    // Pin configuration
    HatPinFunction pin_config[HAT_NUM_EXT_PINS];   // Current EXP_EXT assignments
    bool         config_confirmed;                  // HAT acknowledged last config

    // Power management
    HatConnectorStatus connector[2];                // Connector A and B status
    uint16_t     io_voltage_mv;                     // I/O voltage (mV)
    HatCaps      caps;
    bool         caps_valid;
    HatRailStatus rail[HAT_RAIL_COUNT];
    uint8_t      la_route;

    // SWD management
    bool         dap_connected;                     // USB CMSIS-DAP host connected
    bool         target_detected;                   // SWD target responding
    uint32_t     target_dpidr;                      // Target DPIDR value

    // Timing
    uint32_t     last_ping_ms;                      // Last successful ping timestamp
    uint32_t     last_ok_ms;                        // Last valid UART frame timestamp
    uint32_t     last_timeout_ms;                   // Last command timeout timestamp
    uint8_t      consecutive_timeouts;              // Consecutive command timeouts
    bool         degraded;                          // Detected/connected but UART recently timed out
} HatState;

// -----------------------------------------------------------------------------
// RP2040 Debug Log Ring Buffer
// Stores the last HAT_LOG_RING_SIZE lines received as HAT_RSP_LA_LOG events.
// Thread-safe via s_log_mutex (dedicated mutex, separate from s_hat_mutex).
// -----------------------------------------------------------------------------
#define HAT_LOG_RING_SIZE  64
#define HAT_LOG_LINE_MAX   128

typedef struct {
    char    lines[HAT_LOG_RING_SIZE][HAT_LOG_LINE_MAX];
    uint8_t head;   // next write index (circular)
    uint8_t count;  // number of valid entries (0..HAT_LOG_RING_SIZE)
} HatLogRing;

/**
 * @brief Push one log line into the ring buffer (called from hat.cpp on HAT_RSP_LA_LOG).
 *        Trims to HAT_LOG_LINE_MAX-1 characters. Thread-safe via internal mutex.
 */
void hat_log_ring_push(const char *line);

/**
 * @brief Drain all available log lines as a JSON array string into out_buf.
 *        Clears the ring after draining. Thread-safe via internal mutex.
 * @return Number of bytes written to out_buf (not including NUL), or -1 on overflow.
 */
int hat_log_ring_drain(char *out_buf, size_t buf_sz);

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/**
 * @brief Initialize HAT subsystem. Sets up ADC detect, UART, and IRQ pin.
 *        PCB mode only — returns false and does nothing in breadboard mode.
 */
bool hat_init(void);

/**
 * @brief Check if a HAT is physically present (based on ADC detect).
 */
bool hat_detected(void);

/**
 * @brief Get current HAT state snapshot.
 */
const HatState* hat_get_state(void);

/**
 * @brief Read the detect pin ADC and identify the HAT type.
 *        Called periodically or on-demand.
 * @return Detected HAT type
 */
HatType hat_detect(void);

/**
 * @brief Attempt to establish UART communication with the HAT.
 *        Sends PING, waits for response.
 * @return true if HAT responded
 */
bool hat_connect(void);

/**
 * @brief Set the function of a single EXP_EXT pin.
 * @param ext_pin  Pin index 0-3 (EXP_EXT_1 to EXP_EXT_4)
 * @param func     Desired function
 * @return true if HAT acknowledged the change
 */
bool hat_set_pin(uint8_t ext_pin, HatPinFunction func);

/**
 * @brief Set all 4 EXP_EXT pin functions at once.
 * @param config   Array of 4 HatPinFunction values
 * @return true if HAT acknowledged
 */
bool hat_set_all_pins(const HatPinFunction config[HAT_NUM_EXT_PINS]);

/**
 * @brief Get the current pin config from the HAT (queries via UART).
 * @param config   Output: array of 4 HatPinFunction values
 * @return true if HAT responded
 */
bool hat_get_pin_config(HatPinFunction config[HAT_NUM_EXT_PINS]);

/**
 * @brief Reset HAT to default state (all pins disconnected).
 * @return true if HAT acknowledged
 */
bool hat_reset(void);

// --- Power Management ---

/**
 * @brief Enable or disable a target connector's power.
 * @param conn  HAT_CONNECTOR_A or HAT_CONNECTOR_B
 * @param on    true = enable, false = disable
 * @return true if HAT acknowledged
 */
bool hat_set_power(HatConnector conn, bool on);

/**
 * @brief Get power status for both connectors.
 * @return true if HAT responded
 */
bool hat_get_power_status(void);

/**
 * @brief Set the I/O level translation voltage.
 * @param mv  Target I/O voltage in millivolts
 * @return true if HAT acknowledged
 */
bool hat_set_io_voltage(uint16_t mv);

bool hat_get_caps(HatCaps *caps);
bool hat_get_rail_status(HatRailStatus rails[HAT_RAIL_COUNT], uint8_t *rail_count);
bool hat_set_rail_enable(uint8_t rail_id, bool enable);
bool hat_set_led_state(uint8_t led_index, uint8_t color_code);
void hat_update_leds(void);
bool hat_la_set_route(uint8_t route);

bool hat_calibrate_start(uint8_t rail_id, uint8_t *status_out);
bool hat_calibrate_status(uint8_t *state, uint8_t *progress, uint8_t *rail_id,
                          uint8_t *last_error, uint8_t *persist_state,
                          uint8_t *stage, uint8_t *point, int8_t *code,
                          int32_t *measured_mv, int32_t *min_mv,
                          int32_t *max_mv, int32_t *max_gap_mv,
                          int32_t *max_error_mv, uint16_t *validation_flags);
bool hat_calibrate_import(uint8_t rail_id, uint8_t count, const uint8_t *points_data, size_t data_len);
// Read back one page of stored cal points for a rail (paginated; the HAT frame
// caps a response at 255 bytes). On success: *out_total = total points stored,
// *out_valid = cal-valid flag, *out_returned = points in THIS page; codes_out/
// volts_out receive up to max_out points. Advance `start` by *out_returned.
bool hat_calibrate_export(uint8_t rail_id, uint8_t start,
                          uint8_t *out_total, bool *out_valid, uint8_t *out_returned,
                          int8_t *codes_out, float *volts_out, uint8_t max_out);
bool hat_set_io_bank(uint8_t dirs, uint8_t ups, uint8_t dns, uint8_t vals);
bool hat_set_level_shift(bool oe, bool dir, bool *oe_out, bool *dir_out);
bool hat_set_rail_voltage(uint8_t rail_id, uint16_t mv);

typedef struct {
    uint8_t state;
    uint8_t last_error;
    uint32_t bytes_written;
    uint32_t image_size;
    uint32_t expected_crc32;
    uint32_t actual_crc32;
} HatFwUpdateStatus;

uint8_t hat_get_last_error(void);

/**
 * @brief Raw HAT passthrough returning the actual response command byte (not a
 *        bool). Lets callers handle non-OK data responses such as the DAQ HAT's
 *        CONFIG_VALUE (0x93) / CONFIG_SCHEMA (0x94). Returns 0 on timeout.
 */
uint8_t hat_request(uint8_t cmd, const uint8_t *payload, uint8_t payload_len,
                    uint8_t *rsp_payload, uint8_t *rsp_len, uint32_t timeout_ms, uint8_t max_rsp_len);

bool hat_fw_begin(uint32_t image_size, uint32_t expected_crc32);
bool hat_fw_chunk(uint32_t offset, const uint8_t *data, uint8_t len, uint32_t *ack_offset);
bool hat_fw_commit(void);
bool hat_fw_status(HatFwUpdateStatus *status);

/**
 * @brief Gather S3 mainboard telemetry (die temp, USB-PD contract, VADJ/VLOGIC
 *        rails) and push it to the DAQ HAT for relay to the C6 Diagnostics menu.
 *        No-op unless a DAQ HAT (HAT_TYPE_DAQ_POWER) is connected. Fire-and-
 *        forget — call periodically (~1 Hz) from the main loop.
 */
void hat_daq_push_telemetry(void);
/**
 * @brief Poll the DAQ HAT (P4) for a pending C6 "Main Board Settings" request
 *        and execute it on the S3 (rail setpoints, e-fuse enables), returning
 *        the result for relay to the C6. No-op unless a DAQ HAT is connected;
 *        the P4 defers the request while streaming to the PC. Call ~1 Hz.
 */
void hat_daq_poll_mb(void);
/**
 * @brief Send a digital event MARKER to the DAQ HAT (P4) for the live stream.
 *        No-op unless a DAQ HAT is connected. Fire-and-forget.
 * @param io    S3 IO number (1..12) that fired.
 * @param edge  0 = falling, 1 = rising.
 * @param kind  HAT_DAQ_MARK_KIND_FLAG or HAT_DAQ_MARK_KIND_TRIGGER.
 */
void hat_daq_send_mark(uint8_t io, uint8_t edge, uint8_t kind);
/**
 * @brief Trigger the DAQ HAT (P4) to bring up its WiFi softAP for direct
 *        streaming to a client (e.g. the iOS app). No-op unless a DAQ HAT is
 *        connected. On success, resets the tracked stream info to STARTING
 *        and arms periodic polling (see hat_daq_poll_wifi_stream_info()).
 * @return true if the P4 accepted the request.
 */
bool hat_daq_wifi_stream_start(void);
/**
 * @brief Tell the DAQ HAT (P4) to tear down its WiFi softAP stream and stop
 *        periodic polling. No-op unless a DAQ HAT is connected.
 * @return true if the P4 acknowledged the request.
 */
bool hat_daq_wifi_stream_stop(void);
/**
 * @brief Poll the P4 for WiFi-stream credentials while a start is pending.
 *        No-op unless a poll is currently armed (i.e. between a successful
 *        hat_daq_wifi_stream_start() and READY/FAILED/stop). Call ~4 Hz from
 *        the main loop, alongside hat_daq_poll_mb().
 */
void hat_daq_poll_wifi_stream_info(void);
/**
 * @brief Non-blocking accessor for the current WiFi-stream state/credentials.
 * @param out Destination struct, copied from the internal tracked state.
 */
void hat_daq_wifi_stream_get_status(hat_daq_wifi_stream_info_t *out);
/**
 * @brief Human-readable/API-stable string for the connected HAT's type.
 * @return "daq" for a DAQ HAT, "unknown" otherwise (no HAT / SWD-GPIO HAT).
 */
const char *hat_get_type_string(void);
/**
 * @brief Read one chunk from the P4's `staging` partition (HAT_CMD_STAGE_READ),
 *        e.g. for pulling an ESP32 OTA image the P4 staged from USB.
 * @param offset  Byte offset into the staged image.
 * @param out     Destination buffer, must be >= len bytes.
 * @param len     Requested chunk length; caller must keep this <= HAT_FRAME_MAX_LEN.
 * @return Number of bytes read into @p out (<= len), 0 at end-of-image, or
 *         -1 on a transport error (caller should retry the same offset).
 */
int hat_stage_read(uint32_t offset, uint8_t *out, uint8_t len);

/**
 * @brief Read the DAQ HAT's VDUT (programmable DUT power supply) status:
 *        present/enabled/fault + setpoints + measured voltage/current.
 *        No-op (returns false) unless a DAQ HAT is connected.
 */
bool hat_daq_vdut_status(hat_vdut_status_t *out);

/**
 * @brief Enable/disable the DAQ HAT's VDUT (DUT power supply).
 * @return true on P4 HAT_RSP_OK, false otherwise (including no DAQ HAT).
 */
bool hat_daq_vdut_enable(bool enable);

/**
 * @brief Program the DAQ HAT's VDUT voltage + current-limit setpoints.
 *        The P4 re-validates against hardware limits and rejects (returns
 *        false) out-of-range requests rather than silently clamping.
 * @return true on P4 HAT_RSP_OK, false otherwise (including no DAQ HAT).
 */
bool hat_daq_vdut_setpoint(float vdut_v, float ilimit_a);

/**
 * @brief Arm/disarm the DAQ HAT trigger latch and record the pre-roll depth.
 *        No-op unless a DAQ HAT is connected.
 */
void hat_daq_send_arm(bool armed, uint8_t trig_logic, uint32_t pre_samples);


/**
 * @brief One-call SWD setup: set VADJ, I/O voltage, power on, route SWD pins.
 * @param target_voltage_mv  Target voltage in mV (e.g. 3300 for 3.3V)
 * @param connector          Which connector the target is on
 * @return true if all steps succeeded
 */
bool hat_setup_swd(uint16_t target_voltage_mv, HatConnector connector);

// --- SWD Management ---

/**
 * @brief Query DAP/SWD status from the HAT (USB connection, target detect, clock).
 * @return true if HAT responded
 */
bool hat_get_dap_status(void);

/**
 * @brief Actively probe the SWD target (line reset + DPIDR read) and cache the
 *        result in HatState (target_detected, target_dpidr).
 * @return true if the HAT responded (check HatState.target_detected for the
 *         actual presence of a target).
 */
bool hat_detect_target(void);

/**
 * @brief Set SWD clock speed on the HAT debugprobe.
 * @param khz  Clock speed in kHz (100–50000)
 * @return true if HAT acknowledged
 */
bool hat_set_swd_clock(uint16_t khz);

// --- Logic Analyzer ---

typedef enum {
    LA_STATE_IDLE = 0,      // Not configured or stopped
    LA_STATE_ARMED,         // Waiting for trigger
    LA_STATE_CAPTURING,     // Trigger fired, capturing data
    LA_STATE_DONE,          // Capture complete, data ready for readout
    LA_STATE_STREAMING,     // Continuous DMA→USB streaming (double-buffered)
    LA_STATE_ERROR,         // Error occurred
} LaState;

typedef enum {
    LA_STREAM_STOP_NONE = 0,
    LA_STREAM_STOP_HOST = 1,
    LA_STREAM_STOP_USB_SHORT_WRITE = 2,
    LA_STREAM_STOP_DMA_OVERRUN = 3,
} LaStreamStopReason;

typedef struct {
    uint8_t  state;         // LaState enum
    uint8_t  channels;
    uint32_t samples_captured;
    uint32_t total_samples;
    uint32_t actual_rate_hz;
    uint8_t  usb_connected;
    uint8_t  usb_mounted;
    uint8_t  stream_stop_reason;
    uint32_t stream_overrun_count;
    uint32_t stream_short_write_count;
    uint8_t  usb_rearm_pending;
    uint8_t  usb_rearm_request_count;
    uint8_t  usb_rearm_complete_count;
} HatLaStatus;

/**
 * @brief Configure LA capture parameters.
 * @param channels   1, 2, or 4
 * @param rate_hz    Sample rate in Hz
 * @param depth      Total samples to capture
 */
bool hat_la_configure(uint8_t channels, uint32_t rate_hz, uint32_t depth);

/**
 * @brief Set LA trigger condition.
 * @param type     Trigger type (0=none, 1=rising, 2=falling, 3=both, 4=high, 5=low)
 * @param channel  Channel to trigger on (0-3)
 */
bool hat_la_set_trigger(uint8_t type, uint8_t channel);

bool hat_la_arm(void);
bool hat_la_force(void);
bool hat_la_stop(void);
bool hat_la_stream_start(void);
bool hat_la_usb_reset(void);
bool hat_la_log_enable(bool enable);
bool hat_la_get_status(HatLaStatus *status);

/**
 * @brief Read a chunk of captured data from the LA buffer.
 * @param offset  Byte offset into buffer
 * @param buf     Output buffer
 * @param len     Bytes to read (max 28)
 * @return Actual bytes read
 */
uint8_t hat_la_read_data(uint32_t offset, uint8_t *buf, uint8_t len);

// --- Polling ---

/**
 * @brief Poll HAT UART for unsolicited messages (capture done, etc).
 *        Call periodically (~10ms) from a task.
 */
void hat_poll(void);

// --- LA-done IRQ (dedicated GPIO from RP2040 BB_LA_DONE_PIN) ---

/**
 * @brief Check whether the RP2040 has pulsed the LA-done line since the
 *        last consume. Set from the GPIO ISR in hat.cpp.
 * @return true if a capture-complete edge has been observed and not yet
 *         consumed.
 */
bool hat_la_done_pending(void);

/**
 * @brief Atomically read and clear the LA-done pending flag.
 * @return true if a pending edge was present (and has now been cleared);
 *         false otherwise.
 */
bool hat_la_done_consume(void);

// --- String Helpers ---

/**
 * @brief Get function name string for display.
 */
const char* hat_func_name(HatPinFunction func);

/**
 * @brief Get HAT type name string for display.
 */
const char* hat_type_name(HatType type);

/**
 * @brief Get LA state name string for display.
 */
const char* hat_la_state_name(uint8_t state);

/**
 * @brief Get LA stream stop reason name string for display.
 */
const char* hat_la_stop_reason_name(uint8_t reason);

#ifdef __cplusplus
}
#endif
