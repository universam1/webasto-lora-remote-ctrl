"""Start the server as a subprocess over stdio and list its tools (no hardware involved)."""

import asyncio
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

EXPECTED = {
    "list_devices", "attach", "detach", "detach_all", "read_log", "send_command", "reset_device", "health",
    "ble_scan", "ble_pair", "ble_unpair", "ble_read", "ble_command", "ble_watch",
}


def test_server_lists_tools_over_stdio():
    async def run():
        params = StdioServerParameters(command=sys.executable, args=["-m", "webasto_serial_debug"])
        async with stdio_client(params) as (r, w):
            async with ClientSession(r, w) as s:
                await asyncio.wait_for(s.initialize(), 20)
                tools = await s.list_tools()
                names = {t.name for t in tools.tools}
                res = await s.call_tool("detach_all", {})
                return names, res.content[0].text

    names, text = asyncio.run(run())
    assert EXPECTED <= names
    assert text == "nothing attached"
