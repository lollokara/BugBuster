"""
BugBuster MCP Server
====================

An MCP (Model Context Protocol) server that exposes the BugBuster
hardware debugging platform to AI models.

Usage:
    python -m bugbuster_mcp --transport usb --port /dev/ttyACM0
    python -m bugbuster_mcp --transport http --host 192.168.4.1
"""

__version__ = "0.1.0"
