#pragma once

#include "pch.h"

class Emulator;

struct McpFramebufferSnapshot
{
	uint32_t Frame = 0;
	uint32_t Width = 0;
	uint32_t Height = 0;
	std::vector<uint8_t> Data;
	std::string Hash;
};

class McpFramebuffer
{
public:
	static bool CaptureNes(Emulator* emu, McpFramebufferSnapshot& snapshot, std::string& error);
};
