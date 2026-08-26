#include "pch.h"
#include "Shared/McpServer.h"
#include "Shared/Emulator.h"
#include "Shared/SaveStateManager.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/DebuggerRequest.h"
#include "Shared/MemoryType.h"
#include "Shared/CpuType.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"
#include "NES/NesTypes.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "NES/BaseMapper.h"
#include "Shared/McpFm2Input.h"
#include "../../../mesen-oracle/native/MesenOracleRecorder.h"

#include <sstream>

namespace {
	MemoryType OracleCpuMemoryType(ConsoleType type)
	{
		switch(type) {
			case ConsoleType::Nes: return MemoryType::NesMemory;
			case ConsoleType::Snes: return MemoryType::SnesMemory;
			case ConsoleType::Gameboy: return MemoryType::GameboyMemory;
			case ConsoleType::PcEngine: return MemoryType::PceMemory;
			case ConsoleType::Sms: return MemoryType::SmsMemory;
			case ConsoleType::Gba: return MemoryType::GbaMemory;
			default: return MemoryType::NesMemory;
		}
	}
}

std::string McpServer::ExecStartOracleCapture(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	if(cmd.path.empty()) return ErrorResponse(cmd.id, "missing path");
	if(cmd.address < 0 || cmd.value < 0) return ErrorResponse(cmd.id, "entry_prg and end_prg are required");
	std::string error;
	if(!MesenOracleRecorder::Instance().Start(cmd.path, (uint32_t)cmd.address, (uint32_t)cmd.value,
		cmd.count > 0 ? (uint32_t)cmd.count : 0, cmd.includeFramebufferHash, error)) return ErrorResponse(cmd.id, error);
	return OkResponse(cmd.id, R"({"started":true})");
}

std::string McpServer::ExecStopOracleCapture(McpTypedCommand& cmd)
{
	MesenOracleRecorder& recorder = MesenOracleRecorder::Instance();
	uint64_t rows = recorder.RowCount();
	uint64_t entries = recorder.EntryCount();
	uint64_t framebufferMisses = recorder.FramebufferMissCount();
	recorder.Stop();
	return OkResponse(cmd.id, "{\"stopped\":true,\"rows\":" + std::to_string(rows) + ",\"entries\":" + std::to_string(entries)
		+ ",\"framebufferMisses\":" + std::to_string(framebufferMisses) + "}");
}

std::string McpServer::ExecGetOracleCaptureStatus(McpTypedCommand& cmd)
{
	MesenOracleRecorder& recorder = MesenOracleRecorder::Instance();
	return OkResponse(cmd.id, std::string("{\"enabled\":") + (recorder.IsEnabled() ? "true" : "false")
		+ ",\"rows\":" + std::to_string(recorder.RowCount()) + ",\"entries\":" + std::to_string(recorder.EntryCount())
		+ ",\"framebufferMisses\":" + std::to_string(recorder.FramebufferMissCount()) + "}");
}

std::string McpServer::ExecWriteMemoryBlock(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	if(cmd.address < 0 || cmd.address > 0xFFFF) return ErrorResponse(cmd.id, "invalid address");
	if(cmd.values.empty()) return ErrorResponse(cmd.id, "data must not be empty");
	if(cmd.values.size() > 2048 || (size_t)cmd.address + cmd.values.size() > 0x10000) return ErrorResponse(cmd.id, "memory block exceeds CPU address space or 2048-byte limit");
	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) return ErrorResponse(cmd.id, "no console");
	DebuggerRequest request = _emu->GetDebugger(true);
	Debugger* debugger = request.GetDebugger();
	if(!debugger) return ErrorResponse(cmd.id, "debugger unavailable");
	MemoryDumper* dumper = debugger->GetMemoryDumper();
	MemoryType memoryType = OracleCpuMemoryType(console->GetConsoleType());
	for(size_t i = 0; i < cmd.values.size(); i++) {
		if(cmd.values[i] < 0 || cmd.values[i] > 255) return ErrorResponse(cmd.id, "data values must be 0-255");
		dumper->SetMemoryValue(memoryType, (uint32_t)(cmd.address + i), (uint8_t)cmd.values[i]);
	}
	return OkResponse(cmd.id, "{\"written\":" + std::to_string(cmd.values.size()) + "}");
}

std::string McpServer::ExecSaveStateFile(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	if(cmd.path.empty()) return ErrorResponse(cmd.id, "missing path");
	return _emu->GetSaveStateManager()->SaveState(cmd.path, false)
		? OkResponse(cmd.id, R"({"saved":true})") : ErrorResponse(cmd.id, "unable to save state");
}

std::string McpServer::ExecLoadStateFile(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	if(cmd.path.empty()) return ErrorResponse(cmd.id, "missing path");
	// Routine seeding requires an atomic restore boundary: loading a state must
	// never let the emulation loop run instructions before RAM and breakpoints
	// are installed by the TCP client.
	_emu->GetMcpExecutionController().CloseClock();
	if(!_emu->GetSaveStateManager()->LoadState(cmd.path, false)) {
		return ErrorResponse(cmd.id, "unable to load state");
	}
	return OkResponse(cmd.id, R"({"loaded":true,"paused":true})");
}

