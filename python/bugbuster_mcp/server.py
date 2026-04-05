"""
BugBuster MCP Server
====================

An MCP (Model Context Protocol) server that exposes the BugBuster hardware
debugging platform to AI models (Claude, etc.).

28 tools organized in 9 groups:
  • Discovery & status   — device_status, device_info, check_faults, selftest
  • IO configuration     — configure_io, set_supply_voltage, reset_device
  • Analog measurement   — read_voltage, read_current, read_resistance
  • Analog output        — write_voltage, write_current
  • Digital IO           — read_digital, write_digital
  • Waveform & capture   — start_waveform, stop_waveform,
                           capture_adc_snapshot, capture_logic_analyzer
  • UART & debug         — setup_serial_bridge, setup_swd, uart_config
  • Power management     — usb_pd_status, usb_pd_select, power_control, wifi_status
  • Advanced (low-level) — mux_control, register_access, idac_control

Resources:  bugbuster://status  bugbuster://power  bugbuster://faults
            bugbuster://hat     bugbuster://capabilities

Prompts:    debug_unknown_device  measure_signal  program_target  power_cycle_test
"""

from mcp.server.fastmcp import FastMCP

mcp = FastMCP(
    name="BugBuster",
    instructions=(
        "BugBuster is a hardware debugging platform with a 4-channel 24-bit ADC/DAC, "
        "12 digital IOs, adjustable power supplies (3-15 V), USB PD, UART bridge, "
        "SWD debug probe, and logic analyzer. "
        "Always call device_status first to orient yourself. "
        "Always call configure_io before read/write operations on any IO. "
        "The MUX matrix is managed automatically by configure_io — "
        "each IO can only be in one mode (analog OR digital, not both simultaneously). "
        "IOs 1, 4, 7, 10 support analog modes; all 12 IOs support digital modes. "
        "E-fuse protection is enabled automatically by configure_io for output modes. "
        "Safety limits: DAC max 12 V unipolar, current max 8 mA by default, "
        "VADJ max 15 V (requires confirm=True above 12 V)."
    ),
)

# Register all tools, resources, and prompts
from .tools     import register_all as register_tools
from .resources import register_all as register_resources
from .prompts   import register_all as register_prompts

register_tools(mcp)
register_resources(mcp)
register_prompts(mcp)
