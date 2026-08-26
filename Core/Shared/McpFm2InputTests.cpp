#include "pch.h"
#include "Shared/McpFm2Input.h"

#include <filesystem>
#include <fstream>

namespace McpFm2InputTests {

static uint32_t _failures = 0;
#define CHECK(condition) do { if(!(condition)) { _failures++; } } while(0)

static void Write(const std::filesystem::path& path, const std::string& content)
{
	std::ofstream output(path, std::ios::out | std::ios::trunc | std::ios::binary);
	output << content;
}

uint32_t RunAll()
{
	_failures = 0;
	std::filesystem::path path = std::filesystem::temp_directory_path() / "mesen-mcp-fm2-input-tests.fm2";
	std::error_code ignored;
	std::filesystem::remove(path, ignored);

	std::string allButtons = "\xEF\xBB\xBFversion 3\n"
		"|0|R.......|||\n|0|.L......|||\n|0|..D.....|||\n|0|...U....|||\n"
		"|0|....T...|||\n|0|.....S..|||\n|0|......B.|||\n|0|.......A|||\n|0|RLDUTSBA|||\n";
	Write(path, allButtons);
	std::vector<int> values;
	std::string error;
	CHECK(McpFm2Input::Load(path.u8string(), values, error));
	CHECK(values == std::vector<int>({0x08, 0x04, 0x02, 0x01, 0x10, 0x20, 0x40, 0x80, 0xFF}));

	std::string longMovie = "version 3\n";
	for(size_t i = 0; i < 62840; i++) longMovie += "|0|........|||\n";
	Write(path, longMovie);
	error.clear();
	CHECK(McpFm2Input::Load(path.u8string(), values, error));
	CHECK(values.size() == 62840 && values.front() == 0 && values.back() == 0);

	Write(path, "version 3\n|0|........|||\n|1|........|||\n");
	error.clear();
	CHECK(!McpFm2Input::Load(path.u8string(), values, error));
	CHECK(values.empty());
	CHECK(error.find("unsupported FM2 command 1 on line 3") != std::string::npos);

	Write(path, "version 3\n|0|.......X|||\n");
	error.clear();
	CHECK(!McpFm2Input::Load(path.u8string(), values, error));
	CHECK(error.find("line 2") != std::string::npos);

	error.clear();
	CHECK(!McpFm2Input::Load("relative.fm2", values, error));
	CHECK(error.find("absolute") != std::string::npos);

	std::filesystem::remove(path, ignored);
	return _failures;
}

}
