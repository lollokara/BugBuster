import Foundation

// MARK: - Core Status Models

public struct USBPDSourcePDO: Codable, Equatable, Identifiable {
    public var id: String { voltage }
    public let voltage: String
    public let detected: Bool
    public let maxCurrentA: Double
    public let maxPowerW: Double
}

public struct USBPDStatus: Codable, Equatable {
    public let present: Bool
    public let attached: Bool
    public let cc: String
    public let voltageV: Double
    public let currentA: Double
    public let powerW: Double
    public let pdResponse: Int
    public let sourcePdos: [USBPDSourcePDO]
    public let selectedPdo: Int
}

public struct ChannelState: Codable, Identifiable, Equatable {
    public let id: Int
    public let function: String
    public let functionCode: Int
    public let adcRaw: Int
    public let adcValue: Double
    public let adcRange: Int
    public let adcRate: Int
    public let adcMux: Int
    public let dacCode: Int
    public let dacValue: Double
    public let dinState: Bool
    public let dinCounter: Int
    public let doState: Bool
    public let alert: Int
    public let alertMask: Int
    public let rtdExcitationUa: Int?

    enum CodingKeys: String, CodingKey {
        case id
        case function
        case functionCode = "function_code"
        case adcRaw = "adc_raw"
        case adcValue = "adc_value"
        case adcRange = "adc_range"
        case adcRate = "adc_rate"
        case adcMux = "adc_mux"
        case dacCode = "dac_code"
        case dacValue = "dac_value"
        case dinState = "din_state"
        case dinCounter = "din_counter"
        case doState = "do_state"
        case alert = "channel_alert"
        case alertMask = "channel_alert_mask"
        case rtdExcitationUa = "rtd_excitation_ua"
    }
}

public struct ChannelAdcResponse: Codable, Equatable {
    public let id: Int
    public let adcRaw: Int
    public let adcValue: Double
    public let adcRange: Int
    public let adcRate: Int
    public let adcMux: Int
}

public struct DiagnosticSlot: Codable, Equatable {
    public let source: Int
    public let rawCode: Int
    public let value: Double
}

public struct DeviceStatus: Codable, Equatable {
    public let spiOk: Bool
    public let i2cOk: Bool
    public let muxOk: Bool
    public let dieTemp: Double
    public let alertStatus: Int
    public let alertMask: Int
    public let supplyAlertStatus: Int
    public let supplyAlertMask: Int
    public let liveStatus: Int
    public let channels: [ChannelState]
    public let diagnostics: [DiagnosticSlot]
    public let muxStates: [Int]
    public let freeHeap: Double
    public let minFreeHeap: Double
    public let uptimeMs: Double
    
    enum CodingKeys: String, CodingKey {
        case spiOk = "spi_ok"
        case i2cOk = "i2c_ok"
        case muxOk = "mux_ok"
        case dieTemp = "die_temp_c"
        case alertStatus = "alert_status"
        case alertMask = "alert_mask"
        case supplyAlertStatus = "supply_alert_status"
        case supplyAlertMask = "supply_alert_mask"
        case liveStatus = "live_status"
        case channels
        case diagnostics
        case muxStates = "muxStates"
        case freeHeap
        case minFreeHeap
        case uptimeMs
    }
}

// MARK: - IDAC Models
public struct IDACChannel: Codable, Identifiable, Equatable {
    public let id: Int
    public let code: Int
    public let targetV: Double
    public let midpointV: Double
    public let vMin: Double
    public let vMax: Double
    public let stepMv: Double
    public let calibrated: Bool
    public let name: String
    public let polyValid: Bool
    public let calPoly: [Double]
}

public struct IDACState: Codable, Equatable {
    public let present: Bool
    public let channels: [IDACChannel]
}

// MARK: - IOExp Models
public struct IOExpEnables: Codable, Equatable {
    public let vadj1: Bool
    public let vadj2: Bool
    public let analog15v: Bool
    public let mux: Bool
    public let usbHub: Bool
    
    enum CodingKeys: String, CodingKey {
        case vadj1
        case vadj2
        case analog15v = "analog15v"
        case mux
        case usbHub = "usbHub"
    }
}

public struct IOExpPowerGood: Codable, Equatable {
    public let logic: Bool
    public let vadj1: Bool
    public let vadj2: Bool
}

public struct IOExpEFuse: Codable, Equatable, Identifiable {
    public let id: Int
    public let enabled: Bool
    public let fault: Bool
}

public struct IOExpState: Codable, Equatable {
    public let present: Bool
    public let enables: IOExpEnables
    public let powerGood: IOExpPowerGood
    public let efuses: [IOExpEFuse]?
}

// MARK: - Supplies / Rails
public struct OverviewRail: Codable, Identifiable, Equatable {
    public var id: Int { rail }
    public let rail: Int
    public let name: String
    public let voltage: Double
    public let ok: Bool
}

