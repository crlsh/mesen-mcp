# Mesen2 (MCP-Enabled Fork)

This repository is a personal fork of **Mesen2**, a multi-system emulator
(NES, SNES, Game Boy, Game Boy Advance, PC Engine, SMS/Game Gear, WonderSwan)
for Windows, Linux and macOS.

This fork integrates a native **MCP server** directly into the emulator core
to enable programmatic control without relying on external bridge scripts.

---

## About This Fork

This version replaces the previous external Python/Lua bridge with a
native MCP server implementation inside the emulator core:

- `Core/Shared/McpServer.cpp`
- `Core/Shared/McpServer.h`

The goal is to allow direct integration with external tooling and LLM-driven
control systems in a deterministic and reproducible way.

---

## Compiling

See `COMPILING.md` for platform-specific instructions.

### Requirements

### Windows
- Visual Studio 2022
- x64 toolset

### Linux
- Clang or GCC with C++17 support
- SDL2
- .NET 8 SDK

### macOS
- Clang with C++17 support
- SDL2
- .NET 8 SDK

---

## Build

### Windows

msbuild Core/Core.vcxproj /p:Configuration=Release /p:Platform=x64

shell
Copy code

### Linux / macOS

make

yaml
Copy code

---

## Project Structure

- `Core/` – Emulator core
- `InteropDLL/` – Interop layer
- `Core/Shared/McpServer.*` – Native MCP server integration

---

## License

This project is based on Mesen2 and remains licensed under the GPL v3.

Original project copyright:
Copyright (C) 2014–2025 Sour

This fork maintains GPL compliance and distributes modifications
under the same license terms.

GPL v3 full text:
http://www.gnu.org/licenses/gpl-3.0.en.html