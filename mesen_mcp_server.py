import asyncio
import socket
import json
import os
from mcp.server import Server
from mcp.types import Tool, TextContent, ImageContent

# Configuration
MESEN_IP = "127.0.0.1"
MESEN_PORT = 12345

class MesenClient:
    def __init__(self, ip, port):
        self.ip = ip
        self.port = port
        self.reader = None
        self.writer = None

    async def connect(self):
        try:
            self.reader, self.writer = await asyncio.open_connection(self.ip, self.port)
            print(f"Connected to Mesen at {self.ip}:{self.port}")
            return True
        except Exception as e:
            print(f"Failed to connect to Mesen: {e}")
            return False

    async def send_command(self, command):
        if not self.writer:
            if not await self.connect():
                return "Error: Not connected to Mesen"
        
        try:
            self.writer.write((command + "\n").encode())
            await self.writer.drain()
            
            line = await self.reader.readline()
            return line.decode().strip()
        except Exception as e:
            self.writer = None
            return f"Error: {e}"

mesen = MesenClient(MESEN_IP, MESEN_PORT)
app = Server("mesen-mcp")

@app.list_tools()
async def list_tools():
    return [
        Tool(
            name="execute_lua",
            description="Execute arbitrary Lua code in Mesen2 and return result",
            inputSchema={
                "type": "object",
                "properties": {
                    "code": {"type": "string", "description": "Lua code to execute, e.g. 'emu.read(0, 0, false)'"}
                },
                "required": ["code"]
            }
        ),
        Tool(
            name="read_memory",
            description="Read 8-bit memory from the emulator",
            inputSchema={
                "type": "object",
                "properties": {
                    "address": {"type": "integer"},
                    "mem_type": {"type": "integer", "description": "Enum value for MemoryType"},
                    "signed": {"type": "boolean", "default": False}
                },
                "required": ["address", "mem_type"]
            }
        ),
        Tool(
            name="write_memory",
            description="Write 8-bit memory to the emulator",
            inputSchema={
                "type": "object",
                "properties": {
                    "address": {"type": "integer"},
                    "value": {"type": "integer"},
                    "mem_type": {"type": "integer", "description": "Enum value for MemoryType"}
                },
                "required": ["address", "value", "mem_type"]
            }
        ),
        Tool(
            name="step",
            description="Step execution",
            inputSchema={
                "type": "object",
                "properties": {
                    "cpu_type": {"type": "integer", "description": "Enum value for CpuType"},
                    "step_type": {"type": "integer", "description": "Enum value for StepType"},
                    "count": {"type": "integer", "default": 1}
                },
                "required": ["cpu_type", "step_type"]
            }
        ),
        Tool(
            name="get_state",
            description="Get full emulator state as a serialized Lua table string",
            inputSchema={"type": "object", "properties": {}}
        ),
        Tool(
            name="get_rom_info",
            description="Get information about the loaded ROM",
            inputSchema={"type": "object", "properties": {}}
        ),
        Tool(
            name="get_enums",
            description="Get all Mesen2 Lua enums (MemoryType, CpuType, etc.)",
            inputSchema={"type": "object", "properties": {}}
        ),
        Tool(
            name="take_screenshot",
            description="Take a screenshot and return as raw data string",
            inputSchema={"type": "object", "properties": {}}
        )
    ]

@app.call_tool()
async def call_tool(name: str, arguments: dict):
    if name == "execute_lua":
        res = await mesen.send_command(arguments["code"])
        return [TextContent(type="text", text=res)]

    elif name == "read_memory":
        addr = arguments["address"]
        mtype = arguments["mem_type"]
        signed = "true" if arguments.get("signed") else "false"
        cmd = f"emu.read({addr}, {mtype}, {signed})"
        res = await mesen.send_command(cmd)
        return [TextContent(type="text", text=res)]
    
    elif name == "write_memory":
        addr = arguments["address"]
        val = arguments["value"]
        mtype = arguments["mem_type"]
        cmd = f"emu.write({addr}, {val}, {mtype})"
        res = await mesen.send_command(cmd)
        return [TextContent(type="text", text=res)]

    elif name == "step":
        cpu = arguments["cpu_type"]
        stype = arguments["step_type"]
        count = arguments.get("count", 1)
        cmd = f"emu.step({cpu}, {stype}, {count})"
        res = await mesen.send_command(cmd)
        return [TextContent(type="text", text=res)]

    elif name == "get_state":
        cmd = "emu.getState()"
        res = await mesen.send_command(cmd)
        return [TextContent(type="text", text=res)]

    elif name == "get_rom_info":
        cmd = "emu.getRomInfo()"
        res = await mesen.send_command(cmd)
        return [TextContent(type="text", text=res)]

    elif name == "get_enums":
        # Returns a giant table with all enums
        cmd = "{memType=emu.memType, callbackType=emu.callbackType, cheatType=emu.cheatType, counterType=emu.counterType, cpuType=emu.cpuType, drawSurface=emu.drawSurface, eventType=emu.eventType, stepType=emu.stepType}"
        res = await mesen.send_command(cmd)
        return [TextContent(type="text", text=res)]

    elif name == "take_screenshot":
        cmd = "emu.takeScreenshot()"
        res = await mesen.send_command(cmd)
        return [TextContent(type="text", text=res)]

    return [TextContent(type="text", text=f"Unknown tool: {name}")]

if __name__ == "__main__":
    from mcp.server.stdio import stdio_server
    async def main():
        async with stdio_server() as (read_stream, write_stream):
            await app.run(read_stream, write_stream, app.create_initialization_options())
    
    asyncio.run(main())
