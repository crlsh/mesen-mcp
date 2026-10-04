# Mesen MCP

Mesen MCP is an AI-augmented fork of Mesen (Multi-system emulator for Windows, Linux, and macOS supporting NES, SNES, GB/GBC/GBA, PC Engine, SMS/GG, and WonderSwan). 

It embeds a native, multi-threaded Model Context Protocol (MCP) TCP Server directly into the core emulation engine, enabling Large Language Models (like Claude via Claude Desktop or Cursor) to programmatically inspect memory, execute game inputs, control emulation speed, set hardware breakpoints, and automate reverse engineering tasks in real time.

## Key Features & Built-in MCP Tools

The native MCP server accepts JSON-RPC over TCP/stdio with thread-safe dual-path execution (core thread + direct TCP thread queries).

### 1. Emulation & Flow Control
* `load_rom`: Load and initialize any compatible ROM file.
* `run` / `pause` / `reset`: Control the emulator state without losing debug context.
* `set_speed`: Change emulation speed from `0%` (unlocked maximum speed) up to `5000%`.
* `get_state`: Query emulator status (`idle`, `loading`, `running`, `paused`, `error`), Program Counter (PC), and current frame counter.

### 2. Frame Step & Input Automation
* `step_frame`: Advance emulation by an exact number of frames (1 to 3600).
* `set_input`: Inject button presses via bitmask (D-Pad, A, B, Select, Start) with a persistent Sticky Input mechanism that survives frame polling cycles.

### 3. Memory Read / Write (RAM & VRAM)
* `read_memory`: Read memory buffers up to 2048 bytes across various memory spaces:
  * CPU Memory (`NesMemory`, `cpu`)
  * PPU / Video Memory (`ppuMemory`, `nametableRam`, `paletteRam`, `spriteRam`/OAM, `chrRam`)
* `write_memory`: Directly write byte values (0–255) into RAM for testing, cheat creation, and dynamic state modifications.

### 4. Advanced CPU Debugging & Disassembly
* `set_breakpoints`: Set conditional breakpoints by type: Read (`1`), Write (`2`), or Execute (`4`).
* `step_instruction`: Step through execution by exact CPU instruction count.
* `continue`: Resume execution until the next breakpoint hit.
* `get_cpu_state`: Full register dump (NES: `A`, `X`, `Y`, `PC`, `SP`, `PS`, cycles, and boolean flags `C`, `Z`, `I`, `D`, `V`, `N`).
* `get_trace`: Fetch recent execution traces with disassembled instructions and raw bytecode.
* `get_callstack`: Inspect call stacks, subroutines, return addresses, and `NMI`/`IRQ` interrupts.

### 5. AI-Assisted Reverse Engineering Tools
* `scan_variables_internal`: Automated RAM variable search. Performs multi-trial input injections and calculates deltas across the entire internal RAM to find candidate addresses (e.g., player position, lives, score) categorized by stability.
* `set_write_log` / `get_write_log`: Ring-buffered memory write logger tracking exact CPU cycles, target addresses, written values, and the PC responsible for the write.


## Releases

The latest stable version is available from the [releases page on GitHub](https://github.com/nesdev-org/MesenCE/releases).

## Development Builds

[![Mesen](https://github.com/nesdev-org/MesenCE/actions/workflows/build.yml/badge.svg)](https://github.com/nesdev-org/MesenCE/actions/workflows/build.yml?query=branch%3Amaster)

* [Windows](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28Windows%20-%20net10.0%20-%20AoT%29.zip)
  * Windows 7 or higher is required. Windows 7 users must use SP1 and have all updates installed.
* [Linux x64](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28Linux%20-%20ubuntu-22.04%20-%20clang_aot%29.zip)  (requires **SDL2**)  
* [Linux ARM64](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28Linux%20-%20ubuntu-22.04-arm%20-%20clang_aot%29.zip)  (requires **SDL2**)  
* [macOS - Intel](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28macOS%20-%20macos-15-intel%20-%20clang_aot%29.zip)  (requires **SDL2**)  
* [macOS - Apple Silicon](https://nightly.link/nesdev-org/MesenCE/workflows/build/master/Mesen%20%28macOS%20-%20macos-15%20-%20clang_aot%29.zip)  (requires **SDL2**)  

#### <ins>Notes</ins> ####

* Other builds are also available in the [Actions](https://github.com/nesdev-org/MesenCE/actions/workflows/build.yml?query=branch%3Amaster) tab.
* **macOS**: Builds are self-signed and will require approval via Gatekeeper before they are able to be run.  
* **SteamOS**: See [SteamOS.md](SteamOS.md)  

## Compiling

See [COMPILING.md](COMPILING.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md)

## License

Mesen is available under the GPL V3 license.  Full text here: <http://www.gnu.org/licenses/gpl-3.0.en.html>

Copyright (C) 2014-2026 Sour, 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
