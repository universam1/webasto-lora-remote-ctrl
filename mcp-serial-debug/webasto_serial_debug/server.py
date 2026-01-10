"""
MCP Server for Webasto Serial Device Debugging

Provides tools to connect, monitor, and interact with ESP32 devices over serial.
"""

import asyncio
import logging
import threading
import time
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
    """Manages serial device connections with background reading"""

    def __init__(self):
        self.connections: Dict[str, serial.Serial] = {}
        self.read_buffers: Dict[str, list] = {}
        self.buffer_locks: Dict[str, threading.Lock] = {}
        self.reader_threads: Dict[str, threading.Thread] = {}
        self.reader_running: Dict[str, bool] = {}
        self.buffer_max_lines = 2000

    def _reader_thread(self, device: str):
        """Background thread that continuously reads from serial port"""
        logger.info(f"[{device}] Reader thread started")
        while self.reader_running.get(device, False):
            try:
                ser = self.connections.get(device)
                if ser is None or not ser.is_open:
                    break
                
                if ser.in_waiting > 0:
                    line = ser.readline().decode('utf-8', errors='replace').strip()
                    if line:
                        with self.buffer_locks[device]:
                            self.read_buffers[device].append(line)
                            # Keep buffer size manageable
                            while len(self.read_buffers[device]) > self.buffer_max_lines:
                                self.read_buffers[device].pop(0)
                else:
                    time.sleep(0.01)  # Small sleep to avoid busy waiting
            except Exception as e:
                if self.reader_running.get(device, False):
                    logger.error(f"[{device}] Reader error: {e}")
                break
        logger.info(f"[{device}] Reader thread stopped")

    def list_devices(self) -> list[dict]:
        """List available Webasto devices"""
        import os
        devices = []
        for alias, path in DEVICE_ALIASES.items():
            try:
                # Check if device exists (symlink or direct path)
                exists = os.path.exists(path)
                
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

    def connect(self, device: str, baud_rate: int = 115200, timeout: float = 0.1) -> str:
        """Connect to a device and start background reader"""
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
            self.buffer_locks[device] = threading.Lock()
            
            # Start background reader thread
            self.reader_running[device] = True
            thread = threading.Thread(target=self._reader_thread, args=(device,), daemon=True)
            thread.start()
            self.reader_threads[device] = thread
            
            logger.info(f"Connected to {device} at {path} ({baud_rate} baud)")
            return f"Connected to {device} at {path} ({baud_rate} baud) - background reader started"
        except Exception as e:
            logger.error(f"Failed to connect to {device}: {e}")
            raise Exception(f"Failed to connect to {device}: {e}")

    def disconnect(self, device: str) -> str:
        """Disconnect from a device and stop background reader"""
        if device not in self.connections:
            return f"Not connected to {device}"

        try:
            # Stop background reader
            self.reader_running[device] = False
            if device in self.reader_threads:
                self.reader_threads[device].join(timeout=1.0)
                del self.reader_threads[device]
            
            self.connections[device].close()
            del self.connections[device]
            if device in self.read_buffers:
                del self.read_buffers[device]
            if device in self.buffer_locks:
                del self.buffer_locks[device]
            logger.info(f"Disconnected from {device}")
            return f"Disconnected from {device}"
        except Exception as e:
            logger.error(f"Error disconnecting from {device}: {e}")
            raise Exception(f"Error disconnecting from {device}: {e}")

    def read(self, device: str, lines: int = 50) -> str:
        """Read buffered data from device (background thread collects data)"""
        if device not in self.connections:
            raise Exception(f"Not connected to {device}")

        try:
            with self.buffer_locks[device]:
                buffer = self.read_buffers.get(device, [])
                recent = buffer[-lines:] if len(buffer) > lines else buffer[:]
            
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
            with self.buffer_locks.get(device, threading.Lock()):
                buffer_lines = len(self.read_buffers.get(device, []))
            status.update({
                "baud_rate": ser.baudrate,
                "timeout": ser.timeout,
                "in_waiting": ser.in_waiting,
                "buffer_lines": buffer_lines,
                "reader_active": self.reader_running.get(device, False),
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
