#include "pch.h"
#include "Shared/McpFramebuffer.h"
#include "Shared/Emulator.h"
#include "Shared/Interfaces/IConsole.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConstants.h"
#include "Utilities/md5.h"

#include <algorithm>
#include <cctype>

bool McpFramebuffer::CaptureNes(Emulator* emu, McpFramebufferSnapshot& snapshot, std::string& error)
{
	snapshot = {};
	if(!emu) {
		error = "emulator unavailable";
		return false;
	}
	IConsole* base = emu->GetConsoleUnsafe();
	if(!base || base->GetConsoleType() != ConsoleType::Nes) {
		error = "NES console required";
		return false;
	}
	NesConsole* console = static_cast<NesConsole*>(base);
	const uint16_t* pixels = nullptr;
	uint32_t frame = 0;
	if(!console->GetPpu()->GetLastCompletedFrame(pixels, frame)) {
		error = "completed framebuffer unavailable; advance at least one frame";
		return false;
	}

	snapshot.Frame = frame;
	snapshot.Width = NesConstants::ScreenWidth;
	snapshot.Height = NesConstants::ScreenHeight;
	snapshot.Data.resize(NesConstants::ScreenPixelCount * sizeof(uint16_t));
	for(size_t i = 0; i < NesConstants::ScreenPixelCount; i++) {
		snapshot.Data[i * 2] = (uint8_t)(pixels[i] & 0xFF);
		snapshot.Data[i * 2 + 1] = (uint8_t)(pixels[i] >> 8);
	}
	snapshot.Hash = GetMd5Sum(snapshot.Data.data(), snapshot.Data.size());
	std::transform(snapshot.Hash.begin(), snapshot.Hash.end(), snapshot.Hash.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return true;
}
