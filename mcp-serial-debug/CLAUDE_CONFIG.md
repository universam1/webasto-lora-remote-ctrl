# Claude Desktop Configuration

Add this MCP server to your Claude Desktop configuration file.

## Configuration File Location

- **macOS**: `~/Library/Application Support/Claude/claude_desktop_config.json`
- **Linux**: `~/.config/Claude/claude_desktop_config.json`
- **Windows**: `%APPDATA%\Claude\claude_desktop_config.json`

## Configuration

```json
{
  "mcpServers": {
    "webasto-serial-debug": {
      "command": "/home/sam/src/webasto-lora-remote-ctrl/mcp-serial-debug/venv/bin/python",
      "args": ["-m", "webasto_serial_debug"]
    }
  }
}
```

**Important**: Update the path to match your installation location.

## Verify Installation

1. Restart Claude Desktop after adding the configuration
2. Look for the MCP server tools in the tools menu
3. Available tools:
   - `list_devices` - List Webasto devices
   - `connect_device` - Connect to a device
   - `disconnect_device` - Disconnect from a device
   - `read_serial` - Read serial output
   - `write_serial` - Send data to device
   - `get_device_status` - Get device status

## Usage Example

```
Please list the available Webasto devices
```

```
Connect to the simulator device
```

```
Read the last 100 lines from the simulator
```

## Troubleshooting

- Make sure the virtual environment path is correct
- Ensure the devices have proper permissions (udev rules installed)
- Check Claude Desktop logs for any errors
