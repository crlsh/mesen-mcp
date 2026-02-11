# Mesen2 MCP Bridge & Server

This setup allows an LLM (via the Model Context Protocol) to control and analyze Mesen2.

## Components

1.  **Lua Bridge (`mesen_mcp_bridge.lua`)**: A script that runs inside Mesen2. It opens a TCP socket on port 12345 and listens for Lua commands.
2.  **MCP Server (`mesen_mcp_server.py`)**: An external Python server that implements the MCP protocol and communicates with the Lua bridge.

## Setup Instructions

### 1. Mesen2 Configuration
- Open Mesen2.
- Go to **Settings -> Script Settings** (or similar, depending on the version).
- Ensure the following are **ENABLED**:
    - **Allow access to I/O and OS functions** (Required for `luasocket`).
    - **Allow network access** (Required for the TCP server).
- Open the **Script Window** (Tools -> Script Window).
- Load and run `mesen_mcp_bridge.lua`.
- You should see "MCP Bridge listening on port 12345" in the script log.

### 2. MCP Server Setup
- Ensure Python 3 is installed.
- Install the MCP library:
    ```bash
    pip install mcp
    ```
- The server is designed to be run as an MCP stdio server. You can add it to your LLM client's configuration (e.g., Claude Desktop, Gemini CLI, etc.).

**Example Configuration (Claude Desktop):**
```json
{
  "mcpServers": {
    "mesen2": {
      "command": "python",
      "args": ["C:/repos/Mesen2/mesen_mcp_server.py"]
    }
  }
}
```

## Available Tools

- `execute_lua(code)`: Run any arbitrary Lua code (e.g., `emu.log("Hello")`).
- `read_memory(address, mem_type, signed)`: Read 8-bit memory.
- `write_memory(address, value, mem_type)`: Write 8-bit memory.
- `step(cpu_type, step_type, count)`: Step the emulation.
- `get_state()`: Get the full emulator state.
- `get_rom_info()`: Get ROM information.
- `get_enums()`: Get all enum values (crucial for knowing what `mem_type` or `cpu_type` to use).
- `take_screenshot()`: Get a screenshot.

## Technical Details

- The bridge uses a non-blocking TCP server that checks for commands at the end of every frame (`emu.eventType.endFrame`).
- Commands are sent as raw Lua strings and executed using `load()`.
- Results are serialized into a Lua-table-like string format for return.