// MARK: - Overview Snapshot
public struct OverviewSnapshot: Codable, Equatable {
    public let idac: IDACState
    public let ioexp: IOExpState
    public let rails: [OverviewRail]
}

// MARK: - Self-Test / Calibration
public struct SelftestBoot: Codable, Equatable {
    public let ran: Bool
    public let passed: Bool
    public let vadj1V: Double
    public let vadj2V: Double
    public let vlogicV: Double
}

public struct SelftestCalibration: Codable, Equatable {
    public let status: Int
    public let channel: Int
    public let points: Int
    public let lastVoltageV: Double
    public let errorMv: Double

    /// Human-readable calibration state description
    public var statusText: String {
        switch status {
        case 0: return "Idle"
        case 1: return "Running"
        case 2: return "Complete"
        case 3: return "Error"
        default: return "Unknown (\(status))"
        }
    }
}

public struct SelftestStatus: Codable, Equatable {
    public let boot: SelftestBoot
    public let calibration: SelftestCalibration
    public let workerEnabled: Bool
    public let supplyMonitorActive: Bool
}

// MARK: - Selftest Cached Supply Voltages

public struct SelftestSupplyRail: Codable, Equatable, Identifiable {
    public var id: Int { rail }
    public let rail: Int
    public let name: String
    public let voltageV: Double
}

public struct SelftestSupplyCached: Codable, Equatable {
    public let available: Bool
    public let timestampMs: Double
    public let rails: [SelftestSupplyRail]
}

// MARK: - Calibration Points

public struct CalibrationPoint: Codable, Identifiable, Equatable {
    public var id: Int { dacCode }
    public let dacCode: Int
    public let measuredV: Double
}

// MARK: - Wi-Fi Management
public struct WifiNetwork: Codable, Identifiable, Equatable {
    public var id: String { ssid }
    public let ssid: String
    public let rssi: Int
    public let auth: Int
    
    public var secure: Bool {
        return auth != 0
    }
}

public struct WifiStatus: Codable, Equatable {
    public let connected: Bool
    public let staSSID: String
    public let staIP: String
    public let rssi: Int
    public let apSSID: String
    public let apIP: String
    public let apMAC: String
    
    enum CodingKeys: String, CodingKey {
        case connected
        case staSSID = "sta_ssid"
        case staIP = "sta_ip"
        case rssi
        case apSSID = "ap_ssid"
        case apIP = "ap_ip"
        case apMAC = "ap_mac"
    }
}

public struct WifiScanResponse: Codable, Equatable {
    public let networks: [WifiNetwork]
}

// MARK: - HAT Rails
public struct HatRail: Codable, Identifiable, Equatable {
    public var id: Int { railId }
    public let railId: Int
    public let enabled: Bool
    public let voltageMv: Int
    public let targetVoltageMv: Int?
    public let currentMa: Int
    public let status: Int

    public var configuredVoltageMv: Int {
        if let t = targetVoltageMv, t > 0 { return t }
        return voltageMv
    }
}

public struct HatRailsResponse: Codable, Equatable {
    public let railCount: Int
    public let rails: [HatRail]
}

// MARK: - HAT Status
public struct HatStatus: Codable, Equatable {
    public let detected: Bool?
    public let present: Bool?
    public let kind: String?
    public var isPresent: Bool { detected ?? present ?? false }
    public var isDaqHat: Bool { isPresent && kind == "daq" }
}

// MARK: - DAQ WiFi Stream Status
public struct DaqWifiStreamStatus: Codable {
    public let state: String   // "idle" | "starting" | "ready" | "failed"
    /// Only present while state == "starting": "requested" | "ap" | "dns" | "tcp".
    /// Real P4 bring-up progress (softAP retry loop -> fast-fail DNS -> TCP
    /// backend), not a synthetic timer -- drives the play-button progress screen.
    public let stage: String?
    public let ssid: String?
    public let password: String?
    public let host: String?
    public let port: Int?
}

// MARK: - Device Info
public struct DeviceInfo: Codable, Equatable {
    public let siliconRev: Int
    public let siliconId0: String
    public let siliconId1: String
    public let macAddress: String
    public let spiOk: Bool
    
    enum CodingKeys: String, CodingKey {
        case siliconRev = "silicon_rev"
        case siliconId0 = "siliconId0"
        case siliconId1 = "siliconId1"
        case macAddress = "mac_address"
        case spiOk = "spi_ok"
    }
}

// MARK: - Device Version

public struct DeviceVersion: Codable, Equatable {
    public let esp32: String?
    public let hat: String?

    enum CodingKeys: String, CodingKey {
        case esp32 = "esp32"
        case hat = "hat"
    }
}

// MARK: - GPIO Models

