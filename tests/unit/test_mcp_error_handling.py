"""
Test error mapping and link_status improvements.

Ensures device errors are translated to actionable messages and
link_status returns real values instead of nulls.
"""

from unittest.mock import Mock, patch
from bugbuster_mcp.error_mapping import (
    map_device_error,
    map_hat_type_error,
    map_usb_only_error,
)


class TestErrorMapping:
    """Test device error code to actionable message mapping."""
    
    def test_timeout_error_includes_tool_name(self):
        """0x11 timeout errors should name the tool that failed."""
        msg = map_device_error(0x11, "hat_get_caps", transport="usb")
        assert "hat_get_caps" in msg
        assert "timed out" in msg.lower() or "timeout" in msg.lower()
    
    def test_timeout_error_suggests_reset_link_for_usb(self):
        """0x11 on USB should suggest reset_link."""
        msg = map_device_error(0x11, "hat_get_caps", transport="usb")
        assert "reset_link" in msg.lower()
    
    def test_timeout_error_suggests_network_check_for_http(self):
        """0x11 on HTTP should suggest network check."""
        msg = map_device_error(0x11, "ota_get_info", transport="http")
        assert "network" in msg.lower() or "wifi" in msg.lower()
    
    def test_timeout_error_mentions_hat_type_possibility(self):
        """0x11 timeout should mention HAT type mismatch as a possibility."""
        msg = map_device_error(0x11, "hat_get_caps", transport="usb")
        assert "hat" in msg.lower()
    
    def test_unknown_error_code_does_not_crash(self):
        """Unknown error codes should produce a message, not crash."""
        msg = map_device_error(0xFF, "some_tool")
        assert "some_tool" in msg
        assert "0xff" in msg.lower() or "255" in msg
    
    def test_invalid_state_error_actionable(self):
        """0x07 invalid state should suggest checking device_status."""
        msg = map_device_error(0x07, "configure_io")
        assert "device_status" in msg.lower()
        assert "state" in msg.lower()
    
    def test_io_ownership_error_suggests_io_claim(self):
        """0x12 should mention io_claim."""
        msg = map_device_error(0x12, "write_voltage")
        assert "io_claim" in msg.lower()
        assert "ownership" in msg.lower()
    
    def test_all_known_codes_have_messages(self):
        """Every error code in ERROR_MESSAGES should produce a message."""
        known_codes = [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x11, 0x12, 0x13]
        for code in known_codes:
            msg = map_device_error(code, "test_tool")
            assert msg
            assert "test_tool" in msg
            assert len(msg) > 20  # Should be a real explanation, not just the code


class TestHatTypeError:
    """Test HAT type mismatch error messages."""
    
    def test_hat_type_mismatch_clear_message(self):
        """HAT type mismatch should clearly state expected vs actual."""
        msg = map_hat_type_error("hat_get_caps", "RP2040 LA HAT", "DAQ HAT", 0x10)
        assert "hat_get_caps" in msg
        assert "RP2040 LA HAT" in msg
        assert "DAQ HAT" in msg
        assert "0x10" in msg.lower()
    
    def test_hat_type_with_code_only(self):
        """Should handle case where only type code is known."""
        msg = map_hat_type_error("hat_get_caps", "RP2040 LA HAT", None, 0x10)
        assert "0x10" in msg.lower()
        assert "RP2040 LA HAT" in msg
    
    def test_hat_type_no_actual_info(self):
        """Should handle case where no actual HAT info is available."""
        msg = map_hat_type_error("hat_get_caps", "RP2040 LA HAT", None, None)
        assert "RP2040 LA HAT" in msg
        assert "attached" in msg.lower()


class TestUsbOnlyError:
    """Test USB-only tool error messages."""
    
    def test_usb_only_over_http(self):
        """Should clearly state USB is required."""
        msg = map_usb_only_error("register_access", "http")
        assert "USB" in msg or "usb" in msg.lower()
        assert "register_access" in msg
        assert "http" in msg.lower()


class TestLinkStatus:
    """Test link_status returns real values instead of nulls."""
    
    def test_link_status_returns_real_transport_when_connected(self):
        """link_status should resolve transport to 'usb' or 'http', not 'auto'."""
        from bugbuster_mcp.tools import discovery
        
        mock_mcp = Mock()
        registered_tools = {}
        
        def mock_tool_decorator():
            def decorator(fn):
                registered_tools[fn.__name__] = fn
                return fn
            return decorator
        
        mock_mcp.tool = mock_tool_decorator
        discovery.register(mock_mcp)
        
        link_status_fn = registered_tools["link_status"]
        
        # Mock connected USB session
        with patch("bugbuster_mcp.session.get_transport", return_value="usb"):
            with patch("bugbuster_mcp.session.get_port", return_value="COM6"):
                with patch("bugbuster_mcp.session.link_healthy", return_value=True):
                    result = link_status_fn()
        
        assert result["transport"] == "usb"
        assert result["port"] == "COM6"
        assert result["healthy"] is True
    
    def test_link_status_handles_disconnected_state(self):
        """link_status should return False for healthy when not connected."""
        from bugbuster_mcp.tools import discovery
        
        mock_mcp = Mock()
        registered_tools = {}
        
        def mock_tool_decorator():
            def decorator(fn):
                registered_tools[fn.__name__] = fn
                return fn
            return decorator
        
        mock_mcp.tool = mock_tool_decorator
        discovery.register(mock_mcp)
        
        link_status_fn = registered_tools["link_status"]
        
        # Mock disconnected state - link_healthy should be able to return False or None
        with patch("bugbuster_mcp.session.get_transport", return_value="usb"):
            with patch("bugbuster_mcp.session.get_port", return_value=None):
                with patch("bugbuster_mcp.session.link_healthy", return_value=None):
                    result = link_status_fn()
        
        # Should not crash and should indicate not healthy
        assert "healthy" in result
        # None is acceptable for "not yet connected", False for "connected but broken"
        assert result["healthy"] in (None, False)
    
    def test_link_status_never_returns_null_string(self):
        """link_status should never return the string 'null'."""
        from bugbuster_mcp.tools import discovery
        
        mock_mcp = Mock()
        registered_tools = {}
        
        def mock_tool_decorator():
            def decorator(fn):
                registered_tools[fn.__name__] = fn
                return fn
            return decorator
        
        mock_mcp.tool = mock_tool_decorator
        discovery.register(mock_mcp)
        
        link_status_fn = registered_tools["link_status"]
        
        with patch("bugbuster_mcp.session.get_transport", return_value="auto"):
            with patch("bugbuster_mcp.session.get_port", return_value=None):
                with patch("bugbuster_mcp.session.link_healthy", return_value=None):
                    result = link_status_fn()
        
        # Should return Python None, not string "null"
        if result["port"] is not None:
            assert isinstance(result["port"], str)
            assert result["port"] != "null"
        if result["healthy"] is not None:
            assert isinstance(result["healthy"], bool)
