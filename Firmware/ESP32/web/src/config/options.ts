// =============================================================================
// Shared option catalogs for desktop/web parity.
// Values mirror DesktopApp/BugBuster/src/tauri_bridge.rs constants.
// =============================================================================

export const CHANNEL_FUNCTION_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "High Impedance" },
  { code: 1, label: "Voltage Out" },
  { code: 2, label: "Current Out" },
  { code: 3, label: "Voltage In" },
  { code: 4, label: "Current In (Ext)" },
  { code: 5, label: "Current In (Loop)" },
  { code: 7, label: "Resistance" },
  { code: 8, label: "Digital In (Logic)" },
  { code: 9, label: "Digital In (Loop)" },
  { code: 10, label: "Current Out HART" },
  { code: 11, label: "Current In HART (Ext)" },
  { code: 12, label: "Current In HART (Loop)" },
];

export const CHANNEL_FUNCTION_LABELS: Record<number, string> = Object.fromEntries(
  CHANNEL_FUNCTION_OPTIONS.map((it) => [it.code, it.label]),
);

export const ADC_RANGE_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "0 to 12V" },
  { code: 1, label: "-12 to 12V" },
  { code: 2, label: "-312.5 to 312.5mV" },
  { code: 3, label: "-312.5 to 0mV" },
  { code: 4, label: "0 to 312.5mV" },
  { code: 5, label: "0 to 625mV" },
  { code: 6, label: "-104.16 to 104.16mV" },
  { code: 7, label: "-2.5 to 2.5V" },
];

export const ADC_RATE_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "10 SPS HR" },
  { code: 1, label: "20 SPS" },
  { code: 3, label: "20 SPS HR" },
  { code: 4, label: "200 SPS HR1" },
  { code: 6, label: "200 SPS HR" },
  { code: 8, label: "1.2 kSPS" },
  { code: 9, label: "1.2 kSPS HR" },
  { code: 12, label: "4.8 kSPS" },
  { code: 13, label: "9.6 kSPS" },
];

export const ADC_MUX_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "LF to AGND" },
  { code: 1, label: "HF to LF (diff)" },
  { code: 2, label: "VSENSE- to AGND" },
  { code: 3, label: "LF to VSENSE-" },
  { code: 4, label: "AGND to AGND (self-test)" },
];

export const DIN_DEBOUNCE_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "None" },
  { code: 1, label: "1ms" },
  { code: 2, label: "2ms" },
  { code: 3, label: "4ms" },
  { code: 4, label: "8ms" },
  { code: 5, label: "16ms" },
  { code: 6, label: "32ms" },
  { code: 7, label: "64ms" },
];

export const DO_MODE_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "High-Z" },
  { code: 1, label: "Push-Pull" },
  { code: 2, label: "Open Drain" },
  { code: 3, label: "Push-Pull HART" },
];

export const UART_BAUD_OPTIONS: readonly number[] = [
  300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800,
  921600,
];

export const UART_PARITY_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "None" },
  { code: 1, label: "Odd" },
  { code: 2, label: "Even" },
];

export const UART_STOP_BITS_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "1" },
  { code: 1, label: "1.5" },
  { code: 2, label: "2" },
];

export const UART_DATA_BITS_OPTIONS: readonly number[] = [5, 6, 7, 8];

export const USBPD_VOLTAGE_OPTIONS: ReadonlyArray<5 | 9 | 12 | 15 | 18 | 20> = [
  5, 9, 12, 15, 18, 20,
];

export const DIAG_SOURCE_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "AGND" },
  { code: 1, label: "Temperature" },
  { code: 2, label: "DVCC" },
  { code: 3, label: "AVCC" },
  { code: 4, label: "LDO 1.8V" },
  { code: 5, label: "AVDD HI" },
  { code: 6, label: "AVDD LO" },
  { code: 7, label: "AVSS" },
  { code: 8, label: "LVIN" },
  { code: 9, label: "DO VDD" },
  { code: 10, label: "VSENSE+" },
  { code: 11, label: "VSENSE-" },
  { code: 12, label: "DO Current" },
  { code: 13, label: "AVDD" },
];

export const HAT_PIN_FUNCTION_OPTIONS: ReadonlyArray<{ code: number; label: string }> = [
  { code: 0, label: "Disconnected" },
  { code: 5, label: "GPIO1" },
  { code: 6, label: "GPIO2" },
  { code: 7, label: "GPIO3" },
  { code: 8, label: "GPIO4" },
];

export const WAVEGEN_WAVEFORM_OPTIONS: ReadonlyArray<{ code: 0 | 1 | 2 | 3; label: string }> = [
  { code: 0, label: "Sine" },
  { code: 1, label: "Triangle" },
  { code: 2, label: "Square" },
  { code: 3, label: "Sawtooth" },
];

export const WAVEGEN_MODE_OPTIONS: ReadonlyArray<{ code: 0 | 1; label: string }> = [
  { code: 0, label: "Voltage" },
  { code: 1, label: "Current" },
];
