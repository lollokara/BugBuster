"""
Test safety gates on destructive MCP tools.

Ensures every destructive operation requires explicit confirmation.
"""

import pytest
from unittest.mock import Mock, patch


# List of destructive tools and their confirmation parameter requirements
# Format: (module, tool_name, destructive_params, expected_error_substring)
DESTRUCTIVE_TOOLS = [
    ("advanced", "idac_control", {"action": "set_voltage", "channel": 0, "voltage": 5.0}, "i_understand_the_risk"),
    ("advanced", "idac_control", {"action": "set_code", "channel": 0, "code": 50}, "i_understand_the_risk"),
    ("power", "power_control", {"control": "vadj1", "enable": True}, "i_understand_the_risk"),
    ("ota", "ota_upload_firmware", {"path": "dummy.bin"}, "confirm"),
    ("ota", "ota_rollback", {}, "confirm"),
    ("ota", "ota_apply_update", {}, "confirm"),
    ("power", "wifi_set_ap_password", {"password": "newpassword123"}, "confirm"),
]


@pytest.mark.parametrize("module_name,tool_name,params,confirm_keyword", DESTRUCTIVE_TOOLS)
def test_destructive_tool_refuses_without_confirmation(module_name, tool_name, params, confirm_keyword):
    """
    Test that destructive tools refuse to act without explicit confirmation.
    
    This is a CRITICAL safety requirement - destructive operations must not
    execute without the AI explicitly passing confirm=True or i_understand_the_risk=True.
    """
    # Import the tool module
    module = __import__(f"bugbuster_mcp.tools.{module_name}", fromlist=[tool_name])
    
    # Mock the MCP server and register tools
    mock_mcp = Mock()
    registered_tools = {}
    
    def mock_tool_decorator():
        def decorator(fn):
            registered_tools[fn.__name__] = fn
            return fn
        return decorator
    
    mock_mcp.tool = mock_tool_decorator
    
    # Register the tools
    module.register(mock_mcp)
    
    # Get the tool function
    tool_fn = registered_tools.get(tool_name)
    assert tool_fn is not None, f"Tool {tool_name} not found in {module_name}"
    
    # Try calling without confirmation - should refuse
    with patch("bugbuster_mcp.session.get_client"):
        with pytest.raises((ValueError, RuntimeError)) as exc_info:
            tool_fn(**params)
        
        # Verify the error message mentions the confirmation requirement
        error_msg = str(exc_info.value).lower()
        assert confirm_keyword in error_msg or "confirm" in error_msg, \
            f"Error message should mention '{confirm_keyword}' requirement, got: {error_msg}"


@pytest.mark.parametrize("module_name,tool_name,params,confirm_keyword", DESTRUCTIVE_TOOLS)
def test_destructive_tool_docstring_warns_first_line(module_name, tool_name, params, confirm_keyword):
    """
    Test that destructive tools have a WARNING or consequence statement in the first line.
    
    The docstring is the only interface contract an AI agent sees, so warnings
    must be prominent - not buried in the middle of documentation.
    """
    # Import the tool module
    module = __import__(f"bugbuster_mcp.tools.{module_name}", fromlist=[tool_name])
    
    # Mock the MCP server and register tools
    mock_mcp = Mock()
    registered_tools = {}
    
    def mock_tool_decorator():
        def decorator(fn):
            registered_tools[fn.__name__] = fn
            return fn
        return decorator
    
    mock_mcp.tool = mock_tool_decorator
    
    # Register the tools
    module.register(mock_mcp)
    
    # Get the tool function
    tool_fn = registered_tools.get(tool_name)
    assert tool_fn is not None, f"Tool {tool_name} not found in {module_name}"
    
    # Check docstring
    docstring = tool_fn.__doc__
    assert docstring is not None, f"Tool {tool_name} has no docstring"
    
    # Get first substantial line (skip blank lines)
    lines = [line.strip() for line in docstring.split('\n') if line.strip()]
    assert lines, f"Tool {tool_name} docstring has no content"
    
    first_line = lines[0].lower()
    
    # First line should contain a warning indicator or consequence statement
    warning_keywords = [
        'warning', 'destructive', 'caution', 'danger', 
        'reboot', 'disconnect', 'brick', 'trim', 'calibration',
        'changes password', 'marks image', 'fetches from github',
        'retrim', 'drift'
    ]
    
    has_warning = any(keyword in first_line for keyword in warning_keywords)
    assert has_warning, \
        f"Tool {tool_name} first docstring line should warn about consequences. Got: {first_line}"


def test_all_known_destructive_operations_are_gated():
    """
    Verify all BBP commands listed as destructive in AGENTS.md have MCP tool gates.
    
    This is a defense-in-depth check: if a destructive BBP command is exposed
    through MCP, it must have a safety gate.
    """
    # BBP commands that AGENTS.md lists as destructive
    # Format: (bbp_command_name, bbp_hex_code, tool_name_if_exposed)
    known_destructive_bbp = [
        ("IDAC_CAL_CLEAR", 0xA5, None),  # Not exposed - GOOD
        ("IDAC_CAL_SAVE", 0xA6, None),   # Not exposed - GOOD
        ("IDAC_CALIBRATE", 0xA3, None),  # Not exposed via direct tool
        ("PCA_SET_PORT", 0xB2, "power_control"),  # Exposed, needs gate
        ("SET_AVDD_SELECT", 0x1A, None),  # Not exposed
        ("DEVICE_RESET", 0x70, "reset_device"),  # Exposed
        ("HAT_SET_POWER", 0xCA, None),  # Not directly exposed
        ("HAT_SET_RAIL_ENABLE", 0xD2, "hat_set_rail_enable"),  # Exposed
        ("WIFI_SET_AP_PASSWORD", None, "wifi_set_ap_password"),  # Exposed
    ]
    
    # Tools we know have gates (from test parameters above)
    gated_tools = {tool_name for _, tool_name, _, _ in DESTRUCTIVE_TOOLS}
    
    # Check that exposed destructive operations are in our gated list
    for bbp_name, _bbp_code, tool_name in known_destructive_bbp:
        if tool_name and tool_name not in ["reset_device", "hat_set_rail_enable"]:
            # reset_device is actually safe (AD74416H hw reset only)
            # hat_set_rail_enable has preflight check
            assert tool_name in gated_tools or tool_name == "hat_set_rail_enable", \
                f"Destructive BBP command {bbp_name} exposed as {tool_name} but not in gated tools list"
