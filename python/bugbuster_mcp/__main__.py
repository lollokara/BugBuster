"""
BugBuster MCP Server — Entry point.

Usage:
    python -m bugbuster_mcp                       # auto-detect board over USB
    python -m bugbuster_mcp --transport usb --port COM6
    python -m bugbuster_mcp --transport http --host 192.168.4.1

Transport and port default to auto-detection, so no editor config needs a
hardcoded COM port. Override with --transport/--port or the environment
variables BUGBUSTER_TRANSPORT / BUGBUSTER_PORT / BUGBUSTER_HOST.

Install the server in Claude Code:
    Add to ~/.claude/settings.json → mcpServers:

    "bugbuster": {
        "command": "python3",
        "args": ["-m", "bugbuster_mcp"]
    }
"""

import argparse
import logging
import os


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="bugbuster_mcp",
        description="BugBuster MCP server — exposes hardware debugging tools to AI models.",
    )
    parser.add_argument(
        "--transport",
        choices=["auto", "usb", "http"],
        default=os.environ.get("BUGBUSTER_TRANSPORT", "auto"),
        help="Transport to use. 'auto' (default) picks USB when a board is "
             "attached and falls back to HTTP; 'usb' = binary BBP over CDC; "
             "'http' = WiFi REST API. Env: BUGBUSTER_TRANSPORT.",
    )
    parser.add_argument(
        "--port",
        default=os.environ.get("BUGBUSTER_PORT"),
        help="USB serial port (USB transport). Omit or pass 'auto' to detect "
             "the board automatically by USB descriptor + BBP handshake. "
             "Examples: COM6, /dev/ttyACM0, /dev/cu.usbmodemXXXXXX. "
             "Env: BUGBUSTER_PORT.",
    )
    parser.add_argument(
        "--host",
        default=os.environ.get("BUGBUSTER_HOST", "192.168.4.1"),
        help="BugBuster IP address or hostname (HTTP transport). "
             "Default: 192.168.4.1. Env: BUGBUSTER_HOST.",
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
        "--admin-token",
        default=os.environ.get("BUGBUSTER_ADMIN_TOKEN"),
        help=(
            "Admin token for BugBuster HTTP mutating endpoints. "
            "May also be supplied via BUGBUSTER_ADMIN_TOKEN. Required for HTTP writes."
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
    configure(
        transport=args.transport,
        port=args.port,
        host=args.host,
        vlogic=args.vlogic,
        admin_token=args.admin_token,
    )

    # Run the MCP server (stdio transport — standard for Claude Code integration)
    from .server import mcp
    mcp.run()


if __name__ == "__main__":
    main()
