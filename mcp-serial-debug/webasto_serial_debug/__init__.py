"""
Webasto Serial Debug MCP Server

MCP server for debugging ESP32 Webasto devices over serial TTY connections.
"""

from .server import serve

__all__ = ["serve"]
