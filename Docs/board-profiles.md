# Board profiles

A board profile is a JSON description of the device under test: what each IO is
wired to, what voltage each rail should be, and which of those rails must not
change. It gives the AI and the UI enough context to translate "reset the
target" into "pulse IO 7 low", and it stops either of them from putting 5 V on a
3.3 V board.

Profiles live in
[`python/bugbuster_mcp/board_profiles/`](../python/bugbuster_mcp/board_profiles/).

## Schema

```json
{
  "name": "board_id",
  "description": "Human readable description",
  "vlogic": { "value": 3.3, "locked": true },
  "vadj1":  { "value": 3.3, "locked": false },
  "vadj2":  { "value": 5.0, "locked": true },
  "pins": {
    "1": { "name": "PIN_NAME", "type": "TYPE", "direction": "IN" }
  },
  "swd":  { "target": "openocd_target_name" },
  "uart": { "baudrate": 115200 }
}
```

`pins` is keyed by physical IO number, 1 to 12.

## Rail locking

A rail marked `"locked": true` cannot be moved away from its `value` while the
profile is active. `validate_vadj_voltage()` and `validate_vlogic()` in
[`python/bugbuster_mcp/safety.py`](../python/bugbuster_mcp/safety.py) reject any
request that differs by more than 50 mV, with an error naming the profile.

That check sits in the MCP tool layer, so no tool call - and no amount of
prompting - can get around it.

## Using a profile

| Step | How |
|---|---|
| See what is available | `list_boards()` |
| Activate one | `set_board(name="stm32f4_discovery")` |
| Read the active profile | the `bugbuster://board` resource |

Activation is session state (`python/bugbuster_mcp/session.py`); it resets when
the server restarts.

## Creating one

The desktop app's **Board Map** tab does it visually: name the pins, set the
rail voltages, mark the ones that must not move, then **Export Profile**. Save
the result into `board_profiles/` and the MCP server picks it up.

## IO constraints to respect

| | |
|---|---|
| IOs 3, 6, 9, 12 | Analog-capable: ADC, DAC, RTD, current, plus every digital mode |
| All other IOs | Digital only |
| VADJ1 | Powers IOs 1-6 |
| VADJ2 | Powers IOs 7-12 |
| VLOGIC | Level-shifter voltage for all 12 IOs |

VLOGIC is fixed at MCP server startup by `--vlogic` and is not settable by any
tool, profile or not.
