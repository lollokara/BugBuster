"""
BugBuster MCP — Tool registration.

Each module registers its tools against the FastMCP instance.
"""


def register_all(mcp) -> None:
    """Register all tool groups with the MCP server."""
    from .discovery import register as reg_discovery
    from .io_config  import register as reg_io_config
    from .analog     import register as reg_analog
    from .digital    import register as reg_digital
    from .waveform   import register as reg_waveform
    from .debug      import register as reg_debug
    from .bus        import register as reg_bus
    from .power      import register as reg_power
    from .advanced   import register as reg_advanced

    reg_discovery(mcp)
    reg_io_config(mcp)
    reg_analog(mcp)
    reg_digital(mcp)
    reg_waveform(mcp)
    reg_debug(mcp)
    reg_bus(mcp)
    reg_power(mcp)
    reg_advanced(mcp)
