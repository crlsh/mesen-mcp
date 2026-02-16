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
	printf("[MCP] Server listening on port %d\n", _port); fflush(stdout);
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
	_listener->Bind(_port);
	_listener->Listen(1); // Single client

	while(!_stop) {
		std::unique_ptr<Socket> client = _listener->Accept();
		if(!client->ConnectionError() && !_stop) {
			HandleClient(std::move(client));
		}
	}
}

void McpServer::HandleClient(std::unique_ptr<Socket> client)
{
	printf("[MCP] Client connected\n"); fflush(stdout);
	std::string buffer;
	char chunk[4096];

	while(!_stop && !client->ConnectionError()) {
		int received = client->Recv(chunk, sizeof(chunk) - 1, 0);
		printf("[MCP] Recv returned: %d\n", received); fflush(stdout);
		if(received <= 0) break;
		chunk[received] = '\0';
		buffer += chunk;

		// Process complete lines (newline-delimited JSON)
		size_t nlPos;
		while((nlPos = buffer.find('\n')) != std::string::npos) {
			std::string line = buffer.substr(0, nlPos);
			buffer = buffer.substr(nlPos + 1);
			if(!line.empty() && line.back() == '\r') line.pop_back();
			if(line.empty()) continue;

			// Parse JSON into typed command (TCP thread does ALL parsing)
			printf("[MCP] Parsing: %s\n", line.c_str()); fflush(stdout);
			auto cmd = ParseCommand(line);
			if(!cmd) {
				std::string err = ErrorResponse(0, "invalid command") + "\n";
				client->Send((char*)err.c_str(), (int)err.size(), 0);
				continue;
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
		}
	}
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
	static int drainCount = 0;
	if(++drainCount % 600 == 1) { printf("[MCP] Drain count=%d\n", drainCount); fflush(stdout); }
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

	// Stop current system completely
	if(_emu->IsRunning()) {
		_emu->Stop(false, false, true);
	}

	// Load ROM — this creates console, detects type, starts emu thread
	bool loaded = _emu->LoadRom((VirtualFile)cmd.path, VirtualFile());
	if(!loaded) {
		_coreState.romLoaded = false;
		_coreState.consoleType = -1;
		_coreState.externalControl = false;
		return ErrorResponse(cmd.id, "failed to load ROM");
	}

	ConsoleType ct = _emu->GetConsoleType();
	_coreState.consoleType = (int)ct;
	_coreState.romLoaded = true;
	_coreState.externalControl = true;

	std::ostringstream result;
	result << "{\"console_type\":" << (int)ct
	       << ",\"path\":\"";
	// Escape backslashes in path
	for(char c : cmd.path) {
		if(c == '\\') result << "\\\\";
		else if(c == '"') result << "\\\"";
		else result << c;
	}
	result << "\",\"mode\":\"external_controlled\"}";

	MessageManager::Log("[MCP] ROM loaded: " + cmd.path + " (console=" + std::to_string((int)ct) + ")");
	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecStepFrame(McpTypedCommand& cmd)
{
	if(!_coreState.romLoaded) return ErrorResponse(cmd.id, "no ROM loaded");

	int count = cmd.count;
	if(count < 1) count = 1;
	if(count > 3600) count = 3600;

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) return ErrorResponse(cmd.id, "no active console");

	for(int i = 0; i < count; i++) {
		console->RunFrame();
	}

	uint32_t frameCount = _emu->GetFrameCount();
	std::ostringstream result;
	result << "{\"framesExecuted\":" << count << ",\"frameCount\":" << frameCount << "}";
	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecReadMemory(McpTypedCommand& cmd)
{
	if(!_coreState.romLoaded) return ErrorResponse(cmd.id, "no ROM loaded");
	if(cmd.address < 0) return ErrorResponse(cmd.id, "invalid address");

	MemoryType memType = GetCpuMemoryType((ConsoleType)_coreState.consoleType);

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
	if(!_coreState.romLoaded) return ErrorResponse(cmd.id, "no ROM loaded");
	if(cmd.address < 0) return ErrorResponse(cmd.id, "invalid address");
	if(cmd.value < 0 || cmd.value > 255) return ErrorResponse(cmd.id, "value must be 0-255");

	MemoryType memType = GetCpuMemoryType((ConsoleType)_coreState.consoleType);

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) return ErrorResponse(cmd.id, "debugger not available");

	dbg->GetMemoryDumper()->SetMemoryValue(memType, (uint32_t)cmd.address, (uint8_t)cmd.value);
	return OkResponse(cmd.id, "{\"address\":" + std::to_string(cmd.address) +
	                          ",\"value\":" + std::to_string(cmd.value) + "}");
}

std::string McpServer::ExecSetInput(McpTypedCommand& cmd)
{
	if(!_coreState.romLoaded) return ErrorResponse(cmd.id, "no ROM loaded");

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) return ErrorResponse(cmd.id, "no active console");

	BaseControlManager* ctrlMgr = console->GetControlManager();
	if(!ctrlMgr) return ErrorResponse(cmd.id, "no control manager");

	shared_ptr<BaseControlDevice> controller = ctrlMgr->GetControlDevice(cmd.port, 0);
	if(!controller) return ErrorResponse(cmd.id, "no controller on port " + std::to_string(cmd.port));

	ControlDeviceState state;
	state.State.push_back((uint8_t)(cmd.buttons & 0xFF));
	controller->SetRawState(state);

	return OkResponse(cmd.id, "{\"port\":" + std::to_string(cmd.port) +
	                          ",\"buttons\":" + std::to_string(cmd.buttons) + "}");
}

std::string McpServer::ExecGetState(McpTypedCommand& cmd)
{
	std::ostringstream result;
	result << "{\"rom_loaded\":" << (_coreState.romLoaded ? "true" : "false")
	       << ",\"console_type\":" << _coreState.consoleType
	       << ",\"mode\":\"" << (_coreState.externalControl ? "external_controlled" : "free_running") << "\"";

	if(_coreState.romLoaded) {
		result << ",\"frame_count\":" << _emu->GetFrameCount();

		DebuggerRequest dbgRequest = _emu->GetDebugger(true);
		Debugger* dbg = dbgRequest.GetDebugger();
		if(dbg) {
			CpuType cpuType = GetMainCpuType((ConsoleType)_coreState.consoleType);
			uint32_t pc = dbg->GetProgramCounter(cpuType, false);
			result << ",\"pc\":" << pc;
		}
	}

	result << "}";
	return OkResponse(cmd.id, result.str());
}
