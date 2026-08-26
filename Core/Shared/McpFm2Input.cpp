#include "pch.h"
#include "Shared/McpFm2Input.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace {
	bool DecodeButtons(const std::string& text, size_t lineNumber, int& value, std::string& error)
	{
		static constexpr char columns[] = "RLDUTSBA";
		static constexpr int bits[] = {0x08, 0x04, 0x02, 0x01, 0x10, 0x20, 0x40, 0x80};
		if(text.size() != 8) {
			error = "FM2 controller field on line " + std::to_string(lineNumber) + " must contain exactly 8 RLDUTSBA columns";
			return false;
		}
		value = 0;
		for(size_t i = 0; i < 8; i++) {
			if(text[i] == columns[i]) {
				value |= bits[i];
			} else if(text[i] != '.') {
				error = "invalid FM2 button on line " + std::to_string(lineNumber) + " at RLDUTSBA column " + std::to_string(i + 1);
				return false;
			}
		}
		return true;
	}

	std::vector<std::string> SplitRecord(const std::string& line)
	{
		std::vector<std::string> fields;
		size_t start = 0;
		for(size_t i = 0; i <= line.size(); i++) {
			if(i == line.size() || line[i] == '|') {
				fields.push_back(line.substr(start, i - start));
				start = i + 1;
			}
		}
		return fields;
	}
}

bool McpFm2Input::Load(const std::string& path, std::vector<int>& buttons, std::string& error)
{
	buttons.clear();
	std::vector<int> parsedButtons;
	if(path.empty()) {
		error = "fm2_file must not be empty";
		return false;
	}
	std::filesystem::path inputPath = std::filesystem::u8path(path);
	if(!inputPath.is_absolute()) {
		error = "fm2_file must be an absolute path";
		return false;
	}
	std::string extension = inputPath.extension().u8string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	if(extension != ".fm2") {
		error = "fm2_file must have a .fm2 extension";
		return false;
	}
	std::error_code fileError;
	if(!std::filesystem::is_regular_file(inputPath, fileError) || fileError) {
		error = "fm2_file does not exist or is not a regular file";
		return false;
	}

	std::ifstream source(inputPath, std::ios::in | std::ios::binary);
	if(!source.is_open()) {
		error = "unable to open fm2_file";
		return false;
	}
	std::string line;
	size_t lineNumber = 0;
	while(std::getline(source, line)) {
		lineNumber++;
		if(!line.empty() && line.back() == '\r') line.pop_back();
		if(lineNumber == 1 && line.size() >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) {
			line.erase(0, 3);
		}
		if(line.empty() || line[0] != '|') continue;
		std::vector<std::string> fields = SplitRecord(line);
		if(fields.size() < 4) {
			error = "invalid FM2 record on line " + std::to_string(lineNumber);
			return false;
		}
		size_t consumed = 0;
		int command = 0;
		try {
			command = std::stoi(fields[1], &consumed, 10);
		} catch(...) {
			error = "invalid FM2 command on line " + std::to_string(lineNumber);
			return false;
		}
		if(consumed != fields[1].size() || command < 0) {
			error = "invalid FM2 command on line " + std::to_string(lineNumber);
			return false;
		}
		if(command != 0) {
			error = "unsupported FM2 command " + std::to_string(command) + " on line " + std::to_string(lineNumber) + "; queue_input_sequence can represent buttons only";
			return false;
		}
		int value = 0;
		if(!DecodeButtons(fields[2], lineNumber, value, error)) return false;
		parsedButtons.push_back(value);
	}
	if(source.bad()) {
		error = "error while reading fm2_file";
		return false;
	}
	if(parsedButtons.empty()) {
		error = "FM2 contains no input records";
		return false;
	}
	buttons = std::move(parsedButtons);
	return true;
}
