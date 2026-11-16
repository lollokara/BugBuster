#![allow(dead_code)]

// =============================================================================
// bbp.rs - BugBuster Binary Protocol codec (Rust)
//
// Pure data layer: COBS framing, CRC-16, message building/parsing.
// No I/O - used by transport implementations.
// =============================================================================

use serde::{Deserialize, Serialize};

// -----------------------------------------------------------------------------
// Protocol Constants
// -----------------------------------------------------------------------------

pub const PROTO_VERSION: u8 = 13;

pub const MAGIC: [u8; 4] = [0xBB, 0x42, 0x55, 0x47]; // 0xBB 'B' 'U' 'G'
pub const HANDSHAKE_RSP_LEN: usize = 14;

pub const MAX_PAYLOAD: usize = 1024;
pub const FRAME_DELIMITER: u8 = 0x00;
pub const HEADER_SIZE: usize = 4; // type(1) + seq(2) + cmd(1)
pub const CRC_SIZE: usize = 2;
pub const MIN_MSG_SIZE: usize = HEADER_SIZE + CRC_SIZE;

// -----------------------------------------------------------------------------
// Message Types
// -----------------------------------------------------------------------------

pub const MSG_CMD: u8 = 0x01;
pub const MSG_RSP: u8 = 0x02;
pub const MSG_EVT: u8 = 0x03;
pub const MSG_ERR: u8 = 0x04;

// -----------------------------------------------------------------------------
// Command IDs
// -----------------------------------------------------------------------------

