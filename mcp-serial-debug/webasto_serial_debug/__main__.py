"""Entry point: python -m webasto_serial_debug (stdio MCP server)."""

import asyncio


def main():
    from .server import serve

    asyncio.run(serve())


if __name__ == "__main__":
    main()
