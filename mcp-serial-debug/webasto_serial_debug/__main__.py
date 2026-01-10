"""Main entry point for the MCP server"""

import asyncio
from .server import serve


def main():
    """Run the MCP server"""
    asyncio.run(serve())


if __name__ == "__main__":
    main()
