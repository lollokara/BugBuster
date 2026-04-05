"""
BugBuster MCP — Safety configuration and default limits.
"""

# ---------------------------------------------------------------------------
# Voltage / current safety limits
# ---------------------------------------------------------------------------

# Maximum VADJ rail voltages (LTM8063 hardware max is 15 V)
MAX_VADJ_VOLTAGE = 15.0          # V
MIN_VADJ_VOLTAGE = 3.0           # V

# VLOGIC limits (TPS74601)
MAX_VLOGIC = 5.0                 # V
MIN_VLOGIC = 1.8                 # V

# AD74416H DAC output limits
MAX_DAC_VOLTAGE_UNIPOLAR = 12.0  # V
MAX_DAC_VOLTAGE_BIPOLAR  = 12.0  # V (absolute, i.e. ±12 V)
MAX_DAC_CURRENT_MA       = 25.0  # mA
MAX_DAC_CURRENT_MA_SAFE  = 8.0   # mA — conservative AI default

# Voltage above which explicit confirm=True is required
VADJ_CONFIRM_THRESHOLD = 12.0   # V

# ---------------------------------------------------------------------------
# ADC capture limits
# ---------------------------------------------------------------------------

MAX_SNAPSHOT_DURATION_S = 10.0   # seconds
DEFAULT_SNAPSHOT_DURATION_S = 1.0
MAX_SNAPSHOT_SAMPLES = 500       # polling-based; ADC at 200 SPS ≈ 200 samples/s
SNAPSHOT_POLL_INTERVAL_S = 0.005 # 5 ms between polls (~200 SPS)

# Number of points in the waveform_preview downsampled result
WAVEFORM_PREVIEW_POINTS = 100

# ---------------------------------------------------------------------------
# Logic analyzer defaults
# ---------------------------------------------------------------------------

LA_DEFAULT_RATE_HZ    = 1_000_000   # 1 MHz
LA_DEFAULT_DEPTH      = 100_000     # samples
LA_DEFAULT_CHANNELS   = 4
LA_CAPTURE_TIMEOUT_S  = 5.0

# ---------------------------------------------------------------------------
# USB PD allowed voltages (HUSB238 negotiated values)
# ---------------------------------------------------------------------------

USBPD_ALLOWED_VOLTAGES = {5, 9, 12, 15, 18, 20}  # V

# ---------------------------------------------------------------------------
# IO definitions
# ---------------------------------------------------------------------------

# IOs that can be used in analog modes (route to AD74416H)
ANALOG_IOS = frozenset({1, 4, 7, 10})

# All valid IO numbers
ALL_IOS = frozenset(range(1, 13))

# VADJ rail for each IO block
IO_TO_RAIL = {
    1: 1, 2: 1, 3: 1,    # IO_Block 1 → VADJ1
    4: 1, 5: 1, 6: 1,    # IO_Block 2 → VADJ1
    7: 2, 8: 2, 9: 2,    # IO_Block 3 → VADJ2
    10: 2, 11: 2, 12: 2, # IO_Block 4 → VADJ2
}

# IO_Block each IO belongs to (for e-fuse mapping)
IO_TO_IOBLOCK = {
    1: 1, 2: 1, 3: 1,
    4: 2, 5: 2, 6: 2,
    7: 3, 8: 3, 9: 3,
    10: 4, 11: 4, 12: 4,
}