// Generated from bbp.h by Firmware/tools/gen_error_tables.py - do not edit.
// BEGIN GENERATED: bbp-commands
pub const CMD_GET_STATUS: u8 = 0x01;
pub const CMD_GET_DEVICE_INFO: u8 = 0x02;
pub const CMD_GET_FAULTS: u8 = 0x03;
pub const CMD_GET_DIAGNOSTICS: u8 = 0x04;
pub const CMD_SELFTEST_STATUS: u8 = 0x05; // Get boot self-test result + cal status
pub const CMD_SELFTEST_MEASURE_SUPPLY: u8 = 0x06; // Measure a supply rail via U23
pub const CMD_SELFTEST_SUPPLY_VOLTAGES_CACHED: u8 = 0x07; // Get cached supply rail voltages
pub const CMD_SELFTEST_AUTO_CAL: u8 = 0x08; // Start automatic IDAC calibration
pub const CMD_SELFTEST_INT_SUPPLIES: u8 = 0x09; // Measure internal ADC supplies (AVDD, DVCC, etc.)
pub const CMD_SELFTEST_WORKER: u8 = 0x0B; // Enable/disable/query supply monitor worker
pub const CMD_WIFI_FORGET: u8 = 0x0A; // Erase saved STA credentials from NVS and disconnect
pub const CMD_MEM_STATUS: u8 = 0x0C; // Heap + per-task stack telemetry (see MEM_STATUS_SCHEMA)
pub const CMD_EFUSE_IMON_SET: u8 = 0x0D; // Select monitored e-fuse (0=off,1..4) via U23 IMON path
pub const CMD_EFUSE_IMON_GET: u8 = 0x0E; // Read monitored e-fuse current
pub const CMD_SET_CH_FUNC: u8 = 0x10;
pub const CMD_SET_DAC_CODE: u8 = 0x11;
pub const CMD_SET_DAC_VOLTAGE: u8 = 0x12;
pub const CMD_SET_DAC_CURRENT: u8 = 0x13;
pub const CMD_SET_ADC_CONFIG: u8 = 0x14;
pub const CMD_SET_DIN_CONFIG: u8 = 0x15;
pub const CMD_SET_DO_CONFIG: u8 = 0x16;
pub const CMD_SET_DO_STATE: u8 = 0x17;
pub const CMD_SET_VOUT_RANGE: u8 = 0x18;
pub const CMD_SET_ILIMIT: u8 = 0x19;
pub const CMD_SET_AVDD_SEL: u8 = 0x1A;
pub const CMD_GET_ADC_VALUE: u8 = 0x1B;
pub const CMD_GET_DAC_READBACK: u8 = 0x1C;
pub const CMD_SET_RTD_CONFIG: u8 = 0x1D;
pub const CMD_CLEAR_ALL_ALERTS: u8 = 0x20;
pub const CMD_CLEAR_CH_ALERT: u8 = 0x21;
pub const CMD_SET_ALERT_MASK: u8 = 0x22;
pub const CMD_SET_CH_ALERT_MASK: u8 = 0x23;
pub const CMD_SET_DIAG_CONFIG: u8 = 0x30;
pub const CMD_GET_GPIO_STATUS: u8 = 0x40;
pub const CMD_SET_GPIO_CONFIG: u8 = 0x41;
pub const CMD_SET_GPIO_VALUE: u8 = 0x42;
pub const CMD_ADC_LEDS_SET_MODE: u8 = 0x47; // 1-byte payload: 0=auto, 1=manual
pub const CMD_DIO_GET_ALL: u8 = 0x43; // Read all 12 IO states
pub const CMD_DIO_CONFIG: u8 = 0x44; // Configure IO direction (input/output/disabled)
pub const CMD_DIO_WRITE: u8 = 0x45; // Set output level (high/low)
pub const CMD_DIO_READ: u8 = 0x46; // Read single IO input level
pub const CMD_GET_UART_CONFIG: u8 = 0x50;
pub const CMD_SET_UART_CONFIG: u8 = 0x51;
pub const CMD_GET_UART_PINS: u8 = 0x52;
pub const CMD_START_ADC_STREAM: u8 = 0x60;
pub const CMD_STOP_ADC_STREAM: u8 = 0x61;
pub const CMD_START_SCOPE_STREAM: u8 = 0x62;
pub const CMD_STOP_SCOPE_STREAM: u8 = 0x63;
pub const CMD_START_ADC_DSP_STREAM: u8 = 0x64;
pub const CMD_STOP_ADC_DSP_STREAM: u8 = 0x65;
pub const CMD_MUX_SET_ALL: u8 = 0x90;
pub const CMD_MUX_GET_ALL: u8 = 0x91;
pub const CMD_MUX_SET_SWITCH: u8 = 0x92;
pub const CMD_RAIL_POWER_UP: u8 = 0x93; // Sequenced VADJ rail + e-fuse power-up (power/rail_power.h)
pub const CMD_IDAC_GET_STATUS: u8 = 0xA0; // Get all IDAC channel states
pub const CMD_IDAC_SET_CODE: u8 = 0xA1; // Set raw DAC code
pub const CMD_IDAC_SET_VOLTAGE: u8 = 0xA2; // Set target voltage
pub const CMD_IDAC_CALIBRATE: u8 = 0xA3; // Run auto-calibration (unused, UI-driven)
pub const CMD_IDAC_CAL_ADD_POINT: u8 = 0xA4; // Add a single calibration point
pub const CMD_IDAC_CAL_CLEAR: u8 = 0xA5; // Clear calibration for a channel
pub const CMD_IDAC_CAL_SAVE: u8 = 0xA6; // Save calibration to NVS
pub const CMD_IO_CLAIM: u8 = 0xA7; // Claim one or more IO/CH slots
pub const CMD_IO_RELEASE: u8 = 0xA8; // Release previously claimed slots
pub const CMD_IO_OWNER_STATUS: u8 = 0xA9; // Query ownership table (all 16 slots)
pub const CMD_IO_FORCE_RELEASE: u8 = 0xAA; // Admin: force-release any slot
pub const CMD_PCA_GET_STATUS: u8 = 0xB0; // Get all PCA9535 state
pub const CMD_PCA_SET_CONTROL: u8 = 0xB1; // Set named control output
pub const CMD_PCA_SET_PORT: u8 = 0xB2; // Set raw port value
pub const CMD_PCA_SET_FAULT_CFG: u8 = 0xB3; // Configure fault auto-disable + logging
pub const CMD_PCA_GET_FAULT_LOG: u8 = 0xB4; // Get recent PCA fault events
pub const CMD_EXT_I2C_SETUP: u8 = 0xB8; // Configure external I2C peripheral pins/frequency
pub const CMD_EXT_I2C_SCAN: u8 = 0xB9; // Scan external I2C bus
pub const CMD_EXT_I2C_WRITE: u8 = 0xBA; // Write bytes to external I2C device
pub const CMD_EXT_I2C_READ: u8 = 0xBB; // Read bytes from external I2C device
pub const CMD_EXT_I2C_WRITE_READ: u8 = 0xBC; // Register-style write then repeated-start read
pub const CMD_EXT_SPI_SETUP: u8 = 0xBD; // Configure external SPI peripheral pins/frequency
pub const CMD_EXT_SPI_TRANSFER: u8 = 0xBE; // Full-duplex SPI transfer
pub const CMD_START_WAVEGEN: u8 = 0xD0; // Start waveform generation
pub const CMD_STOP_WAVEGEN: u8 = 0xD1; // Stop waveform generation
pub const CMD_HAT_GET_STATUS: u8 = 0xC5; // Get HAT state (detect, type, pin config, power)
pub const CMD_HAT_SET_PIN: u8 = 0xC6; // Set single EXP_EXT pin function
pub const CMD_HAT_SET_ALL_PINS: u8 = 0xC7; // Set all 4 EXP_EXT pin functions
pub const CMD_HAT_RESET: u8 = 0xC8; // Reset HAT to default
pub const CMD_HAT_DETECT: u8 = 0xC9; // Re-run HAT detection
pub const CMD_HAT_SET_POWER: u8 = 0xCA; // Enable/disable connector (A/B)
pub const CMD_HAT_GET_POWER: u8 = 0xCB; // Get power status (both connectors)
pub const CMD_HAT_SET_IO_VOLTAGE: u8 = 0xCC; // Set I/O voltage (mV)
pub const CMD_HAT_SETUP_SWD: u8 = 0xCD; // One-call SWD quick-setup
pub const CMD_HAT_GET_CAPS: u8 = 0xC3;
pub const CMD_HAT_GET_RAIL_STATUS: u8 = 0xC4;
pub const CMD_HAT_SET_RAIL_VOLTAGE: u8 = 0xB5;
pub const CMD_HAT_SET_RAIL_ENABLE: u8 = 0xD2;
pub const CMD_HAT_SET_LED_STATE: u8 = 0xD3;
pub const CMD_HAT_CALIBRATE_START: u8 = 0xAB;
pub const CMD_HAT_CALIBRATE_STATUS: u8 = 0xAC;
pub const CMD_HAT_CALIBRATE_IMPORT: u8 = 0xAD;
pub const CMD_HAT_SET_IO_BANK: u8 = 0xAE;
pub const CMD_HAT_SET_LEVEL_SHIFT: u8 = 0xAF;
pub const CMD_HAT_LA_CONFIG: u8 = 0xCF; // Configure LA capture
pub const CMD_HAT_LA_ARM: u8 = 0xD5; // Arm trigger / start capture
pub const CMD_HAT_LA_FORCE: u8 = 0xD6; // Force trigger
pub const CMD_HAT_LA_STATUS: u8 = 0xD7; // Get capture status
pub const CMD_HAT_LA_READ: u8 = 0xD8; // Read captured data chunk
pub const CMD_HAT_LA_SET_ROUTE: u8 = 0xD4;
pub const CMD_HAT_LA_STOP: u8 = 0xD9; // Stop capture
pub const CMD_HAT_LA_TRIGGER: u8 = 0xDA; // Set trigger condition
pub const CMD_HAT_DETECT_TARGET: u8 = 0xDB; // Actively probe SWD DPIDR (target detect)
pub const CMD_USBPD_GET_STATUS: u8 = 0xC0; // Get USB PD contract status
pub const CMD_USBPD_SELECT_PDO: u8 = 0xC1; // Select PDO voltage
pub const CMD_USBPD_GO: u8 = 0xC2; // Trigger re-negotiation
pub const CMD_SET_LSHIFT_OE: u8 = 0xE0; // Set level shifter output enable
pub const CMD_WIFI_GET_STATUS: u8 = 0xE1; // Get WiFi status
pub const CMD_WIFI_CONNECT: u8 = 0xE2; // Connect to WiFi network
pub const CMD_WIFI_SCAN: u8 = 0xE4; // Scan for WiFi networks
pub const CMD_WIFI_SET_AP_PASSWORD: u8 = 0xEF; // Set SoftAP password (persist to NVS, apply live)
pub const CMD_HAT_LA_LOG_ENABLE: u8 = 0xEB; // Enable/disable RP2040 log relay
pub const CMD_HAT_LA_USB_RESET: u8 = 0xED; // Reinitialize vendor bulk endpoint
pub const CMD_HAT_LA_STREAM_START: u8 = 0xEE; // Start LA streaming over vendor bulk
pub const CMD_QS_LIST: u8 = 0xF0; // List quick setup slots
pub const CMD_QS_GET: u8 = 0xF1; // Get quick setup JSON
pub const CMD_QS_SAVE: u8 = 0xF2; // Save current state to slot
pub const CMD_QS_APPLY: u8 = 0xF3; // Apply slot
pub const CMD_QS_DELETE: u8 = 0xF4; // Delete slot
pub const CMD_SCRIPT_EVAL: u8 = 0xF5; // Submit Python source for eval (max 32 KB)
pub const CMD_SCRIPT_STATUS: u8 = 0xF6; // Get script engine status
pub const CMD_SCRIPT_LOGS: u8 = 0xF7; // Drain script log ring (up to 1020 bytes)
pub const CMD_SCRIPT_STOP: u8 = 0xF8; // Request cooperative stop
pub const CMD_SCRIPT_UPLOAD: u8 = 0xF9; // Upload script file to SPIFFS
pub const CMD_SCRIPT_LIST: u8 = 0xFA; // List stored script files
pub const CMD_SCRIPT_RUN_FILE: u8 = 0xFB; // Run a stored script file
pub const CMD_SCRIPT_DELETE: u8 = 0xFC; // Delete a stored script file
pub const CMD_SCRIPT_AUTORUN: u8 = 0xFD; // Autorun control (sub-byte multiplex)
pub const CMD_SET_SPI_CLOCK: u8 = 0xE3; // Set SPI clock speed (Hz, u32)
pub const CMD_DAQ_CONFIG: u8 = 0xB6; // sub-op multiplexed (see BBP_DAQ_CFG_*)
pub const CMD_DAQ_CAL: u8 = 0xB7; // sub-op multiplexed (see BBP_DAQ_CAL_*)
pub const CMD_DAQ_MEASURE: u8 = 0xBF; // no payload -> s3link_daq_status_t
pub const CMD_DAQ_TRIG: u8 = 0xCE; // sub-op multiplexed (see BBP_DAQ_TRIG_*)
pub const CMD_DEVICE_RESET: u8 = 0x70;
pub const CMD_REG_READ: u8 = 0x71;
pub const CMD_REG_WRITE: u8 = 0x72;
pub const CMD_SET_WATCHDOG: u8 = 0x73; // Enable/disable AD74416H watchdog timer
pub const CMD_GET_ADMIN_TOKEN: u8 = 0x74; // Retrieve derived admin token (USB only)
pub const CMD_EXT_JOB_SUBMIT: u8 = 0x75; // Queue deferred external I2C/SPI operation
pub const CMD_EXT_JOB_GET: u8 = 0x76; // Poll deferred external I2C/SPI operation
pub const CMD_OTA: u8 = 0x77; // USB OTA info/upload/rollback control
pub const CMD_STANDBY: u8 = 0x78;
pub const CMD_PING: u8 = 0xFE;
pub const CMD_DISCONNECT: u8 = 0xFF;
// END GENERATED: bbp-commands

