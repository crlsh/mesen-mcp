#include "pch.h"
#include "Shared/McpFramebuffer.h"
#include "Shared/Emulator.h"
#include "Shared/Interfaces/IConsole.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConstants.h"
// STRUCTURAL: CE removed md5.h; CRC32 is sufficient for framebuffer fingerprinting
#include "Utilities/CRC32.h"
#include "Utilities/HexUtilities.h"

#include <algorithm>
#include <cctype>

void McpFramebuffer::CaptureNesPixels(const uint16_t* pixels, uint32_t frame, McpFramebufferSnapshot& snapshot)
{
	snapshot = {};
	snapshot.Frame = frame;
	snapshot.Width = NesConstants::ScreenWidth;
	snapshot.Height = NesConstants::ScreenHeight;
	snapshot.Data.resize(NesConstants::ScreenPixelCount * sizeof(uint16_t));
	for(size_t i = 0; i < NesConstants::ScreenPixelCount; i++) {
		snapshot.Data[i * 2] = (uint8_t)(pixels[i] & 0xFF);
		snapshot.Data[i * 2 + 1] = (uint8_t)(pixels[i] >> 8);
	}
	// STRUCTURAL: CRC32 replaces MD5 for framebuffer fingerprint (CE removed md5.h)
	uint32_t crc = CRC32::GetCRC(snapshot.Data.data(), (std::streamoff)snapshot.Data.size());
	snapshot.Hash = HexUtilities::ToHex32(crc);
}

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

	CaptureNesPixels(pixels, frame, snapshot);
	return true;
}
