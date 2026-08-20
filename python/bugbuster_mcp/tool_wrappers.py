"""
BugBuster MCP - Tool error context wrapper.

Catches device errors and re-raises with actionable agent-readable messages.
"""

from __future__ import annotations
import functools
from typing import Callable, Any
from . import session
from .error_mapping import (
    map_device_error,
    map_hat_type_error,
    map_usb_only_error,
)


def with_error_context(tool_name: str):
    """
    Decorator that wraps a tool function to provide better error messages.
    
    Catches exceptions from the BugBuster library and re-raises them with
    actionable messages that include tool name, transport context, and suggestions.
    
    Usage:
        @mcp.tool()
        @with_error_context("my_tool")
        def my_tool(params):
            ...
    """
    def decorator(func: Callable) -> Callable:
        @functools.wraps(func)
        def wrapper(*args, **kwargs) -> Any:
            try:
                return func(*args, **kwargs)
            except Exception as e:
                # Check for device error patterns in the exception message
                error_msg = str(e)
                
                # Pattern: "Device error 0x11" or similar
                if "device error" in error_msg.lower():
                    import re
                    match = re.search(r'device error (0x[0-9a-f]+)', error_msg, re.IGNORECASE)
                    if match:
                        error_code_str = match.group(1)
                        error_code = int(error_code_str, 16)
                        
                        # Get transport context if available
                        transport = None
                        try:
                            transport = session.get_transport()
                            if transport == "auto":
                                transport = None
                        except Exception:
                            pass
                        
                        # Generate actionable message
                        actionable_msg = map_device_error(
                            error_code,
                            tool_name,
                            transport=transport,
                        )
                        raise RuntimeError(actionable_msg) from e
                
                # Re-raise other exceptions as-is but with tool context
                if "tool" not in error_msg.lower() and tool_name not in error_msg:
                    raise RuntimeError(f"Tool '{tool_name}' failed: {error_msg}") from e
                raise
        
        return wrapper
    return decorator


def require_hat_type(expected_type_name: str, expected_type_code: int | None = None):
    """
    Decorator for tools that require a specific HAT type (e.g. LA HAT vs DAQ HAT).
    
    Checks the attached HAT type before executing the tool and raises a clear
    error if the wrong HAT is attached.
    
    Usage:
        @mcp.tool()
        @require_hat_type("RP2040 LA HAT", expected_type_code=0x02)
        def hat_get_caps():
            ...
    
    Parameters:
    - expected_type_name: Human-readable name (e.g. "RP2040 LA HAT")
    - expected_type_code: Optional numeric type code (e.g. 0x02)
    """
    def decorator(func: Callable) -> Callable:
        @functools.wraps(func)
        def wrapper(*args, **kwargs) -> Any:
            # Try to get HAT status and check type
            try:
                bb = session.get_client()
                hat_status = bb.hat_get_status()
                
                # Check if HAT is detected at all
                if not hat_status.get("detected"):
                    from .error_mapping import map_hal_requirement_error
                    raise RuntimeError(
                        map_hal_requirement_error(func.__name__, expected_type_name)
                    )
                
                # Check HAT type if we have a code to compare against
                if expected_type_code is not None:
                    actual_type = hat_status.get("hat_type", hat_status.get("type"))
                    if actual_type is not None and int(actual_type) != expected_type_code:
                        # Determine actual type name
                        type_names = {
                            0x02: "RP2040 LA HAT",
                            0x10: "DAQ HAT (ESP32-P4)",
                        }
                        actual_type_name = type_names.get(int(actual_type))
                        
                        raise RuntimeError(
                            map_hat_type_error(
                                func.__name__,
                                expected_type_name,
                                actual_type_name,
                                int(actual_type),
                            )
                        )
            except RuntimeError:
                # Re-raise our own errors
                raise
            except Exception:
                # If we can't check, let the tool run and fail naturally
                pass
            
            return func(*args, **kwargs)
        
        return wrapper
    return decorator


def require_usb_transport():
    """
    Decorator for tools that only work over USB transport.
    
    Checks the transport before executing and raises a clear error if HTTP is used.
    
    Usage:
        @mcp.tool()
        @require_usb_transport()
        def register_access(...):
            ...
    """
    def decorator(func: Callable) -> Callable:
        @functools.wraps(func)
        def wrapper(*args, **kwargs) -> Any:
            transport = session.get_transport()
            if transport == "http" or (transport == "auto" and not session.is_usb()):
                raise RuntimeError(map_usb_only_error(func.__name__, transport or "non-USB"))
            return func(*args, **kwargs)
        
        return wrapper
    return decorator
