"""
Webasto CAN Capture MCP Server

MCP server that records a vehicle CAN bus through an ESP32 GVRET (SavvyCAN-compatible)
sniffer and offers offline analysis of the captures.
"""

from .server import serve

__all__ = ["serve"]
