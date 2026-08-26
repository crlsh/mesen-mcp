#pragma once

#include "pch.h"

class McpFm2Input
{
public:
	static bool Load(const std::string& path, std::vector<int>& buttons, std::string& error);
};
