# BugBuster Board Profiles

Board Profiles provide structured knowledge about the Device Under Test (DUT) connected to BugBuster. They allow the AI and the UI to understand pin mappings, power domains, and enforce safety constraints.

## Profile Schema (JSON)

Profiles are stored in `python/bugbuster_mcp/board_profiles/*.json`.

```json
{
  "name": "board_id",
  "description": "Human readable description",
  "vlogic": { "value": 3.3, "locked": true },
  "vadj1": { "value": 3.3, "locked": false },
  "vadj2": { "value": 5.0, "locked": true },
  "pins": {
    "1": { "name": "PIN_NAME", "type": "TYPE", "direction": "IN/OUT" },
    ... up to 12 ...
  },
  "swd": { "target": "openocd_target_name" },
  "uart": { "baudrate": 115200 }
}
```

### Safety Locking
If a power domain (vlogic, vadj1, vadj2) is marked as `"locked": true`, the Python HAL will prevent any tool from changing that voltage from the value specified in the profile. This prevents accidental damage to the DUT.

## MCP Usage

### 1. List available boards
Use the `list_boards` tool to see what profiles are available on the server.

### 2. Set the active board
Use the `set_board(name="stm32f4_discovery")` tool to load a profile. This orientates the AI to the connected hardware.

### 3. Read board context
The `bugbuster://board` resource provides the full structured profile. The AI uses this to map its high-level goals (e.g., "Reset the target") to physical IOs (e.g., "Pulse IO 7 low").

## Desktop App Usage

The **Board** tab in the Desktop App allows you to:
1. Visualize the BugBuster IO blocks.
2. Configure the names and types of connected pins.
3. Set and lock power domain voltages.
4. **Export Profile**: Generates a JSON profile that you can save to the `board_profiles/` directory for the MCP server to use.

### IO Block Constraints
- **IOs 1, 4, 7, 10**: Support "Analog" modes (ADC/DAC/RTD).
- **All other IOs**: Digital-only (GPIO, GPI, GPO).
- **VADJ1**: Powers IOs 1 through 6.
- **VADJ2**: Powers IOs 7 through 12.