public struct GPIOPin: Codable, Identifiable, Equatable {
    public var id: Int { pin }
    public let pin: Int
    public let mode: Int       // 0=disabled, 1=input, 2=output, 3=open-drain, 4=input-pulldown
    public let input: Bool
    public let output: Bool

    public var modeLabel: String {
        switch mode {
        case 0: return "Disabled"
        case 1: return "Input"
        case 2: return "Output"
        case 3: return "Open Drain"
        case 4: return "Input PD"
        default: return "Unknown"
        }
    }
}

public struct GPIOStatusResponse: Codable, Equatable {
    public let gpios: [GPIOPin]
}

// MARK: - Internal Selftest Supplies (AD74416H diagnostics)

public struct InternalSupplyEntry: Codable, Identifiable, Equatable {
    public var id: String { name }
    public let name: String
    public let value: Double
    public let unit: String
}

public struct InternalSuppliesResponse: Codable, Equatable {
    public let supplies: [InternalSupplyEntry]
}

// MARK: - Script Autorun

public struct AutorunStatus: Codable, Equatable {
    public let enabled: Bool
    public let scriptName: String?

    enum CodingKeys: String, CodingKey {
        case enabled
        case scriptName = "script_name"
    }
}

// MARK: - Toast Message

public enum ToastType {
    case success
    case error
    case info
}

public struct ToastMessage: Identifiable, Equatable {
    public let id = UUID()
    public let text: String
    public let type: ToastType

    public static func == (lhs: ToastMessage, rhs: ToastMessage) -> Bool {
        lhs.id == rhs.id
    }
}

// MARK: - OTA / Firmware Update

/// One published GitHub release, classified into per-MCU-target asset
/// availability. Four independently-updatable targets exist:
/// ESP32-S3 mainboard, RP2040 LA HAT, and (DAQ HAT only) ESP32-P4 and ESP32-C6.
///
/// Real asset names (see `.mex/patterns/daq-hat-release-images.md` and
/// `Firmware/ESP32/src/update/update_manager.cpp:fill_component_from_asset`):
///   esp32:  bugbuster-esp32s3-vX.Y.Z-ota.bin
///   rp2040: bugbuster-hat-rp2040-vX.Y.Z.bin
///   p4:     bugbuster-daq-p4-vX.Y.Z-ota.bin       (app-only OTA image)
///   c6:     bugbuster-daq-c6-vX.Y.Z-merged.bin    (full merged image from 0x0)
///
/// P4 and C6 are NOT interchangeable formats -- the app-only image goes
/// through `esp_ota_write()` into an A/B slot, the merged image is pushed by
/// the ESP-ROM loader starting at flash offset 0. Crossing them can brick the
/// C6 with no second MCU to recover it. This model therefore keeps them as two
/// independent, separately-classified asset slots rather than one generic
/// "DAQ HAT" slot a UI could misdirect.
public struct GitRelease: Identifiable {
    public let id: String
    public let tag: String
    public let publishedAt: String

    public let esp32Available: Bool
    public let esp32Version: String
    public let hatAvailable: Bool       // RP2040 LA HAT
    public let spiffsAvailable: Bool
    public let p4Available: Bool        // DAQ HAT ESP32-P4 (app-only OTA image)
    public let p4Version: String
    public let c6Available: Bool        // DAQ HAT ESP32-C6 (full merged image)
    public let c6Version: String
}

/// The four independently-updatable MCU targets accepted by
/// `POST /api/update/apply` (`api_core.cpp:api_ota_apply`, mirrored in
/// `webserver.cpp:handle_post_update_apply` -- both parsers must be kept in
/// sync by hand; see `.mex/patterns/multi-mcu-ota.md`, "There are TWO
/// /api/update/apply handlers, not one.").
///
/// Naming ANY key in the body resets the selection from scratch rather than
/// layering onto the legacy rp2040+esp32 default, and an all-false body is a
/// 400 on the device. This struct always sends all four keys explicitly, so
/// there is no ambiguity between "not mentioned" and "explicitly off".
/// Device-enforced apply order is RP2040 -> C6 -> P4 -> ESP32
/// (`UPDATE_TARGET_ORDER`, update_manager.h) -- the UI does not need to
/// reproduce that ordering, only the selection.
public struct OtaApplyTargets {
    public var rp2040: Bool
    public var esp32: Bool
    public var p4: Bool
    public var c6: Bool

    public init(rp2040: Bool = false, esp32: Bool = false, p4: Bool = false, c6: Bool = false) {
        self.rp2040 = rp2040
        self.esp32 = esp32
        self.p4 = p4
        self.c6 = c6
    }

    public var isEmpty: Bool { !rp2040 && !esp32 && !p4 && !c6 }

    public var jsonBody: [String: Any] {
        ["rp2040": rp2040, "esp32": esp32, "p4": p4, "c6": c6]
    }
}
