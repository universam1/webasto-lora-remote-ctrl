"""
MCP Server for Webasto Serial Device Debugging

Provides tools to connect, monitor, and interact with ESP32 devices over serial.
"""

import asyncio
import logging
from typing import Dict, Optional
from contextlib import asynccontextmanager

import serial
from serial.tools import list_ports
from mcp.server import Server
from mcp.types import Tool, TextContent

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("webasto-serial-debug")

# Device aliases
DEVICE_ALIASES = {
    "simulator": "/dev/ttyWEBASTOSIM",
    "sender": "/dev/ttyWEBASTOSEND",
    "receiver": "/dev/ttyWEBASTORECV",
}

# Active serial connections
connections: Dict[str, serial.Serial] = {}


class SerialDeviceManager:
    """Manages serial device connections"""

    def __init__(self):
        self.connections: Dict[str, serial.Serial] = {}
        self.read_buffers: Dict[str, list] = {}
        self.buffer_max_lines = 1000

    def list_devices(self) -> list[dict]:
        """List available Webasto devices"""
        devices = []
        for alias, path in DEVICE_ALIASES.items():
            try:
                # Check if device exists
                ports = [p.device for p in list_ports.comports()]
                exists = path in ports or any(p.device for p in list_ports.comports() if p.device == path)
                
                connected = alias in self.connections and self.connections[alias].is_open
                devices.append({
                    "alias": alias,
                    "path": path,
                    "exists": exists,
                    "connected": connected,
                })
            except Exception as e:
                logger.error(f"Error checking device {alias}: {e}")
                devices.append({
                    "alias": alias,
                    "path": path,
                    "exists": False,
                    "connected": False,
                    "error": str(e)
                })
        return devices

    def connect(self, device: str, baud_rate: int = 115200, timeout: float = 1.0) -> str:
        """Connect to a device"""
        if device in self.connections and self.connections[device].is_open:
            return f"Already connected to {device}"

        path = DEVICE_ALIASES.get(device, device)
        
        try:
            ser = serial.Serial(
                port=path,
                baudrate=baud_rate,
                timeout=timeout,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
            )
            self.connections[device] = ser
            self.read_buffers[device] = []
            logger.info(f"Connected to {device} at {path} ({baud_rate} baud)")
            return f"Connected to {device} at {path} ({baud_rate} baud)"
        except Exception as e:
            logger.error(f"Failed to connect to {device}: {e}")
            raise Exception(f"Failed to connect to {device}: {e}")

    def disconnect(self, device: str) -> str:
        """Disconnect from a device"""
        if device not in self.connections:
            return f"Not connected to {device}"

        try:
            self.connections[device].close()
            del self.connections[device]
            if device in self.read_buffers:
                del self.read_buffers[device]
            logger.info(f"Disconnected from {device}")
            return f"Disconnected from {device}"
        except Exception as e:
            logger.error(f"Error disconnecting from {device}: {e}")
            raise Exception(f"Error disconnecting from {device}: {e}")

    def read(self, device: str, lines: int = 50) -> str:
        """Read available data from device"""
        if device not in self.connections:
            raise Exception(f"Not connected to {device}")

        ser = self.connections[device]
        try:
            # Read all available data
            while ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if line:
                    self.read_buffers[device].append(line)
                    # Keep buffer size manageable
                    if len(self.read_buffers[device]) > self.buffer_max_lines:
                        self.read_buffers[device].pop(0)

            # Return last N lines
            buffer = self.read_buffers.get(device, [])
            recent = buffer[-lines:] if len(buffer) > lines else buffer
            
            if not recent:
                return f"No data available from {device}"
            
            return "\n".join(recent)
        except Exception as e:
            logger.error(f"Error reading from {device}: {e}")
            raise Exception(f"Error reading from {device}: {e}")

    def write(self, device: str, data: str) -> str:
        """Write data to device"""
        if device not in self.connections:
            raise Exception(f"Not connected to {device}")

        ser = self.connections[device]
        try:
            # Ensure newline at end
            if not data.endswith('\n'):
                data += '\n'
            
            written = ser.write(data.encode('utf-8'))
            ser.flush()
            logger.info(f"Wrote {written} bytes to {device}")
            return f"Wrote {written} bytes to {device}"
        except Exception as e:
            logger.error(f"Error writing to {device}: {e}")
            raise Exception(f"Error writing to {device}: {e}")

    def get_status(self, device: str) -> dict:
        """Get device status"""
        path = DEVICE_ALIASES.get(device, device)
        connected = device in self.connections and self.connections[device].is_open
        
        status = {
            "device": device,
            "path": path,
            "connected": connected,
        }

        if connected:
            ser = self.connections[device]
            status.update({
                "baud_rate": ser.baudrate,
                "timeout": ser.timeout,
                "in_waiting": ser.in_waiting,
                "buffer_lines": len(self.read_buffers.get(device, [])),
            })

        return status


