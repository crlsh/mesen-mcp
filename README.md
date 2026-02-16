Mesen2 (MCP-Enabled Fork)

This repository is a personal fork of Mesen2, a multi-system emulator
(NES, SNES, Game Boy, Game Boy Advance, PC Engine, SMS/Game Gear, WonderSwan)
for Windows, Linux and macOS.

This fork integrates a native MCP server directly into the emulator core
to enable programmatic control without relying on external bridge scripts.

This is not an official Mesen release.

About This Fork

This version replaces the previous external Python/Lua bridge with a
native MCP server implementation inside the emulator core.

Modified / Added components:

Core/Shared/McpServer.cpp

Core/Shared/McpServer.h

The objective is to allow deterministic, direct programmatic control of the
emulator from external tooling, including automation systems and LLM-driven workflows.

Requirements
Windows

Visual Studio 2022

MSVC v143 toolset

.NET 8 SDK

x64 build tools

Linux

Clang or GCC with C++17 support

SDL2

.NET 8 SDK

macOS

Clang with C++17 support

SDL2

.NET 8 SDK

Build
Windows
dotnet restore Mesen.sln
msbuild Mesen.sln /p:Configuration=Release /p:Platform=x64

Linux / macOS
make

Continuous Integration

This repository includes a minimal GitHub Actions workflow that validates
that the solution builds successfully on a clean Windows environment.

Only the latest push is built; previous runs are automatically canceled.

Project Structure

Core/ — Emulator core

InteropDLL/ — Native interop layer

UI/ — .NET-based user interface

Core/Shared/McpServer.* — Native MCP server integration

License

This project is based on Mesen2 and remains licensed under the GPL v3.

Original project copyright:
Copyright (C) 2014–2025 Sour

This fork maintains GPL compliance and distributes modifications
under the same license terms.

Full license text:
http://www.gnu.org/licenses/gpl-3.0.en.html