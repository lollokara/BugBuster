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
    public let channelAlert: Int
    public let channelAlertMask: Int
    public let rtdExcitationUa: Double
    
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
        case channelAlert = "channel_alert"
        case channelAlertMask = "channel_alert_mask"
        case rtdExcitationUa = "rtd_excitation_ua"
    }
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
public struct CalibrationPoint: Codable, Equatable, Identifiable {
    public var id: Int { dacCode }
    public let dacCode: Int
    public let measuredV: Double
}

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
}

public struct SelftestStatus: Codable, Equatable {
    public let boot: SelftestBoot
    public let calibration: SelftestCalibration
    public let workerEnabled: Bool
    public let supplyMonitorActive: Bool
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
    public let currentMa: Int
    public let status: Int
}

public struct HatRailsResponse: Codable, Equatable {
    public let railCount: Int
    public let rails: [HatRail]
}

// MARK: - HAT Status
public struct HatStatus: Codable, Equatable {
    public let detected: Bool?
    public let present: Bool?
    public var isPresent: Bool { detected ?? present ?? false }
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