std::string McpServer::ExecGetNesRuntimeState(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	IConsole* base = _emu->GetConsoleUnsafe();
	if(!base || base->GetConsoleType() != ConsoleType::Nes) return ErrorResponse(cmd.id, "NES console required");
	NesConsole* console = static_cast<NesConsole*>(base);
	NesPpuState ppu = {};
	console->GetPpu()->GetState(ppu);
	BaseMapper* mapper = console->GetMapper();
	if(!mapper) return ErrorResponse(cmd.id, "mapper unavailable");
	CartridgeState cartridge = mapper->GetState();
	uint16_t v = ppu.VideoRamAddr;
	uint16_t t = ppu.TmpVideoRamAddr;
	uint32_t nt = (v >> 10) & 3;
	uint32_t scrollX = (nt & 1) * 256 + (v & 0x1F) * 8 + ppu.ScrollX;
	uint32_t scrollY = ((nt >> 1) & 1) * 240 + ((v >> 5) & 0x1F) * 8 + ((v >> 12) & 7);
	const char* mirroring = "unknown";
	switch(cartridge.Mirroring) {
		case MirroringType::Horizontal: mirroring = "horizontal"; break;
		case MirroringType::Vertical: mirroring = "vertical"; break;
		case MirroringType::ScreenAOnly: mirroring = "screenA"; break;
		case MirroringType::ScreenBOnly: mirroring = "screenB"; break;
		case MirroringType::FourScreens: mirroring = "fourScreens"; break;
	}
	std::ostringstream result;
	result << "{\"frame\":" << ppu.FrameCount << ",\"scanline\":" << ppu.Scanline << ",\"cycle\":" << ppu.Cycle
		<< ",\"ppu\":{\"v\":" << v << ",\"t\":" << t << ",\"fineX\":" << (int)ppu.ScrollX
		<< ",\"writeToggle\":" << (ppu.WriteToggle ? "true" : "false") << ",\"spriteRamAddr\":" << (int)ppu.SpriteRamAddr
		<< ",\"scrollX\":" << scrollX << ",\"scrollY\":" << scrollY << ",\"ntBase\":" << nt << "}"
		<< ",\"mapper\":{\"mirroring\":\"" << mirroring << "\",\"activeNametablePage\":"
		<< (cartridge.Mirroring == MirroringType::ScreenBOnly ? 1 : 0) << ",\"prgMap\":[";
	auto windows = mapper->GetCurrentPrgMap();
	for(size_t i = 0; i < windows.size(); i++) {
		if(i) result << ',';
		const PrgWindow& window = windows[i];
		result << "{\"cpuStart\":" << window.CpuStart << ",\"cpuEnd\":" << window.CpuEnd
			<< ",\"prgOffset\":" << window.PrgOffsetStart << ",\"bank\":" << window.Bank << "}";
	}
	result << "]},\"chr\":{\"ram\":" << (mapper->HasChrRam() ? "true" : "false")
		<< ",\"rom\":" << (mapper->HasChrRom() ? "true" : "false") << "}}";
	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecQueueInputSequence(McpTypedCommand& cmd)
{
	if(cmd.port < 0 || cmd.port > 3) return ErrorResponse(cmd.id, "invalid port");
	if(cmd.buttonsSpecified == cmd.fm2FileSpecified) return ErrorResponse(cmd.id, "exactly one of buttons or fm2_file is required");
	std::string source = "buttons";
	if(cmd.fm2FileSpecified) {
		std::string error;
		if(!McpFm2Input::Load(cmd.path, cmd.values, error)) return ErrorResponse(cmd.id, error);
		source = "fm2_file";
	}
	if(cmd.values.empty()) return ErrorResponse(cmd.id, "buttons must not be empty");
	for(int value : cmd.values) {
		if(value < 0 || value > 255) return ErrorResponse(cmd.id, "button values must be 0-255");
	}
	std::lock_guard<std::mutex> lock(_coreState.inputSequenceMutex);
	for(int value : cmd.values) {
		_coreState.inputSequence.push_back((uint8_t)value);
	}
	_coreState.inputSequencePort = cmd.port;
	_coreState.inputSequenceEnabled = true;
	return OkResponse(cmd.id, "{\"queued\":" + std::to_string(cmd.values.size()) + ",\"total\":" + std::to_string(_coreState.inputSequence.size()) + ",\"source\":\"" + source + "\"}");
}

std::string McpServer::ExecClearInputSequence(McpTypedCommand& cmd)
{
	std::lock_guard<std::mutex> lock(_coreState.inputSequenceMutex);
	_coreState.inputSequence.clear();
	_coreState.inputSequenceIndex = 0;
	_coreState.inputSequenceLastFrame = UINT32_MAX;
	_coreState.inputSequenceCurrentButtons = 0;
	_coreState.inputSequenceEnabled = false;
	return OkResponse(cmd.id, R"({"cleared":true})");
}

std::string McpServer::ExecGetInputSequenceStatus(McpTypedCommand& cmd)
{
	std::lock_guard<std::mutex> lock(_coreState.inputSequenceMutex);
	std::ostringstream result;
	result << "{\"enabled\":" << (_coreState.inputSequenceEnabled ? "true" : "false")
		<< ",\"index\":" << _coreState.inputSequenceIndex << ",\"total\":" << _coreState.inputSequence.size()
		<< ",\"remaining\":" << (_coreState.inputSequence.size() - _coreState.inputSequenceIndex) << "}";
	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecWaitForBreak(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	int timeoutMs = cmd.count;
	if(timeoutMs < 1) timeoutMs = 1;
	if(timeoutMs > 600000) timeoutMs = 600000;
	for(int elapsed = 0; elapsed < timeoutMs; elapsed += 5) {
		McpExecutionState state = _emu->GetMcpExecutionController().GetSnapshot().State;
		if(state == McpExecutionState::BreakpointStopped) return ExecGetState(cmd);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return ErrorResponse(cmd.id, "breakpoint timeout");
}