// HTTP-only: drain buffered RP2040 log lines from the ESP32 ring buffer.
// Same byte as EVT_LA_LOG (0xEC) but safe — commands are host→device,
// events are device→host; they never collide in the protocol state machine.
pub const CMD_HAT_LA_LOG_GET: u8 = 0xEC;

// CMD_DAQ_TRIG sub-ops (payload[0]); BBP_DAQ_TRIG_* in bbp.h.
pub const DAQ_TRIG_SET_IO: u8 = 0x00;
pub const DAQ_TRIG_GET_IO: u8 = 0x01;
pub const DAQ_TRIG_SET_LOGIC: u8 = 0x02;
pub const DAQ_TRIG_ARM: u8 = 0x03;
pub const DAQ_TRIG_STATUS: u8 = 0x04;
pub const DAQ_TRIG_GET_ALL: u8 = 0x05;

// -----------------------------------------------------------------------------
// Event IDs
// -----------------------------------------------------------------------------

// Generated from bbp.h by Firmware/tools/gen_error_tables.py - do not edit.
// BEGIN GENERATED: bbp-events
pub const EVT_ADC_DATA: u8 = 0x80;
pub const EVT_SCOPE_DATA: u8 = 0x81;
pub const EVT_ALERT: u8 = 0x82;
pub const EVT_DIN: u8 = 0x83;
pub const EVT_PCA_FAULT: u8 = 0x84; // PCA9535 fault event (e-fuse trip, PG change)
pub const EVT_LA_DONE: u8 = 0x85; // Logic analyzer capture complete
pub const EVT_IO_PREEMPTED: u8 = 0x86; // IO slot preempted by selftest/internal
pub const EVT_IO_OWNER_REJECT: u8 = 0x87; // IO claim rejected: (cmd_id u8, slot u8, owner_kind u8)
pub const EVT_ADC_DSP: u8 = 0x88; // DSP stream window (stats + FFT peaks + spikes)
pub const EVT_LA_LOG: u8 = 0xEC; // RP2040 log message relay
// END GENERATED: bbp-events
pub const EVT_DISCONNECT: u8 = 0xFE; // Synthetic: USB reader thread detected serial error

