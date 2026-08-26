#include "pch.h"
#include "Shared/McpFramebufferRecorder.h"
#include "Shared/McpFramebuffer.h"

#include <filesystem>

McpFramebufferRecorder& McpFramebufferRecorder::Instance()
{
	static McpFramebufferRecorder recorder;
	return recorder;
}

bool McpFramebufferRecorder::Start(const std::string& path, std::string& error)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if(_enabled) {
		error = "framebuffer capture already active";
		return false;
	}
	if(_output.is_open()) _output.close();
	_output.clear();
	std::filesystem::path outputPath = std::filesystem::u8path(path);
	if(path.empty() || !outputPath.is_absolute()) {
		error = "path must be absolute";
		return false;
	}
	if(!outputPath.has_parent_path() || !std::filesystem::is_directory(outputPath.parent_path())) {
		error = "output directory does not exist";
		return false;
	}
	_output.open(outputPath, std::ios::out | std::ios::trunc | std::ios::binary);
	if(!_output.is_open()) {
		error = "unable to open framebuffer capture path";
		return false;
	}
	_frames = 0;
	_hasLastFrame = false;
	_lastFrame = 0;
	_writeFailures = 0;
	_enabled = true;
	return true;
}

McpFramebufferRecorderStatus McpFramebufferRecorder::Stop()
{
	std::lock_guard<std::mutex> lock(_mutex);
	_enabled = false;
	if(_output.is_open()) {
		_output.flush();
		_output.close();
	}
	_output.clear();
	return {_enabled, _frames, _hasLastFrame, _lastFrame, _writeFailures};
}

McpFramebufferRecorderStatus McpFramebufferRecorder::GetStatus()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return {_enabled, _frames, _hasLastFrame, _lastFrame, _writeFailures};
}

void McpFramebufferRecorder::Record(uint32_t frame, const uint16_t* pixels)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if(!_enabled || !pixels) return;

	McpFramebufferSnapshot snapshot;
	McpFramebuffer::CaptureNesPixels(pixels, frame, snapshot);
	_output << "{\"frame\":" << frame << ",\"fbHash\":\"" << snapshot.Hash << "\"}\n";
	if(!_output.good()) {
		_writeFailures++;
		_enabled = false;
		_output.close();
		_output.clear();
		return;
	}
	_frames++;
	_hasLastFrame = true;
	_lastFrame = frame;
}
