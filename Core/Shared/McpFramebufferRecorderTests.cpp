#include "pch.h"
#include "Shared/McpFramebufferRecorder.h"
#include "Shared/McpFramebuffer.h"
#include "NES/NesConstants.h"

#include <filesystem>
#include <fstream>

namespace McpFramebufferRecorderTests {

static uint32_t _failures = 0;
#define CHECK(condition) do { if(!(condition)) { _failures++; } } while(0)

uint32_t RunAll()
{
	_failures = 0;
	std::filesystem::path path = std::filesystem::temp_directory_path() / "mesen-mcp-framebuffer-recorder-tests.jsonl";
	std::error_code ignored;
	std::filesystem::remove(path, ignored);
	std::vector<uint16_t> first(NesConstants::ScreenPixelCount, 0);
	std::vector<uint16_t> second(NesConstants::ScreenPixelCount, 0xFFFF);
	McpFramebufferSnapshot firstSnapshot;
	McpFramebufferSnapshot secondSnapshot;
	McpFramebuffer::CaptureNesPixels(first.data(), 7, firstSnapshot);
	McpFramebuffer::CaptureNesPixels(second.data(), 8, secondSnapshot);

	McpFramebufferRecorder& recorder = McpFramebufferRecorder::Instance();
	std::string error;
	CHECK(recorder.Start(path.u8string(), error));
	CHECK(recorder.GetStatus().Frames == 0);
	recorder.Record(7, first.data());
	recorder.Record(8, second.data());
	McpFramebufferRecorderStatus active = recorder.GetStatus();
	CHECK(active.Enabled && active.Frames == 2 && active.HasLastFrame && active.LastFrame == 8);
	McpFramebufferRecorderStatus stopped = recorder.Stop();
	CHECK(!stopped.Enabled && stopped.Frames == 2 && stopped.WriteFailures == 0);

	std::ifstream input(path, std::ios::in | std::ios::binary);
	std::string firstLine;
	std::string secondLine;
	std::string extraLine;
	std::getline(input, firstLine);
	std::getline(input, secondLine);
	CHECK(!std::getline(input, extraLine));
	CHECK(firstLine == "{\"frame\":7,\"fbHash\":\"" + firstSnapshot.Hash + "\"}");
	CHECK(secondLine == "{\"frame\":8,\"fbHash\":\"" + secondSnapshot.Hash + "\"}");

	// A stopped recorder must be reusable and reset all per-capture counters.
	error.clear();
	CHECK(recorder.Start(path.u8string(), error));
	recorder.Record(9, first.data());
	CHECK(recorder.Stop().Frames == 1);
	std::filesystem::remove(path, ignored);
	return _failures;
}

}