// -----------------------------------------------------------------------------
// Error Codes
// -----------------------------------------------------------------------------

// Generated from bbp.h by Firmware/tools/gen_error_tables.py - do not edit.
// BEGIN GENERATED: bbp-errors
pub const ERR_INVALID_CMD: u8 = 0x01;
pub const ERR_INVALID_CH: u8 = 0x02;
pub const ERR_INVALID_PARAM: u8 = 0x03;
pub const ERR_SPI_FAIL: u8 = 0x04;
pub const ERR_QUEUE_FULL: u8 = 0x05;
pub const ERR_BUSY: u8 = 0x06;
pub const ERR_INVALID_STATE: u8 = 0x07;
pub const ERR_CRC_FAIL: u8 = 0x08;
pub const ERR_FRAME_TOO_LARGE: u8 = 0x09;
pub const ERR_STREAM_ACTIVE: u8 = 0x0A;
pub const ERR_TIMEOUT: u8 = 0x11;
pub const ERR_IO_OWNERSHIP_REQUIRED: u8 = 0x12;
pub const ERR_ADGS_ROUTE_REJECTED: u8 = 0x13;
pub const ERR_UNSUPPORTED_HAT: u8 = 0x14;
// END GENERATED: bbp-errors

/// Convert error code to human-readable string. Handles unknown future codes
/// gracefully by returning "Unknown error 0xXX" instead of panicking.
pub fn error_to_string(code: u8) -> String {
    match code {
        // BEGIN GENERATED: bbp-errors
        ERR_INVALID_CMD => "The command is not recognized by the device".to_string(),
        ERR_INVALID_CH => "Channel number is out of range".to_string(),
        ERR_INVALID_PARAM => "One or more parameters are invalid".to_string(),
        ERR_SPI_FAIL => "SPI communication with the AD74416H failed".to_string(),
        ERR_QUEUE_FULL => "Device command queue is full - retry after a short delay".to_string(),
        ERR_BUSY => "Device is busy processing another operation".to_string(),
        ERR_INVALID_STATE => "Operation cannot be performed in the current device state".to_string(),
        ERR_CRC_FAIL => "Payload CRC check failed".to_string(),
        ERR_FRAME_TOO_LARGE => "Command or response payload exceeds the maximum frame size".to_string(),
        ERR_STREAM_ACTIVE => "Cannot configure while streaming is active".to_string(),
        ERR_TIMEOUT => "Device operation timed out or no response received".to_string(),
        ERR_IO_OWNERSHIP_REQUIRED => "IO slot is owned by another session".to_string(),
        ERR_ADGS_ROUTE_REJECTED => "MUX route rejected by the U17-S3 / U23 self-test interlock".to_string(),
        ERR_UNSUPPORTED_HAT => "Command requires a different HAT type, or no HAT is attached".to_string(),
        // END GENERATED: bbp-errors
        _ => format!("Unknown error 0x{:02X}", code),
    }
}

// -----------------------------------------------------------------------------
// COBS Codec
// -----------------------------------------------------------------------------

/// COBS-encode a payload. Returns the encoded bytes (without delimiter).
pub fn cobs_encode(input: &[u8]) -> Vec<u8> {
    let mut output = Vec::with_capacity(input.len() + input.len() / 254 + 2);
    let mut code_idx = 0;
    let mut code: u8 = 1;

    output.push(0); // Placeholder for first code byte

    for &byte in input {
        if byte == 0x00 {
            output[code_idx] = code;
            code_idx = output.len();
            output.push(0); // Placeholder for next code byte
            code = 1;
        } else {
            output.push(byte);
            code += 1;
            if code == 0xFF {
                output[code_idx] = code;
                code_idx = output.len();
                output.push(0);
                code = 1;
            }
        }
    }
    output[code_idx] = code;
    output
}

