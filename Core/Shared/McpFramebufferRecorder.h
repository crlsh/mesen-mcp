#pragma once

#include "pch.h"
#include <fstream>
#include <mutex>

struct McpFramebufferRecorderStatus
{
	bool Enabled = false;
	uint64_t Frames = 0;
	bool HasLastFrame = false;
	uint32_t LastFrame = 0;
	uint64_t WriteFailures = 0;
};

class McpFramebufferRecorder
{
private:
	std::mutex _mutex;
	std::ofstream _output;
	bool _enabled = false;
	uint64_t _frames = 0;
	bool _hasLastFrame = false;
	uint32_t _lastFrame = 0;
	uint64_t _writeFailures = 0;

	McpFramebufferRecorder() = default;

public:
	McpFramebufferRecorder(const McpFramebufferRecorder&) = delete;
	McpFramebufferRecorder& operator=(const McpFramebufferRecorder&) = delete;

	static McpFramebufferRecorder& Instance();
	bool Start(const std::string& path, std::string& error);
	McpFramebufferRecorderStatus Stop();
	McpFramebufferRecorderStatus GetStatus();
	void Record(uint32_t frame, const uint16_t* pixels);
};
