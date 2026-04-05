"""
BugBuster MCP Server — Entry point.

Usage:
    python -m bugbuster_mcp --transport usb --port /dev/ttyACM0
    python -m bugbuster_mcp --transport http --host 192.168.4.1

Install the server in Claude Code:
    Add to ~/.claude/settings.json → mcpServers:

    "bugbuster": {
        "command": "python3",
        "args": ["-m", "bugbuster_mcp", "--transport", "usb", "--port", "/dev/cu.usbmodemXXXXXX"]
    }

    Or with uv (installed via brew install uv):
    "bugbuster": {
        "command": "uv",
        "args": ["run", "--project", "/path/to/BugBuster/python",
                 "python", "-m", "bugbuster_mcp",
                 "--transport", "usb", "--port", "/dev/cu.usbmodemXXXXXX"]
    }
"""

import argparse
import logging


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="bugbuster_mcp",
        description="BugBuster MCP server — exposes hardware debugging tools to AI models.",
    )
    parser.add_argument(
        "--transport",
        choices=["usb", "http"],
        default="usb",
        help="Transport to use: 'usb' (binary BBP over CDC) or 'http' (WiFi REST API).",
    )
    parser.add_argument(
        "--port",
        default=None,
        help="USB serial port path (USB transport). "
             "Examples: /dev/ttyACM0, /dev/cu.usbmodemXXXXXX, COM3",
    )
    parser.add_argument(
        "--host",
        default="192.168.4.1",
        help="BugBuster IP address or hostname (HTTP transport). Default: 192.168.4.1",
    )
    parser.add_argument(
        "--vlogic",
        type=float,
        default=3.3,
        help=(
            "Logic-level voltage for all digital IOs in volts (default: 3.3). "
            "Valid range: 1.8-5.0 V. Set this once at startup — AI tools cannot change it. "
            "Examples: 1.8 (for 1.8 V MCUs), 3.3 (default), 5.0 (for 5 V logic)."
        ),
    )
    parser.add_argument(
        "--log-level",
        default="WARNING",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Logging verbosity (default: WARNING).",
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
    )

    # Validate vlogic before starting
    from .config import MIN_VLOGIC, MAX_VLOGIC
    if not (MIN_VLOGIC <= args.vlogic <= MAX_VLOGIC):
        parser.error(
            f"--vlogic {args.vlogic} V is outside the valid range "
            f"{MIN_VLOGIC}-{MAX_VLOGIC} V."
        )

    # Configure the session before starting the server
    from .session import configure
    configure(transport=args.transport, port=args.port, host=args.host, vlogic=args.vlogic)

    # Run the MCP server (stdio transport — standard for Claude Code integration)
    from .server import mcp
    mcp.run()


if __name__ == "__main__":
    main()
