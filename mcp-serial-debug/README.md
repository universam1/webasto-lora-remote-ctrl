# Webasto Serial Debug MCP Server

Model Context Protocol (MCP) server for debugging ESP32 Webasto devices over serial TTY connections.

## Features

- **List Devices**: Discover available Webasto devices (`/dev/ttyWEBASTOSIM`, `/dev/ttyWEBASTOSEND`, `/dev/ttyWEBASTORECV`)
- **Connect & Monitor**: Connect to devices and read serial output in real-time
- **Send Commands**: Send data/commands to connected devices
- **Multi-Device Support**: Monitor multiple devices simultaneously
- **Status Monitoring**: Check device connection status, baud rate, and buffer information

## Installation

```bash
cd mcp-serial-debug
pip install -e .
```

## Usage

### With Claude Desktop

Add to your Claude Desktop configuration (`~/Library/Application Support/Claude/claude_desktop_config.json` on macOS):

```json
{
  "mcpServers": {
    "webasto-serial-debug": {
      "command": "python",
      "args": ["-m", "webasto_serial_debug"],
      "cwd": "/home/sam/src/webasto-lora-remote-ctrl/mcp-serial-debug"
    }
  }
}
```

### Available Tools

1. **list_devices**: List available Webasto serial devices
2. **connect_device**: Connect to a specific device
3. **disconnect_device**: Disconnect from a device
4. **read_serial**: Read output from a connected device
5. **write_serial**: Send data to a connected device
6. **get_device_status**: Get connection status and configuration

## Requirements

- Python 3.10+
- pyserial
- mcp library

## License

Same as parent project (Webasto LoRa Remote Control)
