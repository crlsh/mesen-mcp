#include "pch.h"
#include "Shared/McpServer.h"
#include "Shared/Emulator.h"
#include "Shared/BaseControlManager.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/ControlDeviceState.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/MessageManager.h"
#include "Shared/MemoryType.h"
#include "Shared/SettingTypes.h"
#include "Shared/CpuType.h"
#include "Shared/DebuggerRequest.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/DebugTypes.h"
#include "Utilities/Socket.h"
#include "Utilities/VirtualFile.h"

#include <sstream>
#include <algorithm>

// ============================================================================
// JSON helpers — TCP thread only, core thread never calls these
// ============================================================================

std::string McpServer::ExtractString(const std::string& json, const std::string& key)
{
	std::string search = "\"" + key + "\"";
	size_t pos = json.find(search);
	if(pos == std::string::npos) return "";

	pos = json.find(':', pos + search.size());
	if(pos == std::string::npos) return "";
	pos++;
	while(pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
	if(pos >= json.size() || json[pos] != '"') return "";
	pos++;

	std::string result;
	while(pos < json.size() && json[pos] != '"') {
		if(json[pos] == '\\' && pos + 1 < json.size()) {
			pos++;
			if(json[pos] == '"') result += '"';
			else if(json[pos] == '\\') result += '\\';
			else if(json[pos] == 'n') result += '\n';
			else result += json[pos];
		} else {
			result += json[pos];
		}
		pos++;
	}
	return result;
}

int McpServer::ExtractInt(const std::string& json, const std::string& key, int defaultVal)
{
	std::string search = "\"" + key + "\"";
	size_t pos = json.find(search);
	if(pos == std::string::npos) return defaultVal;

	pos = json.find(':', pos + search.size());
	if(pos == std::string::npos) return defaultVal;
	pos++;
	while(pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

	bool negative = false;
	if(pos < json.size() && json[pos] == '-') { negative = true; pos++; }

	int val = 0;
	bool found = false;
	while(pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
		val = val * 10 + (json[pos] - '0');
		found = true;
		pos++;
	}
	if(!found) return defaultVal;
	return negative ? -val : val;
}

std::string McpServer::OkResponse(int id, const std::string& resultJson)
{
	return "{\"ok\":true,\"result\":" + resultJson + ",\"id\":" + std::to_string(id) + "}";
}

std::string McpServer::ErrorResponse(int id, const std::string& error)
{
	// Escape quotes in error message
	std::string escaped;
	for(char c : error) {
		if(c == '"') escaped += "\\\"";
		else if(c == '\\') escaped += "\\\\";
		else escaped += c;
	}
	return "{\"ok\":false,\"error\":\"" + escaped + "\",\"id\":" + std::to_string(id) + "}";
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

McpServer::McpServer(Emulator* emu, uint16_t port)
	: _emu(emu), _port(port), _stop(false)
{
}

McpServer::~McpServer()
{
	Stop();
}

// ============================================================================
// Start / Stop
// ============================================================================

void McpServer::Start()
{
	if(_listenThread) return;
	_stop = false;
	_listenThread.reset(new std::thread(&McpServer::ListenLoop, this));
}

void McpServer::Stop()
{
	_stop = true;
	if(_listener) _listener->Close();
	if(_listenThread && _listenThread->joinable()) {
		_listenThread->join();
		_listenThread.reset();
	}
	_listener.reset();
}

// ============================================================================
// TCP Listen Loop — runs on its own thread
// ============================================================================

void McpServer::ListenLoop()
{
	_listener.reset(new Socket());
	_listener->SetBlocking(true);
	_listener->Bind(_port);
	_listener->Listen(5);

	while(!_stop) {
		std::unique_ptr<Socket> client = _listener->Accept();
		if(!client->ConnectionError() && !_stop) {
			client->SetBlocking(true);
			HandleClient(std::move(client));
		}
	}
}

void McpServer::HandleClient(std::unique_ptr<Socket> client)
{
	std::string buffer;
	char chunk[4096];

	int received = client->Recv(chunk, sizeof(chunk) - 1, 0);
	if(received <= 0) return;

	chunk[received] = '\0';
	buffer += chunk;

	// Look for complete line (newline-delimited JSON)
	size_t nlPos = buffer.find('\n');
	if(nlPos == std::string::npos) {
		// Incomplete message - send error and close
		std::string err = ErrorResponse(0, "incomplete request") + "\n";
		client->Send((char*)err.c_str(), (int)err.size(), 0);
		return;
	}

	std::string line = buffer.substr(0, nlPos);
	if(!line.empty() && line.back() == '\r') line.pop_back();
	if(line.empty()) {
		std::string err = ErrorResponse(0, "empty request") + "\n";
		client->Send((char*)err.c_str(), (int)err.size(), 0);
		return;
	}

	// Parse JSON into typed command (TCP thread does ALL parsing)
	auto cmd = ParseCommand(line);
	if(!cmd) {
		std::string err = ErrorResponse(0, "invalid command") + "\n";
		client->Send((char*)err.c_str(), (int)err.size(), 0);
		return;
	}

	// Enqueue for core thread
	{
		std::lock_guard<std::mutex> lock(_queueMutex);
		_commandQueue.push(cmd);
	}

	// Block until core thread processes it (hard 30s timeout)
	std::string response = cmd->WaitForResponse();
	response += "\n";
	client->Send((char*)response.c_str(), (int)response.size(), 0);

	// Socket closes automatically when client goes out of scope
}


std::shared_ptr<McpTypedCommand> McpServer::ParseCommand(const std::string& json)
{
	std::string method = ExtractString(json, "method");
	if(method.empty()) return nullptr;

	auto cmd = std::make_shared<McpTypedCommand>();
	cmd->id = ExtractInt(json, "id", 0);

	if(method == "load_rom") {
		cmd->type = McpCommandType::LoadRom;
		cmd->path = ExtractString(json, "path");
	} else if(method == "step_frame") {
		cmd->type = McpCommandType::StepFrame;
		cmd->count = ExtractInt(json, "count", 1);
	} else if(method == "read_memory") {
		cmd->type = McpCommandType::ReadMemory;
		cmd->address = ExtractInt(json, "address", -1);
		cmd->count = ExtractInt(json, "size", 1); // reuse count for size
	} else if(method == "write_memory") {
		cmd->type = McpCommandType::WriteMemory;
		cmd->address = ExtractInt(json, "address", -1);
		cmd->value = ExtractInt(json, "value", -1);
	} else if(method == "set_input") {
		cmd->type = McpCommandType::SetInput;
		cmd->port = ExtractInt(json, "port", 0);
		cmd->buttons = ExtractInt(json, "buttons", 0);
	} else if(method == "get_state") {
		cmd->type = McpCommandType::GetState;
	} else {
		return nullptr;
	}

	return cmd;
}

// ============================================================================
// DrainCommandQueue — called from emulation thread (core thread)
// ============================================================================

void McpServer::DrainCommandQueue()
{
	while(true) {
		std::shared_ptr<McpTypedCommand> cmd;
		{
			std::lock_guard<std::mutex> lock(_queueMutex);
			if(_commandQueue.empty()) break;
			cmd = _commandQueue.front();
			_commandQueue.pop();
		}

		std::string response = ExecuteCommand(*cmd);
		cmd->SetResponse(response);
	}
}

void McpServer::ExecutePendingIntentions()
{
	// Execute ROM loading intention if pending
	// Safe here: we're in the emu thread at a safe point (before frame execution)
	if(_coreState.phase == EmuPhase::LoadingRom) {
		std::string path;
		{
			std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
			path = _coreState.pendingRomPath;
			_coreState.pendingRomPath.clear();
		}

		// Safe to call LoadRom - we're at a safe point in the emu loop
		// Pass stopRom=false to keep the emulator running after loading (MCP controls the clock)
		bool loaded = _emu->LoadRom((VirtualFile)path, VirtualFile(), false);

		_coreState.phase = loaded ? EmuPhase::Running : EmuPhase::Error;
	}

	// Execute input intention if pending (BEFORE frame execution)
	if(_coreState.phase == EmuPhase::Running) {
		int port, buttons;
		{
			std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
			port = _coreState.pendingInputPort;
			buttons = _coreState.pendingInputButtons;
			_coreState.pendingInputPort = -1;  // Clear after reading
		}

		if(port >= 0) {  // Valid input pending
			IConsole* console = _emu->GetConsoleUnsafe();
			if(console) {
				BaseControlManager* ctrlMgr = console->GetControlManager();
				if(ctrlMgr) {
					shared_ptr<BaseControlDevice> controller = ctrlMgr->GetControlDevice(port, 0);
					if(controller) {
						ControlDeviceState state;
						state.State.push_back((uint8_t)(buttons & 0xFF));
						controller->SetRawState(state);
					}
				}
			}
		}
	}

	// Execute frame stepping intention if pending
	if(_coreState.phase == EmuPhase::Running) {
		int frameCount;
		{
			std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
			frameCount = _coreState.pendingFrameCount;
			_coreState.pendingFrameCount = 0;
		}

		if(frameCount > 0) {
			IConsole* console = _emu->GetConsoleUnsafe();
			if(console) {
				for(int i = 0; i < frameCount; i++) {
					console->RunFrame();
				}
			}
		}
	}
}

// ============================================================================
// Command Router — core thread only
// ============================================================================

std::string McpServer::ExecuteCommand(McpTypedCommand& cmd)
{
	switch(cmd.type) {
		case McpCommandType::LoadRom: return ExecLoadRom(cmd);
		case McpCommandType::StepFrame: return ExecStepFrame(cmd);
		case McpCommandType::ReadMemory: return ExecReadMemory(cmd);
		case McpCommandType::WriteMemory: return ExecWriteMemory(cmd);
		case McpCommandType::SetInput: return ExecSetInput(cmd);
		case McpCommandType::GetState: return ExecGetState(cmd);
		default: return ErrorResponse(cmd.id, "unknown command type");
	}
}

// ============================================================================
// Command Implementations — core thread only, no JSON parsing here
// ============================================================================

static MemoryType GetCpuMemoryType(ConsoleType ct)
{
	switch(ct) {
		case ConsoleType::Nes: return MemoryType::NesMemory;
		case ConsoleType::Snes: return MemoryType::SnesMemory;
		case ConsoleType::Gameboy: return MemoryType::GameboyMemory;
		case ConsoleType::PcEngine: return MemoryType::PceMemory;
		case ConsoleType::Sms: return MemoryType::SmsMemory;
		case ConsoleType::Gba: return MemoryType::GbaMemory;
		default: return MemoryType::NesMemory;
	}
}

static CpuType GetMainCpuType(ConsoleType ct)
{
	switch(ct) {
		case ConsoleType::Nes: return CpuType::Nes;
		case ConsoleType::Snes: return CpuType::Snes;
		case ConsoleType::Gameboy: return CpuType::Gameboy;
		case ConsoleType::PcEngine: return CpuType::Pce;
		case ConsoleType::Gba: return CpuType::Gba;
		default: return CpuType::Nes;
	}
}

std::string McpServer::ExecLoadRom(McpTypedCommand& cmd)
{
	if(cmd.path.empty()) {
		return ErrorResponse(cmd.id, "missing path");
	}

	// Declare intention only - do NOT execute
	{
		std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
		_coreState.pendingRomPath = cmd.path;
		_coreState.phase = EmuPhase::LoadingRom;
	}

	// Return immediately - execution happens in emu thread
	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecStepFrame(McpTypedCommand& cmd)
{
	// Only accept step_frame when running
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	int count = cmd.count;
	if(count < 1) count = 1;
	if(count > 3600) count = 3600;

	// Declare intention - do NOT execute frames
	{
		std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
		_coreState.pendingFrameCount = count;
	}

	// Return immediately - frames execute in emu thread
	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecReadMemory(McpTypedCommand& cmd)
{
	if(!_emu->IsRunning()) return ErrorResponse(cmd.id, "no ROM loaded");
	if(cmd.address < 0) return ErrorResponse(cmd.id, "invalid address");

	MemoryType memType = GetCpuMemoryType(_emu->GetConsoleType());

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) return ErrorResponse(cmd.id, "debugger not available");

	int size = cmd.count;
	if(size < 1) size = 1;
	if(size > 256) size = 256;

	if(size == 1) {
		uint8_t val = dbg->GetMemoryDumper()->GetMemoryValue(memType, (uint32_t)cmd.address);
		return OkResponse(cmd.id, "{\"value\":" + std::to_string(val) + "}");
	}

	// Multi-byte read
	std::vector<uint8_t> buf(size);
	dbg->GetMemoryDumper()->GetMemoryValues(memType, (uint32_t)cmd.address, (uint32_t)(cmd.address + size - 1), buf.data());

	std::ostringstream result;
	result << "{\"address\":" << cmd.address << ",\"size\":" << size << ",\"data\":[";
	for(int i = 0; i < size; i++) {
		if(i > 0) result << ",";
		result << (int)buf[i];
	}
	result << "]}";
	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecWriteMemory(McpTypedCommand& cmd)
{
	if(!_emu->IsRunning()) return ErrorResponse(cmd.id, "no ROM loaded");
	if(cmd.address < 0) return ErrorResponse(cmd.id, "invalid address");
	if(cmd.value < 0 || cmd.value > 255) return ErrorResponse(cmd.id, "value must be 0-255");

	MemoryType memType = GetCpuMemoryType(_emu->GetConsoleType());

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) return ErrorResponse(cmd.id, "debugger not available");

	dbg->GetMemoryDumper()->SetMemoryValue(memType, (uint32_t)cmd.address, (uint8_t)cmd.value);
	return OkResponse(cmd.id, "{\"address\":" + std::to_string(cmd.address) +
	                          ",\"value\":" + std::to_string(cmd.value) + "}");
}

std::string McpServer::ExecSetInput(McpTypedCommand& cmd)
{
	// Only accept set_input when running
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	// Declare intention - do NOT execute
	{
		std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
		_coreState.pendingInputPort = cmd.port;
		_coreState.pendingInputButtons = cmd.buttons;
	}

	// Return immediately - input executes in emu thread
	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecGetState(McpTypedCommand& cmd)
{
	std::ostringstream result;
	result << "{\"phase\":\"";

	switch(_coreState.phase.load()) {
		case EmuPhase::Idle:       result << "idle"; break;
		case EmuPhase::LoadingRom: result << "loading"; break;
		case EmuPhase::Running:    result << "running"; break;
		case EmuPhase::Error:      result << "error"; break;
	}

	result << "\"";

	// Add frame count and PC when running
	if(_coreState.phase == EmuPhase::Running && _emu->IsRunning()) {
		result << ",\"frame_count\":" << _emu->GetFrameCount();

		DebuggerRequest dbgRequest = _emu->GetDebugger(true);
		Debugger* dbg = dbgRequest.GetDebugger();
		if(dbg) {
			ConsoleType ct = _emu->GetConsoleType();
			CpuType cpuType = GetMainCpuType(ct);
			uint32_t pc = dbg->GetProgramCounter(cpuType, false);
			result << ",\"pc\":" << pc;
		}
	}

	result << "}";
	return OkResponse(cmd.id, result.str());
}