/// COBS-decode a frame (without the trailing 0x00 delimiter).
pub fn cobs_decode(input: &[u8]) -> Option<Vec<u8>> {
    let mut output = Vec::with_capacity(input.len());
    let mut read_idx = 0;

    while read_idx < input.len() {
        let code = input[read_idx];
        read_idx += 1;
        if code == 0 {
            return None; // Invalid: 0x00 in COBS stream
        }
        for _ in 1..code {
            if read_idx >= input.len() {
                break;
            }
            output.push(input[read_idx]);
            read_idx += 1;
        }
        // Add implicit zero delimiter between groups, but NOT after the last group
        if code != 0xFF && read_idx < input.len() {
            output.push(0x00);
        }
    }
    Some(output)
}

// -----------------------------------------------------------------------------
// CRC-16/CCITT
// -----------------------------------------------------------------------------

pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &byte in data {
        crc ^= (byte as u16) << 8;
        for _ in 0..8 {
            if crc & 0x8000 != 0 {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    crc
}

// -----------------------------------------------------------------------------
// Parsed Message
// -----------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct Message {
    pub msg_type: u8,
    pub seq: u16,
    pub cmd_id: u8,
    pub payload: Vec<u8>,
}

impl Message {
    /// Parse a raw (COBS-decoded) message. Validates CRC.
    pub fn parse(data: &[u8]) -> Option<Self> {
        if data.len() < MIN_MSG_SIZE {
            return None;
        }

        // Verify CRC
        let crc_offset = data.len() - 2;
        let rx_crc = u16::from_le_bytes([data[crc_offset], data[crc_offset + 1]]);
        let calc_crc = crc16(&data[..crc_offset]);
        if rx_crc != calc_crc {
            log::warn!(
                "CRC mismatch: rx=0x{:04X} calc=0x{:04X}, len={}, data={:02X?}",
                rx_crc,
                calc_crc,
                data.len(),
                &data[..std::cmp::min(data.len(), 32)]
            );
            return None;
        }

        Some(Self {
            msg_type: data[0],
            seq: u16::from_le_bytes([data[1], data[2]]),
            cmd_id: data[3],
            payload: data[HEADER_SIZE..crc_offset].to_vec(),
        })
    }

    /// Build a raw message (before COBS encoding).
    pub fn build(msg_type: u8, seq: u16, cmd_id: u8, payload: &[u8]) -> Vec<u8> {
        let mut msg = Vec::with_capacity(HEADER_SIZE + payload.len() + CRC_SIZE);
        msg.push(msg_type);
        msg.extend_from_slice(&seq.to_le_bytes());
        msg.push(cmd_id);
        msg.extend_from_slice(payload);
        let crc = crc16(&msg);
        msg.extend_from_slice(&crc.to_le_bytes());
        msg
    }

    /// Build a command message, COBS-encode it, and append the delimiter.
    pub fn build_frame(seq: u16, cmd_id: u8, payload: &[u8]) -> Vec<u8> {
        let raw = Self::build(MSG_CMD, seq, cmd_id, payload);
        let mut encoded = cobs_encode(&raw);
        encoded.push(FRAME_DELIMITER);
        encoded
    }

    /// Check if this is a response message.
    pub fn is_response(&self) -> bool {
        self.msg_type == MSG_RSP
    }

    /// Check if this is an error message.
    pub fn is_error(&self) -> bool {
        self.msg_type == MSG_ERR
    }

    /// Check if this is an event message.
    pub fn is_event(&self) -> bool {
        self.msg_type == MSG_EVT
    }

    /// Get error code from an ERR message payload.
    pub fn error_code(&self) -> Option<u8> {
        if self.is_error() && !self.payload.is_empty() {
            Some(self.payload[0])
        } else {
            None
        }
    }
}

// -----------------------------------------------------------------------------
// Handshake
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HandshakeInfo {
    pub proto_version: u8,
    pub fw_major: u8,
    pub fw_minor: u8,
    pub fw_patch: u8,
    pub mac_address: String,
}

impl HandshakeInfo {
    /// Parse a 14-byte handshake response.
    pub fn parse(data: &[u8]) -> Option<Self> {
        if data.len() < HANDSHAKE_RSP_LEN {
            return None;
        }
        // Verify magic
        if data[0..4] != MAGIC {
            return None;
        }

        // MAC address is at bytes 8..14
        let mac = format!(
            "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
            data[8], data[9], data[10], data[11], data[12], data[13]
        );

        Some(Self {
            proto_version: data[4],
            fw_major: data[5],
            fw_minor: data[6],
            fw_patch: data[7],
            mac_address: mac,
        })
    }
}

// -----------------------------------------------------------------------------
// Frame Accumulator
// Collects bytes and yields complete COBS frames on delimiter.
// -----------------------------------------------------------------------------

pub struct FrameAccumulator {
    buf: Vec<u8>,
}

impl FrameAccumulator {
    pub fn new() -> Self {
        Self {
            buf: Vec::with_capacity(MAX_PAYLOAD + 16),
        }
    }

    /// Feed bytes and return decoded messages.
    pub fn feed(&mut self, data: &[u8]) -> Vec<Message> {
        let mut messages = Vec::new();

        for &byte in data {
            if byte == FRAME_DELIMITER {
                if !self.buf.is_empty() {
                    log::debug!(
                        "COBS frame ({} encoded bytes): {:02X?}",
                        self.buf.len(),
                        &self.buf[..std::cmp::min(self.buf.len(), 32)]
                    );
                    if let Some(decoded) = cobs_decode(&self.buf) {
                        log::debug!(
                            "Decoded ({} bytes): {:02X?}",
                            decoded.len(),
                            &decoded[..std::cmp::min(decoded.len(), 32)]
                        );
                        if let Some(msg) = Message::parse(&decoded) {
                            messages.push(msg);
                        }
                    }
                    self.buf.clear();
                }
            } else {
                if self.buf.len() < MAX_PAYLOAD + 16 {
                    self.buf.push(byte);
                } else {
                    // Frame too large, discard
                    log::warn!("BBP frame too large ({} bytes), discarding", self.buf.len());
                    self.buf.clear();
                }
            }
        }

        messages
    }

    /// Reset the accumulator (e.g. on reconnect).
    pub fn reset(&mut self) {
        self.buf.clear();
    }
}

// -----------------------------------------------------------------------------
// Payload helpers (little-endian)
// -----------------------------------------------------------------------------

pub struct PayloadWriter {
    pub buf: Vec<u8>,
}

impl PayloadWriter {
    pub fn new() -> Self {
        Self {
            buf: Vec::with_capacity(32),
        }
    }

    pub fn put_u8(&mut self, v: u8) {
        self.buf.push(v);
    }

    pub fn put_u16(&mut self, v: u16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn put_u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn put_f32(&mut self, v: f32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn put_bool(&mut self, v: bool) {
        self.buf.push(if v { 1 } else { 0 });
    }
}

pub struct PayloadReader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> PayloadReader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    pub fn remaining(&self) -> usize {
        self.data.len().saturating_sub(self.pos)
    }

    pub fn pos(&self) -> usize {
        self.pos
    }

    pub fn skip(&mut self, n: usize) {
        self.pos = (self.pos + n).min(self.data.len());
    }

    pub fn get_u8(&mut self) -> Option<u8> {
        if self.pos < self.data.len() {
            let v = self.data[self.pos];
            self.pos += 1;
            Some(v)
        } else {
            None
        }
    }

    pub fn get_u16(&mut self) -> Option<u16> {
        if self.pos + 2 <= self.data.len() {
            let v = u16::from_le_bytes([self.data[self.pos], self.data[self.pos + 1]]);
            self.pos += 2;
            Some(v)
        } else {
            None
        }
    }

    pub fn get_u24(&mut self) -> Option<u32> {
        if self.pos + 3 <= self.data.len() {
            let v = self.data[self.pos] as u32
                | ((self.data[self.pos + 1] as u32) << 8)
                | ((self.data[self.pos + 2] as u32) << 16);
            self.pos += 3;
            Some(v)
        } else {
            None
        }
    }

    pub fn get_u32(&mut self) -> Option<u32> {
        if self.pos + 4 <= self.data.len() {
            let v = u32::from_le_bytes([
                self.data[self.pos],
                self.data[self.pos + 1],
                self.data[self.pos + 2],
                self.data[self.pos + 3],
            ]);
            self.pos += 4;
            Some(v)
        } else {
            None
        }
    }

    pub fn get_f32(&mut self) -> Option<f32> {
        self.get_u32().map(f32::from_bits)
    }

    pub fn get_bool(&mut self) -> Option<bool> {
        self.get_u8().map(|v| v != 0)
    }
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cobs_roundtrip() {
        let data = vec![0x01, 0x00, 0x02, 0x03, 0x00, 0x04];
        let encoded = cobs_encode(&data);
        let decoded = cobs_decode(&encoded).unwrap();
        assert_eq!(data, decoded);
    }

    #[test]
    fn test_cobs_no_zeros() {
        let data = vec![0x11, 0x22, 0x33];
        let encoded = cobs_encode(&data);
        assert!(!encoded.iter().any(|&b| b == 0x00));
        let decoded = cobs_decode(&encoded).unwrap();
        assert_eq!(data, decoded, "COBS roundtrip failed for no-zero data");
    }

    #[test]
    fn test_cobs_all_zeros() {
        let data = vec![0x00, 0x00, 0x00];
        let encoded = cobs_encode(&data);
        assert!(!encoded.iter().any(|&b| b == 0x00));
        let decoded = cobs_decode(&encoded).unwrap();
        assert_eq!(data, decoded);
    }

    #[test]
    fn test_cobs_empty() {
        let data: Vec<u8> = vec![];
        let encoded = cobs_encode(&data);
        let decoded = cobs_decode(&encoded).unwrap();
        assert_eq!(data, decoded);
    }

    #[test]
    fn test_crc16() {
        let data = [0x01, 0x00, 0x00, 0x01]; // CMD, seq=0, cmd=GET_STATUS
        let crc = crc16(&data);
        assert_ne!(crc, 0); // Just check it produces something
                            // Same data should produce same CRC
        assert_eq!(crc, crc16(&data));
    }

    #[test]
    fn test_message_roundtrip() {
        let raw = Message::build(MSG_CMD, 42, CMD_GET_STATUS, &[]);
        let parsed = Message::parse(&raw).unwrap();
        assert_eq!(parsed.msg_type, MSG_CMD);
        assert_eq!(parsed.seq, 42);
        assert_eq!(parsed.cmd_id, CMD_GET_STATUS);
        assert!(parsed.payload.is_empty());
    }

    #[test]
    fn test_message_with_payload() {
        let payload = vec![0x00, 0x03]; // ch=0, func=3 (VIN)
        let raw = Message::build(MSG_CMD, 1, CMD_SET_CH_FUNC, &payload);
        let parsed = Message::parse(&raw).unwrap();
        assert_eq!(parsed.payload, payload);
    }

    #[test]
    fn test_frame_accumulator() {
        let frame1 = Message::build_frame(1, CMD_GET_STATUS, &[]);
        let frame2 = Message::build_frame(2, CMD_PING, &42u32.to_le_bytes());

        let mut acc = FrameAccumulator::new();

        // Feed both frames concatenated
        let mut data = frame1;
        data.extend_from_slice(&frame2);
        let msgs = acc.feed(&data);

        assert_eq!(msgs.len(), 2);
        assert_eq!(msgs[0].cmd_id, CMD_GET_STATUS);
        assert_eq!(msgs[1].cmd_id, CMD_PING);
    }

    #[test]
    fn test_handshake_parse() {
        let data = [
            0xBB, 0x42, 0x55, 0x47, 0x05, 0x01, 0x02, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        ];
        let info = HandshakeInfo::parse(&data).unwrap();
        assert_eq!(info.proto_version, 5);
        assert_eq!(info.fw_major, 1);
        assert_eq!(info.fw_minor, 2);
        assert_eq!(info.fw_patch, 0);
        assert_eq!(info.mac_address, "AA:BB:CC:DD:EE:FF");
    }

    #[test]
    fn test_handshake_bad_magic() {
        let data = [0xBB, 0x42, 0x55, 0x00, 0x01, 0x01, 0x00, 0x00];
        assert!(HandshakeInfo::parse(&data).is_none());
    }

    #[test]
    fn test_payload_reader_writer() {
        let mut w = PayloadWriter::new();
        w.put_u8(42);
        w.put_u16(1000);
        w.put_f32(3.14);
        w.put_bool(true);

        let mut r = PayloadReader::new(&w.buf);
        assert_eq!(r.get_u8(), Some(42));
        assert_eq!(r.get_u16(), Some(1000));
        let f = r.get_f32().unwrap();
        assert!((f - 3.14).abs() < 0.001);
        assert_eq!(r.get_bool(), Some(true));
        assert_eq!(r.remaining(), 0);
    }

    // -------------------------------------------------------------------------
    // PayloadWriter/Reader round-trip with all types
    // -------------------------------------------------------------------------

    #[test]
    fn test_payload_roundtrip_all_types() {
        let mut w = PayloadWriter::new();
        w.put_u8(0xFF);
        w.put_u16(0xBEEF);
        // u24 is written as 3 LE bytes manually (no put_u24 on writer)
        w.buf.extend_from_slice(&[0x56, 0x34, 0x12]); // 0x123456
        w.put_u32(0xDEADBEEF);
        w.put_f32(-1.5);
        w.put_bool(false);
        w.put_bool(true);

        let mut r = PayloadReader::new(&w.buf);
        assert_eq!(r.get_u8(), Some(0xFF));
        assert_eq!(r.get_u16(), Some(0xBEEF));
        assert_eq!(r.get_u24(), Some(0x123456));
        assert_eq!(r.get_u32(), Some(0xDEADBEEF));
        assert_eq!(r.get_f32(), Some(-1.5));
        assert_eq!(r.get_bool(), Some(false));
        assert_eq!(r.get_bool(), Some(true));
        assert_eq!(r.remaining(), 0);
    }

    #[test]
    fn test_payload_reader_exhaustion() {
        // Reading past the end should return None, not panic
        let data = [0x42];
        let mut r = PayloadReader::new(&data);
        assert_eq!(r.get_u8(), Some(0x42));
        assert_eq!(r.get_u8(), None);
        assert_eq!(r.get_u16(), None);
        assert_eq!(r.get_u24(), None);
        assert_eq!(r.get_u32(), None);
        assert_eq!(r.get_f32(), None);
        assert_eq!(r.get_bool(), None);
    }

    // -------------------------------------------------------------------------
    // COBS encode/decode with embedded zeros
    // -------------------------------------------------------------------------

    #[test]
    fn test_cobs_embedded_zeros() {
        let data = vec![0x00, 0x11, 0x00, 0x00, 0x22, 0x00];
        let encoded = cobs_encode(&data);
        // Encoded stream must never contain 0x00
        assert!(
            !encoded.iter().any(|&b| b == 0x00),
            "COBS encoded data must not contain zero bytes"
        );
        let decoded = cobs_decode(&encoded).unwrap();
        assert_eq!(data, decoded);
    }

    #[test]
    fn test_cobs_single_zero() {
        let data = vec![0x00];
        let encoded = cobs_encode(&data);
        assert!(!encoded.iter().any(|&b| b == 0x00));
        let decoded = cobs_decode(&encoded).unwrap();
        assert_eq!(data, decoded);
    }

    #[test]
    fn test_cobs_long_non_zero_run() {
        // Test a run longer than 254 bytes (exercises the 0xFF block boundary)
        let data: Vec<u8> = (1..=255).cycle().take(300).collect();
        let encoded = cobs_encode(&data);
        assert!(!encoded.iter().any(|&b| b == 0x00));
        let decoded = cobs_decode(&encoded).unwrap();
        assert_eq!(data, decoded);
    }

    #[test]
    fn test_cobs_decode_invalid_zero_in_stream() {
        // A zero byte inside the COBS stream is invalid
        let invalid = vec![0x02, 0x11, 0x00, 0x03, 0x22, 0x33];
        assert!(cobs_decode(&invalid).is_none());
    }

    // -------------------------------------------------------------------------
    // Message parse with truncated data
    // -------------------------------------------------------------------------

    #[test]
    fn test_message_parse_truncated_empty() {
        assert!(
            Message::parse(&[]).is_none(),
            "Empty data should return None"
        );
    }

    #[test]
    fn test_message_parse_truncated_too_short() {
        // Less than MIN_MSG_SIZE (6 bytes: header 4 + CRC 2)
        assert!(Message::parse(&[0x01, 0x00, 0x00]).is_none());
        assert!(Message::parse(&[0x01, 0x00, 0x00, 0x01, 0x00]).is_none());
    }

    #[test]
    fn test_message_parse_bad_crc() {
        // Build a valid message then corrupt the CRC
        let mut raw = Message::build(MSG_CMD, 1, CMD_PING, &[]);
        let len = raw.len();
        raw[len - 1] ^= 0xFF; // Flip bits in CRC
        assert!(
            Message::parse(&raw).is_none(),
            "Corrupted CRC should return None"
        );
    }

    #[test]
    fn test_message_parse_min_valid() {
        // A valid message with no payload (just header + CRC)
        let raw = Message::build(MSG_CMD, 0, CMD_PING, &[]);
        assert_eq!(raw.len(), MIN_MSG_SIZE);
        let msg = Message::parse(&raw).unwrap();
        assert_eq!(msg.msg_type, MSG_CMD);
        assert_eq!(msg.seq, 0);
        assert_eq!(msg.cmd_id, CMD_PING);
        assert!(msg.payload.is_empty());
    }

    // -------------------------------------------------------------------------
    // Frame accumulator with partial frames
    // -------------------------------------------------------------------------

    #[test]
    fn test_frame_accumulator_partial_then_complete() {
        let frame = Message::build_frame(1, CMD_GET_STATUS, &[0xAA, 0xBB]);
        let mid = frame.len() / 2;

        let mut acc = FrameAccumulator::new();

        // Feed first half -- no complete frames yet
        let msgs = acc.feed(&frame[..mid]);
        assert!(msgs.is_empty(), "Partial frame should not yield messages");

        // Feed second half -- now we should get the message
        let msgs = acc.feed(&frame[mid..]);
        assert_eq!(msgs.len(), 1);
        assert_eq!(msgs[0].cmd_id, CMD_GET_STATUS);
        assert_eq!(msgs[0].payload, vec![0xAA, 0xBB]);
    }

    #[test]
    fn test_frame_accumulator_multiple_frames_byte_by_byte() {
        let frame1 = Message::build_frame(10, CMD_PING, &[]);
        let frame2 = Message::build_frame(20, CMD_GET_STATUS, &[0x01]);

        let mut combined = frame1;
        combined.extend_from_slice(&frame2);

        let mut acc = FrameAccumulator::new();
        let mut all_msgs = Vec::new();

        // Feed one byte at a time
        for &byte in &combined {
            let msgs = acc.feed(&[byte]);
            all_msgs.extend(msgs);
        }

        assert_eq!(all_msgs.len(), 2);
        assert_eq!(all_msgs[0].cmd_id, CMD_PING);
        assert_eq!(all_msgs[0].seq, 10);
        assert_eq!(all_msgs[1].cmd_id, CMD_GET_STATUS);
        assert_eq!(all_msgs[1].seq, 20);
    }

    #[test]
    fn test_frame_accumulator_empty_between_delimiters() {
        // Consecutive delimiters should not produce messages
        let mut acc = FrameAccumulator::new();
        let msgs = acc.feed(&[0x00, 0x00, 0x00]);
        assert!(msgs.is_empty());
    }

    #[test]
    fn test_frame_accumulator_reset() {
        let frame = Message::build_frame(1, CMD_PING, &[]);
        let mid = frame.len() / 2;

        let mut acc = FrameAccumulator::new();
        acc.feed(&frame[..mid]); // partial
        acc.reset();

        // After reset, the partial data is gone; feed the rest should not yield a message
        let msgs = acc.feed(&frame[mid..]);
        assert!(
            msgs.is_empty(),
            "Reset should discard buffered partial data"
        );
    }

    // -------------------------------------------------------------------------
    // CRC-16 known test vector
    // -------------------------------------------------------------------------

    #[test]
    fn test_crc16_known_vector() {
        // CRC-16/CCITT-FALSE for "123456789" is 0x29B1
        let data = b"123456789";
        let crc = crc16(data);
        assert_eq!(
            crc, 0x29B1,
            "CRC-16/CCITT-FALSE of '123456789' should be 0x29B1, got 0x{:04X}",
            crc
        );
    }

    #[test]
    fn test_crc16_empty() {
        // CRC of empty data with init 0xFFFF should remain 0xFFFF
        assert_eq!(crc16(&[]), 0xFFFF);
    }

    #[test]
    fn test_crc16_single_byte() {
        // Deterministic: same input always gives same output
        let a = crc16(&[0x00]);
        let b = crc16(&[0x00]);
        assert_eq!(a, b);
        // Different input gives different output
        let c = crc16(&[0x01]);
        assert_ne!(a, c);
    }
}