# Global device manager
device_manager = SerialDeviceManager()


async def serve():
    """Run the MCP server"""
    server = Server("webasto-serial-debug")

    @server.list_tools()
    async def list_tools() -> list[Tool]:
        """List available tools"""
        return [
            Tool(
                name="list_devices",
                description="List available Webasto serial devices (simulator, sender, receiver)",
                inputSchema={
                    "type": "object",
                    "properties": {},
                    "required": [],
                },
            ),
            Tool(
                name="connect_device",
                description="Connect to a Webasto device for serial communication",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "device": {
                            "type": "string",
                            "description": "Device alias (simulator, sender, receiver) or full path",
                            "enum": ["simulator", "sender", "receiver"],
                        },
                        "baud_rate": {
                            "type": "integer",
                            "description": "Baud rate (default: 115200)",
                            "default": 115200,
                        },
                    },
                    "required": ["device"],
                },
            ),
            Tool(
                name="disconnect_device",
                description="Disconnect from a Webasto device",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "device": {
                            "type": "string",
                            "description": "Device alias (simulator, sender, receiver)",
                            "enum": ["simulator", "sender", "receiver"],
                        },
                    },
                    "required": ["device"],
                },
            ),
            Tool(
                name="read_serial",
                description="Read serial output from a connected device (returns last N lines from buffer)",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "device": {
                            "type": "string",
                            "description": "Device alias (simulator, sender, receiver)",
                            "enum": ["simulator", "sender", "receiver"],
                        },
                        "lines": {
                            "type": "integer",
                            "description": "Number of lines to return (default: 50)",
                            "default": 50,
                        },
                    },
                    "required": ["device"],
                },
            ),
            Tool(
                name="write_serial",
                description="Write data to a connected device",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "device": {
                            "type": "string",
                            "description": "Device alias (simulator, sender, receiver)",
                            "enum": ["simulator", "sender", "receiver"],
                        },
                        "data": {
                            "type": "string",
                            "description": "Data to send (newline will be appended automatically)",
                        },
                    },
                    "required": ["device", "data"],
                },
            ),
            Tool(
                name="get_device_status",
                description="Get connection status and configuration for a device",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "device": {
                            "type": "string",
                            "description": "Device alias (simulator, sender, receiver)",
                            "enum": ["simulator", "sender", "receiver"],
                        },
                    },
                    "required": ["device"],
                },
            ),
        ]

    @server.call_tool()
    async def call_tool(name: str, arguments: dict) -> list[TextContent]:
        """Handle tool calls"""
        try:
            if name == "list_devices":
                devices = device_manager.list_devices()
                result = "Available Webasto Devices:\n\n"
                for dev in devices:
                    status = "🟢 Connected" if dev.get("connected") else ("🟡 Available" if dev.get("exists") else "🔴 Not Found")
                    result += f"- {dev['alias']}: {dev['path']} - {status}\n"
                return [TextContent(type="text", text=result)]

            elif name == "connect_device":
                device = arguments["device"]
                baud_rate = arguments.get("baud_rate", 115200)
                result = device_manager.connect(device, baud_rate)
                return [TextContent(type="text", text=result)]

            elif name == "disconnect_device":
                device = arguments["device"]
                result = device_manager.disconnect(device)
                return [TextContent(type="text", text=result)]

            elif name == "read_serial":
                device = arguments["device"]
                lines = arguments.get("lines", 50)
                result = device_manager.read(device, lines)
                return [TextContent(type="text", text=f"=== {device} serial output ===\n{result}")]

            elif name == "write_serial":
                device = arguments["device"]
                data = arguments["data"]
                result = device_manager.write(device, data)
                return [TextContent(type="text", text=result)]

            elif name == "get_device_status":
                device = arguments["device"]
                status = device_manager.get_status(device)
                result = f"Device Status for '{device}':\n"
                for key, value in status.items():
                    result += f"  {key}: {value}\n"
                return [TextContent(type="text", text=result)]

            else:
                raise ValueError(f"Unknown tool: {name}")

        except Exception as e:
            logger.error(f"Error in tool {name}: {e}")
            return [TextContent(type="text", text=f"Error: {str(e)}")]

    # Run the server
    from mcp.server.stdio import stdio_server

    async with stdio_server() as (read_stream, write_stream):
        logger.info("Webasto Serial Debug MCP Server starting...")
        await server.run(read_stream, write_stream, server.create_initialization_options())
