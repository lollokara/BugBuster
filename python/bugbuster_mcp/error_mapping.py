"""
BugBuster MCP - Device error code mapping.

Translates bare device error codes into actionable agent-readable messages.
"""

from __future__ import annotations
from typing import Optional


# Device error codes - GENERATED from bbp.h by Firmware/tools/gen_error_tables.py.
ERROR_MESSAGES = {
    # BEGIN GENERATED: bbp-errors
    0x01: ("invalid_cmd", "The command is not recognized by the device"),
    0x02: ("invalid_channel", "Channel number is out of range"),
    0x03: ("invalid_param", "One or more parameters are invalid"),
    0x04: ("spi_fail", "SPI communication with the AD74416H failed"),
    0x05: ("queue_full", "Device command queue is full - retry after a short delay"),
    0x06: ("busy", "Device is busy processing another operation"),
    0x07: ("invalid_state", "Operation cannot be performed in the current device state"),
    0x08: ("crc_fail", "Payload CRC check failed"),
    0x09: ("frame_too_large", "Command or response payload exceeds the maximum frame size"),
    0x0A: ("stream_active", "Cannot configure while streaming is active"),
    0x11: ("timeout", "Device operation timed out or no response received"),
    0x12: ("io_ownership_required", "IO slot is owned by another session"),
    0x13: ("adgs_route_rejected", "MUX route rejected by the U17-S3 / U23 self-test interlock"),
    0x14: ("unsupported_hat", "Command requires a different HAT type, or no HAT is attached"),
    # END GENERATED: bbp-errors
}


def map_device_error(
    error_code: int,
    tool_name: str,
    transport: Optional[str] = None,
    operation: Optional[str] = None,
) -> str:
    """
    Map a device error code to an actionable message.

    Parameters:
    - error_code: The numeric error code from device (e.g. 0x11)
    - tool_name: The MCP tool that failed
    - transport: Current transport ('usb' or 'http'), if known
    - operation: Optional description of what was being attempted

    Returns: A human-readable message explaining the error and suggesting next steps
    """
    error_name, base_message = ERROR_MESSAGES.get(error_code, ("unknown", f"Unknown error code {error_code:#04x}"))

    context = f"Tool '{tool_name}' failed"
    if operation:
        context += f" while {operation}"
    context += f": {base_message}"

    # Add specific guidance based on error code
    if error_code == 0x11:  # Timeout
        suggestions = []
        if transport == "usb":
            suggestions.append("Check USB connection and try reset_link if commands are timing out")
        elif transport == "http":
            suggestions.append("Check network connection and device WiFi status")
        else:
            suggestions.append("Check device connection")
        
        suggestions.append("Verify the device supports this operation")
        suggestions.append("For HAT operations, ensure the correct HAT type is attached")
        
        return f"{context}. Suggestions: {'; '.join(suggestions)}"
    
    elif error_code == 0x07:  # Invalid state
        return f"{context}. The device cannot perform this operation in its current state. Check device_status and ensure prerequisites are met"
    
    elif error_code == 0x0A:  # Stream active
        return f"{context}. Stop active streams (ADC/scope/LA) before changing configuration"
    
    elif error_code == 0x12:  # IO ownership
        return f"{context}. Use io_claim to acquire ownership of the IO slots, then pass the lease handle to this tool"
    
    elif error_code == 0x06:  # Busy
        return (f"{context}. Wait for the current operation to complete, then retry. "
                "If standby_status shows the device is preparing/asleep/waking, call standby_wake "
                "and retry once ready; the command is not replayed automatically")
    
    elif error_code == 0x13:  # MUX route rejected by the self-test interlock
        return f"{context}. U17 S3 (IO9 analog path) and the U23 self-test share a net; retry once the self-test / e-fuse current monitor releases U23"

    elif error_code == 0x14:  # Unsupported HAT
        return f"{context}. Check device_status for the attached HAT type; this command needs a different HAT"
    
    else:
        return f"{context}. Error: {error_name} ({error_code:#04x})"


def map_hat_type_error(tool_name: str, expected_type: str, actual_type: str | None, actual_type_code: int | None = None) -> str:
    """
    Generate error message for HAT type mismatch.

    Parameters:
    - tool_name: The MCP tool that requires a specific HAT
    - expected_type: Name of the required HAT type (e.g. "RP2040 LA HAT")
    - actual_type: Name of the attached HAT, if known
    - actual_type_code: Numeric HAT type code, if known

    Returns: A clear error message stating the HAT requirement
    """
    msg = f"Tool '{tool_name}' requires {expected_type}"
    
    if actual_type:
        msg += f", but {actual_type}"
        if actual_type_code is not None:
            msg += f" (type {actual_type_code:#04x})"
        msg += " is attached"
    elif actual_type_code is not None:
        msg += f", but HAT type {actual_type_code:#04x} is attached"
    else:
        msg += ", but a different HAT type is attached or no HAT is present"
    
    return msg + ". Check the HAT connection and ensure you have the correct hardware"


def map_usb_only_error(tool_name: str, transport: str) -> str:
    """
    Generate error message for USB-only tool called over HTTP.

    Parameters:
    - tool_name: The MCP tool that requires USB
    - transport: The transport that was used

    Returns: A clear error message
    """
    return (
        f"Tool '{tool_name}' requires USB transport but was called over {transport}. "
        f"Connect via USB (--transport usb) to access this feature"
    )


def map_hal_requirement_error(tool_name: str, hal_type: str) -> str:
    """
    Generate error message for missing HAL requirements.

    Parameters:
    - tool_name: The MCP tool that failed
    - hal_type: What type of HAL/hardware is required (e.g. "DAQ HAT", "RP2040 HAT")

    Returns: A clear error message
    """
    return (
        f"Tool '{tool_name}' requires {hal_type} but it is not present or not responding. "
        f"Check physical connection and device_status"
    )
