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
#include "Shared/EmuSettings.h"
#include "Shared/CpuType.h"
#include "Shared/DebuggerRequest.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Breakpoint.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/ITraceLogger.h"
#include "Debugger/CallstackManager.h"
#include "Shared/McpWriteLog.h"
#include "NES/NesTypes.h"
#include "NES/NesConsole.h"
#include "NES/Debugger/NesControlFlowTracer.h"
#include "NES/NesMemoryManager.h"
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

	// Accept JSON booleans as 0/1 so callers can use ExtractInt for "enabled":true
	if(pos + 4 <= json.size() && json.compare(pos, 4, "true") == 0) return 1;
	if(pos + 5 <= json.size() && json.compare(pos, 5, "false") == 0) return 0;

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

std::string McpServer::ExtractJsonArray(const std::string& json, const std::string& key)
{
	std::string search = "\"" + key + "\"";
	size_t pos = json.find(search);
	if(pos == std::string::npos) return "";

	pos = json.find(':', pos + search.size());
	if(pos == std::string::npos) return "";
	pos++;
	while(pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
	if(pos >= json.size() || json[pos] != '[') return "";

	// Find matching closing bracket (handle nesting)
	int depth = 0;
	size_t start = pos;
	while(pos < json.size()) {
		if(json[pos] == '[') depth++;
		else if(json[pos] == ']') {
			depth--;
			if(depth == 0) {
				return json.substr(start, pos - start + 1);
			}
		} else if(json[pos] == '"') {
			// Skip string content
			pos++;
			while(pos < json.size() && json[pos] != '"') {
				if(json[pos] == '\\') pos++;
				pos++;
			}
		}
		pos++;
	}
	return "";
}

std::vector<int> McpServer::ExtractIntArray(const std::string& json, const std::string& key)
{
	std::string array = ExtractJsonArray(json, key);
	std::vector<int> result;
	if(array.size() < 2) return result;
	size_t pos = 1;
	while(pos + 1 < array.size()) {
		while(pos < array.size() && (array[pos] == ' ' || array[pos] == '\t' || array[pos] == ',')) pos++;
		bool negative = false;
		if(pos < array.size() && array[pos] == '-') { negative = true; pos++; }
		int value = 0;
		bool found = false;
		while(pos < array.size() && array[pos] >= '0' && array[pos] <= '9') {
			value = value * 10 + (array[pos++] - '0');
			found = true;
		}
		if(!found) break;
		result.push_back(negative ? -value : value);
	}
	return result;
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
	constexpr size_t MaxRequestSize = 1024 * 1024;
	while(buffer.find('\n') == std::string::npos) {
		int received = client->Recv(chunk, sizeof(chunk), 0);
		if(received <= 0) return;
		buffer.append(chunk, received);
		if(buffer.size() > MaxRequestSize) {
			std::string err = ErrorResponse(0, "request exceeds 1 MiB limit") + "\n";
			client->Send((char*)err.c_str(), (int)err.size(), 0);
			return;
		}
	}

	// Look for complete line (newline-delimited JSON)
	size_t nlPos = buffer.find('\n');
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

	// Dual-path routing: execute directly on TCP thread when debugger is stopped
	std::string response;
	if(CanExecuteDirect(cmd->type)) {
		response = ExecuteCommandDirect(*cmd);
	} else {
		// Enqueue for core thread
		{
			std::lock_guard<std::mutex> lock(_queueMutex);
			_commandQueue.push(cmd);
		}

		// Block until core thread processes it (hard 30s timeout)
		response = cmd->WaitForResponse();
	}

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
		cmd->memoryType = ExtractString(json, "memory_type");
	} else if(method == "write_memory") {
		cmd->type = McpCommandType::WriteMemory;
		cmd->address = ExtractInt(json, "address", -1);
		cmd->value = ExtractInt(json, "value", -1);
	} else if(method == "set_input") {
		cmd->type = McpCommandType::SetInput;
		cmd->port = ExtractInt(json, "port", 0);
		cmd->buttons = ExtractInt(json, "buttons", 0);
	} else if(method == "reset") {
		cmd->type = McpCommandType::Reset;
	} else if(method == "get_state") {
		cmd->type = McpCommandType::GetState;
	} else if(method == "run") {
		cmd->type = McpCommandType::Run;
	} else if(method == "pause") {
		cmd->type = McpCommandType::Pause;
	} else if(method == "set_breakpoints") {
		cmd->type = McpCommandType::SetBreakpoints;
		cmd->breakpointsJson = ExtractJsonArray(json, "breakpoints");
	} else if(method == "step_instruction") {
		cmd->type = McpCommandType::StepInstruction;
		cmd->count = ExtractInt(json, "count", 1);
	} else if(method == "continue") {
		cmd->type = McpCommandType::Continue;
	} else if(method == "get_cpu_state") {
		cmd->type = McpCommandType::GetCpuState;
	} else if(method == "get_trace") {
		cmd->type = McpCommandType::GetTrace;
		cmd->count = ExtractInt(json, "count", 100);
	} else if(method == "scan_variables_internal") {
		cmd->type = McpCommandType::ScanVariablesInternal;
		cmd->buttons = ExtractInt(json, "button", 0);
		cmd->frames = ExtractInt(json, "frames", 30);
		cmd->trials = ExtractInt(json, "trials", 3);
	} else if(method == "get_callstack") {
		cmd->type = McpCommandType::GetCallstack;
	} else if(method == "set_write_log") {
		cmd->type = McpCommandType::SetWriteLog;
		cmd->startAddr = ExtractInt(json, "start_address", 0);
		cmd->endAddr = ExtractInt(json, "end_address", 0xFFFF);
		cmd->enabled = ExtractInt(json, "enabled", 0) != 0;
		cmd->count = ExtractInt(json, "cap", 65536);
	} else if(method == "get_write_log") {
		cmd->type = McpCommandType::GetWriteLog;
		cmd->count = ExtractInt(json, "max_entries", 10000);
		cmd->enabled = ExtractInt(json, "drain", 1) != 0;
	} else if(method == "set_speed") {
		cmd->type = McpCommandType::SetSpeed;
		cmd->speed = ExtractInt(json, "speed", 100);
	} else if(method == "start_control_flow_trace") {
		cmd->type = McpCommandType::StartControlFlowTrace;
		cmd->path = ExtractString(json, "path");
		cmd->deduplicate = ExtractInt(json, "deduplicate", 0) != 0;
		cmd->eventMask = ExtractInt(json, "event_mask", 0x3FF);
		cmd->graphDeduplicate = ExtractInt(json, "graph_deduplicate", 0) != 0;
		cmd->summaryPath = ExtractString(json, "summary_path");
	} else if(method == "stop_control_flow_trace") {
		cmd->type = McpCommandType::StopControlFlowTrace;
	} else if(method == "get_control_flow_trace_stats") {
		cmd->type = McpCommandType::GetControlFlowTraceStats;
	} else if(method == "write_memory_block") {
		cmd->type = McpCommandType::WriteMemoryBlock;
		cmd->address = ExtractInt(json, "address", -1);
		cmd->values = ExtractIntArray(json, "data");
	} else if(method == "save_state_file") {
		cmd->type = McpCommandType::SaveStateFile;
		cmd->path = ExtractString(json, "path");
	} else if(method == "load_state_file") {
		cmd->type = McpCommandType::LoadStateFile;
		cmd->path = ExtractString(json, "path");
	} else if(method == "get_nes_runtime_state") {
		cmd->type = McpCommandType::GetNesRuntimeState;
	} else if(method == "queue_input_sequence") {
		cmd->type = McpCommandType::QueueInputSequence;
		cmd->port = ExtractInt(json, "port", 0);
		cmd->values = ExtractIntArray(json, "buttons");
	} else if(method == "clear_input_sequence") {
		cmd->type = McpCommandType::ClearInputSequence;
	} else if(method == "get_input_sequence_status") {
		cmd->type = McpCommandType::GetInputSequenceStatus;
	} else if(method == "wait_for_break") {
		cmd->type = McpCommandType::WaitForBreak;
		cmd->count = ExtractInt(json, "timeout_ms", 30000);
	} else if(method == "start_oracle_capture") {
		cmd->type = McpCommandType::StartOracleCapture;
		cmd->path = ExtractString(json, "path");
		cmd->address = ExtractInt(json, "entry_prg", -1);
		cmd->value = ExtractInt(json, "end_prg", -1);
		cmd->count = ExtractInt(json, "max_rows", 0);
	} else if(method == "stop_oracle_capture") {
		cmd->type = McpCommandType::StopOracleCapture;
	} else if(method == "get_oracle_capture_status") {
		cmd->type = McpCommandType::GetOracleCaptureStatus;
	} else {
		return nullptr;
	}

	return cmd;
}

// Forward declarations
static MemoryType GetCpuMemoryType(ConsoleType ct);
static CpuType GetMainCpuType(ConsoleType ct);

// ============================================================================
// Dual-path execution
// ============================================================================

bool McpServer::CanExecuteDirect(McpCommandType type)
{
	// With no console there is no emulation loop to drain the queue. ROM load
	// must therefore bootstrap directly; LoadRom creates the emulation thread.
	if(type == McpCommandType::LoadRom && _coreState.phase == EmuPhase::Idle) return true;
	// GetState is always safe to run on the IO thread — it only reads atomics,
	// frame count, and PC. NEVER queue it; queueing causes a 30s hang if the
	// core thread is stuck in SleepUntilResume (e.g. inside a tight-loop BP)
	// and can't drain commands between break events.
	if(type == McpCommandType::GetState) return true;
	if(_coreState.phase != EmuPhase::Running) return false;
	if(type == McpCommandType::Continue) return true;

	// SetSpeed only writes settings + toggles a flag — no emu-thread state involved.
	// Run direct so speed changes work even while free-running (no debugger break).
	if(type == McpCommandType::SetSpeed) return true;

	//Start/StopControlFlowTrace install or clear a single pointer + open/close a file.
	//The pointer write is the only race with the emu thread and it's a single aligned
	//uint64 store — torn writes don't apply on x86-64. Worst case the emu thread sees
	//the new pointer one instruction later, which is harmless.
	if(type == McpCommandType::StartControlFlowTrace) return true;
	if(type == McpCommandType::StopControlFlowTrace) return true;
	//GetControlFlowTraceStats reads three uint64 counters — no emu-thread mutation.
	if(type == McpCommandType::GetControlFlowTraceStats) return true;
	if(type == McpCommandType::QueueInputSequence) return true;
	if(type == McpCommandType::ClearInputSequence) return true;
	if(type == McpCommandType::GetInputSequenceStatus) return true;
	if(type == McpCommandType::WaitForBreak) return true;
	if(type == McpCommandType::SetWriteLog) return true;
	if(type == McpCommandType::GetWriteLog) return true;
	if(type == McpCommandType::StartOracleCapture) return true;
	if(type == McpCommandType::StopOracleCapture) return true;
	if(type == McpCommandType::GetOracleCaptureStatus) return true;

	// All other commands need the debugger to be stopped (core thread sleeping
	// in SleepUntilResume), otherwise they would race with the running emu.
	DebuggerRequest dbgRequest = _emu->GetDebugger(false);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg || !dbg->IsExecutionStopped()) return false;

	switch(type) {
		case McpCommandType::Continue:
		case McpCommandType::StepInstruction:
		case McpCommandType::StepFrame:
		case McpCommandType::GetCpuState:
		case McpCommandType::GetTrace:
		case McpCommandType::ReadMemory:
		case McpCommandType::WriteMemory:
		case McpCommandType::SetBreakpoints:
		case McpCommandType::SetInput:
		case McpCommandType::Run:
		case McpCommandType::Pause:
		case McpCommandType::GetCallstack:
		case McpCommandType::SetWriteLog:
		case McpCommandType::GetWriteLog:
		case McpCommandType::WriteMemoryBlock:
		case McpCommandType::SaveStateFile:
		case McpCommandType::LoadStateFile:
		case McpCommandType::GetNesRuntimeState:
			return true;
		default:
			return false;
	}
}

std::string McpServer::ExecuteCommandDirect(McpTypedCommand& cmd)
{
	switch(cmd.type) {
		case McpCommandType::LoadRom: return ExecLoadRomDirect(cmd);
		case McpCommandType::Continue: return ExecContinue(cmd);
		case McpCommandType::StepInstruction: return ExecStepInstructionDirect(cmd);
		case McpCommandType::StepFrame: return ExecStepFrameDirect(cmd);
		case McpCommandType::GetCpuState: return ExecGetCpuState(cmd);
		case McpCommandType::GetTrace: return ExecGetTrace(cmd);
		case McpCommandType::ReadMemory: return ExecReadMemory(cmd);
		case McpCommandType::WriteMemory: return ExecWriteMemory(cmd);
		case McpCommandType::SetBreakpoints: return ExecSetBreakpoints(cmd);
		case McpCommandType::GetState: return ExecGetState(cmd);
		case McpCommandType::SetInput: return ExecSetInput(cmd);
		case McpCommandType::Run: return ExecRun(cmd);
		case McpCommandType::Pause: return ExecPause(cmd);
		case McpCommandType::GetCallstack: return ExecGetCallstack(cmd);
		case McpCommandType::SetWriteLog: return ExecSetWriteLog(cmd);
		case McpCommandType::GetWriteLog: return ExecGetWriteLog(cmd);
		case McpCommandType::SetSpeed: return ExecSetSpeed(cmd);
		case McpCommandType::StartControlFlowTrace: return ExecStartControlFlowTrace(cmd);
		case McpCommandType::StopControlFlowTrace: return ExecStopControlFlowTrace(cmd);
		case McpCommandType::GetControlFlowTraceStats: return ExecGetControlFlowTraceStats(cmd);
		case McpCommandType::WriteMemoryBlock: return ExecWriteMemoryBlock(cmd);
		case McpCommandType::SaveStateFile: return ExecSaveStateFile(cmd);
		case McpCommandType::LoadStateFile: return ExecLoadStateFile(cmd);
		case McpCommandType::GetNesRuntimeState: return ExecGetNesRuntimeState(cmd);
		case McpCommandType::QueueInputSequence: return ExecQueueInputSequence(cmd);
		case McpCommandType::ClearInputSequence: return ExecClearInputSequence(cmd);
		case McpCommandType::GetInputSequenceStatus: return ExecGetInputSequenceStatus(cmd);
		case McpCommandType::WaitForBreak: return ExecWaitForBreak(cmd);
		case McpCommandType::StartOracleCapture: return ExecStartOracleCapture(cmd);
		case McpCommandType::StopOracleCapture: return ExecStopOracleCapture(cmd);
		case McpCommandType::GetOracleCaptureStatus: return ExecGetOracleCaptureStatus(cmd);
		default: return ErrorResponse(cmd.id, "command not supported in direct mode");
	}
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
	// Ensure we're registered as input provider (must re-register after ROM load creates new console)
	if(!_inputProviderRegistered && _coreState.phase == EmuPhase::Running) {
		_emu->RegisterInputProvider(this);
		_inputProviderRegistered = true;
	}

	// Execute ROM loading intention if pending
	// Safe here: we're in the emu thread at a safe point (before frame execution)
	if(_coreState.phase == EmuPhase::LoadingRom) {
		std::string path;
		{
			std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
			path = _coreState.pendingRomPath;
			_coreState.pendingRomPath.clear();

			// Start paused after ROM load (MCP controls the clock)
			_coreState.freeRunning = false;
			_coreState.traceEnabled = false;
		}

		// Safe to call LoadRom - we're at a safe point in the emu loop
		// Pass stopRom=false to keep the emulator running after loading (MCP controls the clock)
		bool loaded = _emu->LoadRom((VirtualFile)path, VirtualFile(), false);

		_inputProviderRegistered = false;  // New console = new control manager
		_coreState.phase = loaded ? EmuPhase::Running : EmuPhase::Error;

		// Force DrawPartialFrame=true so the emulator window stays visible
		// when the debugger breaks (BP hit, step). Without this, breaks happen
		// mid-frame inside SleepUntilResume, the partial frame is never sent
		// to the video renderer, and the window goes black. This mirrors the
		// "Debug → Options → Draw partial frame" option in Mesen's UI.
		if(loaded) {
			DebugConfig& dbgCfg = _emu->GetSettings()->GetDebugConfig();
			dbgCfg.DrawPartialFrame = true;
			_emu->GetSettings()->SetDebugConfig(dbgCfg);
		}
	}

	// Execute reset intention if pending (highest priority, BEFORE input)
	if(_coreState.phase == EmuPhase::Running) {
		bool shouldReset;
		{
			std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
			shouldReset = _coreState.pendingReset;
			_coreState.pendingReset = false;  // Clear after reading
		}

		if(shouldReset) {
			IConsole* console = _emu->GetConsoleUnsafe();
			if(console) {
				console->Reset();
			}
		}
	}

	// Update sticky input if new set_input command arrived
	if(_coreState.phase == EmuPhase::Running) {
		{
			std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
			if(_coreState.pendingInputPort >= 0) {
				_coreState.stickyInputPort = _coreState.pendingInputPort;
				_coreState.stickyInputButtons = _coreState.pendingInputButtons;
				_coreState.pendingInputPort = -1;
			}
		}

		// Re-apply sticky input every frame (survives Mesen's input polling)
		if(_coreState.stickyInputPort >= 0) {
			IConsole* console = _emu->GetConsoleUnsafe();
			if(console) {
				BaseControlManager* ctrlMgr = console->GetControlManager();
				if(ctrlMgr) {
					shared_ptr<BaseControlDevice> controller = ctrlMgr->GetControlDevice(_coreState.stickyInputPort, 0);
					if(controller) {
						ControlDeviceState state;
						state.State.push_back((uint8_t)(_coreState.stickyInputButtons & 0xFF));
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
				// This queued path is used immediately after load_rom, before a
				// debugger exists. Oracle capture is driven by
				// NesDebugger::ProcessInstruction, so bootstrap it here too.
				DebuggerRequest dbgRequest = _emu->GetDebugger(true);
				if(!dbgRequest.GetDebugger()) {
					return;
				}
				for(int i = 0; i < frameCount; i++) {
					// Re-apply sticky input before each frame
					if(_coreState.stickyInputPort >= 0) {
						BaseControlManager* ctrlMgr = console->GetControlManager();
						if(ctrlMgr) {
							shared_ptr<BaseControlDevice> controller = ctrlMgr->GetControlDevice(_coreState.stickyInputPort, 0);
							if(controller) {
								ControlDeviceState state;
								state.State.push_back((uint8_t)(_coreState.stickyInputButtons & 0xFF));
								controller->SetRawState(state);
								controller->RefreshStateBuffer();
							}
						}
					}
					console->RunFrame();
				}
			}
		}
	}

	// Execute CPU step instruction intention if pending
	if(_coreState.phase == EmuPhase::Running) {
		bool shouldStep = false;
		int stepCount = 1;
		{
			std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
			shouldStep = _coreState.pendingStepInstruction;
			stepCount = _coreState.pendingStepCount;
			_coreState.pendingStepInstruction = false;
		}

		if(shouldStep) {
			IConsole* console = _emu->GetConsoleUnsafe();
			if(console) {
				DebuggerRequest dbgRequest = _emu->GetDebugger(true);
				Debugger* dbg = dbgRequest.GetDebugger();
				if(dbg) {
					ConsoleType ct = console->GetConsoleType();
					CpuType cpuType = GetMainCpuType(ct);
					dbg->Step(cpuType, stepCount, StepType::Step);
					console->RunFrame();  // blocks in SleepUntilResume after N instructions
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
		case McpCommandType::Reset: return ExecReset(cmd);
		case McpCommandType::Run: return ExecRun(cmd);
		case McpCommandType::Pause: return ExecPause(cmd);
		case McpCommandType::GetState: return ExecGetState(cmd);
		case McpCommandType::SetBreakpoints: return ExecSetBreakpoints(cmd);
		case McpCommandType::StepInstruction: return ExecStepInstruction(cmd);
		case McpCommandType::Continue: return ExecContinue(cmd);
		case McpCommandType::GetCpuState: return ExecGetCpuState(cmd);
		case McpCommandType::GetTrace: return ExecGetTrace(cmd);
		case McpCommandType::ScanVariablesInternal: return ExecScanVariablesInternal(cmd);
		case McpCommandType::GetCallstack: return ExecGetCallstack(cmd);
		case McpCommandType::SetWriteLog: return ExecSetWriteLog(cmd);
		case McpCommandType::GetWriteLog: return ExecGetWriteLog(cmd);
		case McpCommandType::SetSpeed: return ExecSetSpeed(cmd);
		case McpCommandType::StartControlFlowTrace: return ExecStartControlFlowTrace(cmd);
		case McpCommandType::StopControlFlowTrace: return ExecStopControlFlowTrace(cmd);
		case McpCommandType::GetControlFlowTraceStats: return ExecGetControlFlowTraceStats(cmd);
		case McpCommandType::WriteMemoryBlock: return ExecWriteMemoryBlock(cmd);
		case McpCommandType::SaveStateFile: return ExecSaveStateFile(cmd);
		case McpCommandType::LoadStateFile: return ExecLoadStateFile(cmd);
		case McpCommandType::GetNesRuntimeState: return ExecGetNesRuntimeState(cmd);
		case McpCommandType::QueueInputSequence: return ExecQueueInputSequence(cmd);
		case McpCommandType::ClearInputSequence: return ExecClearInputSequence(cmd);
		case McpCommandType::GetInputSequenceStatus: return ExecGetInputSequenceStatus(cmd);
		case McpCommandType::WaitForBreak: return ExecWaitForBreak(cmd);
		case McpCommandType::StartOracleCapture: return ExecStartOracleCapture(cmd);
		case McpCommandType::StopOracleCapture: return ExecStopOracleCapture(cmd);
		case McpCommandType::GetOracleCaptureStatus: return ExecGetOracleCaptureStatus(cmd);
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

std::string McpServer::ExecLoadRomDirect(McpTypedCommand& cmd)
{
	if(cmd.path.empty()) return ErrorResponse(cmd.id, "missing path");
	_coreState.phase = EmuPhase::LoadingRom;
	_coreState.freeRunning = false;
	bool loaded = _emu->LoadRom((VirtualFile)cmd.path, VirtualFile());
	_inputProviderRegistered = false;
	_coreState.phase = loaded ? EmuPhase::Running : EmuPhase::Error;
	return loaded ? OkResponse(cmd.id, R"({"loaded":true})") : ErrorResponse(cmd.id, "unable to load ROM");
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
	// Synchronous: runs in emu thread (DrainCommandQueue), returns data directly.
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	// Resolve memory type: default=CPU bus, or explicit type for PPU/CHR/etc.
	MemoryType memType = GetCpuMemoryType(console->GetConsoleType());
	int maxAddr = 0x10000; // CPU address space

	if(!cmd.memoryType.empty()) {
		if(cmd.memoryType == "chrRam" || cmd.memoryType == "NesChrRam") {
			memType = MemoryType::NesChrRam;
			maxAddr = 0x2000; // 8KB CHR-RAM
		} else if(cmd.memoryType == "ppuMemory" || cmd.memoryType == "NesPpuMemory") {
			memType = MemoryType::NesPpuMemory;
			maxAddr = 0x4000; // 16KB PPU address space
		} else if(cmd.memoryType == "nametableRam" || cmd.memoryType == "NesNametableRam") {
			memType = MemoryType::NesNametableRam;
			maxAddr = 0x1000; // 4KB nametable
		} else if(cmd.memoryType == "paletteRam" || cmd.memoryType == "NesPaletteRam") {
			memType = MemoryType::NesPaletteRam;
			maxAddr = 0x20; // 32 bytes
		} else if(cmd.memoryType == "spriteRam" || cmd.memoryType == "NesSpriteRam") {
			memType = MemoryType::NesSpriteRam;
			maxAddr = 0x100; // 256 bytes OAM
		} else if(cmd.memoryType == "cpu" || cmd.memoryType == "NesMemory") {
			// default, already set
		} else {
			return ErrorResponse(cmd.id, "unknown memory_type: " + cmd.memoryType);
		}
	}

	if(cmd.address < 0 || cmd.address >= maxAddr) {
		return ErrorResponse(cmd.id, "invalid address for memory type");
	}

	int size = cmd.count;
	if(size < 1) size = 1;
	if(size > 2048) size = 2048;
	if(cmd.address + size > maxAddr) size = maxAddr - cmd.address;

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	MemoryDumper* dumper = dbg->GetMemoryDumper();
	std::ostringstream result;
	result << "{\"address\":" << cmd.address << ",\"size\":" << size << ",\"data\":[";
	for(int i = 0; i < size; i++) {
		if(i > 0) result << ",";
		result << (int)dumper->GetMemoryValue(memType, (uint32_t)(cmd.address + i));
	}
	result << "]}";

	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecWriteMemory(McpTypedCommand& cmd)
{
	// Synchronous: runs in emu thread (DrainCommandQueue), writes and confirms.
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	if(cmd.address < 0 || cmd.address > 0xFFFF) {
		return ErrorResponse(cmd.id, "invalid address");
	}

	if(cmd.value < 0 || cmd.value > 255) {
		return ErrorResponse(cmd.id, "value must be 0-255");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	MemoryType memType = GetCpuMemoryType(console->GetConsoleType());
	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	dbg->GetMemoryDumper()->SetMemoryValue(memType, (uint32_t)cmd.address, (uint8_t)cmd.value);
	return OkResponse(cmd.id, R"({"written":true})");
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

std::string McpServer::ExecReset(McpTypedCommand& cmd)
{
	// Only accept reset when running
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	// Declare intention - do NOT execute
	{
		std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
		_coreState.pendingReset = true;
	}

	// Return immediately - reset executes in emu thread
	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecRun(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	_coreState.freeRunning = true;

	DebuggerRequest dbgRequest = _emu->GetDebugger(false);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(dbg && dbg->IsExecutionStopped()) {
		dbg->Run();
	}

	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecSetSpeed(McpTypedCommand& cmd)
{
	int speed = cmd.speed;
	if(speed < 0 || speed > 5000) {
		return ErrorResponse(cmd.id, "speed out of range (0-5000)");
	}

	EmuSettings* settings = _emu->GetSettings();
	if(speed == 0) {
		settings->SetFlag(EmulationFlags::MaximumSpeed);
	} else {
		settings->ClearFlag(EmulationFlags::MaximumSpeed);
		EmulationConfig cfg = settings->GetEmulationConfig();
		cfg.EmulationSpeed = (uint32_t)speed;
		settings->SetEmulationConfig(cfg);
	}

	std::string result = "{\"accepted\":true,\"speed\":" + std::to_string(speed) + "}";
	return OkResponse(cmd.id, result);
}

std::string McpServer::ExecStartControlFlowTrace(McpTypedCommand& cmd)
{
	if(cmd.path.empty()) {
		return ErrorResponse(cmd.id, "path is required");
	}
	shared_ptr<IConsole> console = _emu->GetConsole();
	NesConsole* nes = dynamic_cast<NesConsole*>(console.get());
	if(!nes) {
		return ErrorResponse(cmd.id, "no NES ROM loaded");
	}
	if(cmd.deduplicate && cmd.summaryPath.empty()) {
		return ErrorResponse(cmd.id, "summary_path is required when deduplicate is true");
	}
	nes->StartControlFlowTrace(cmd.path, cmd.deduplicate, cmd.summaryPath,
		(uint32_t)cmd.eventMask, cmd.graphDeduplicate);
	std::string result = "{\"accepted\":true,\"path\":\"" + cmd.path + "\"";
	if(cmd.deduplicate) {
		result += ",\"deduplicate\":true,\"summary_path\":\"" + cmd.summaryPath + "\"";
	}
	result += "}";
	return OkResponse(cmd.id, result);
}

std::string McpServer::ExecStopControlFlowTrace(McpTypedCommand& cmd)
{
	shared_ptr<IConsole> console = _emu->GetConsole();
	NesConsole* nes = dynamic_cast<NesConsole*>(console.get());
	if(!nes) {
		return ErrorResponse(cmd.id, "no NES ROM loaded");
	}
	nes->StopControlFlowTrace();
	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecGetControlFlowTraceStats(McpTypedCommand& cmd)
{
	shared_ptr<IConsole> console = _emu->GetConsole();
	NesConsole* nes = dynamic_cast<NesConsole*>(console.get());
	if(!nes) {
		return ErrorResponse(cmd.id, "no NES ROM loaded");
	}
	NesControlFlowTracer* tracer = nes->GetControlFlowTracer();
	if(!tracer || !tracer->IsEnabled()) {
		return ErrorResponse(cmd.id, "no control-flow trace is active");
	}
	auto s = tracer->ConsumeStats();
	char buf[256];
	int n = snprintf(buf, sizeof(buf),
		"{\"eventsSeen\":%llu,\"uniqueEvents\":%llu,\"newEventsSinceLastStats\":%llu}",
		(unsigned long long)s.eventsSeen,
		(unsigned long long)s.uniqueEvents,
		(unsigned long long)s.newEventsSinceLastStats);
	return OkResponse(cmd.id, std::string(buf, n));
}

std::string McpServer::ExecPause(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	// Use the same path as the UI's Pause shortcut. Emulator::Pause() does:
	//   if (debugger) debugger->Step(1, Step, BreakSource::Pause);  else _paused = true;
	// We force the debugger to exist first (GetDebugger(true)) so we get the
	// Step-based pause that sets _executionStopped, not the _paused fallback
	// (which doesn't update IsExecutionStopped()).
	//
	// Do NOT touch _coreState.freeRunning — IsExternalControlled would gate the
	// emu loop and the queued Step(1) instruction would never execute.
	{
		DebuggerRequest dbgRequest = _emu->GetDebugger(true);
		(void)dbgRequest;
	}
	_emu->Pause();

	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecGetState(McpTypedCommand& cmd)
{
	// A ROM supplied on Mesen's command line is loaded by the UI, outside the
	// MCP load_rom command. Adopt it only once startup has finished far enough
	// for debugger creation to succeed. This preserves UI navigation without a
	// second, racing ROM load and closes the external MCP clock gate immediately.
	if(_coreState.phase == EmuPhase::Idle && _emu->IsRunning()) {
		DebuggerRequest readyRequest = _emu->GetDebugger(true);
		if(readyRequest.GetDebugger()) {
			_coreState.freeRunning = false;
			_coreState.phase = EmuPhase::Running;
			_inputProviderRegistered = false;
		}
	}

	std::ostringstream result;
	EmuPhase phase = _coreState.phase.load();
	bool consoleLoaded = _emu->IsRunning();
	DebuggerRequest dbgRequest = _emu->GetDebugger(false);
	Debugger* dbg = dbgRequest.GetDebugger();
	bool debuggerAvailable = dbg != nullptr;
	bool debuggerStopped = dbg && dbg->IsExecutionStopped();
	bool mcpReady = phase == EmuPhase::Running && consoleLoaded && debuggerAvailable;
	bool clockGated = phase == EmuPhase::Running && !_coreState.freeRunning;

	const char* status = "idle_no_rom";
	if(phase == EmuPhase::Error) {
		status = "error";
	} else if(phase == EmuPhase::LoadingRom) {
		status = "loading_rom";
	} else if(phase == EmuPhase::Running && !consoleLoaded) {
		status = "running_without_console";
	} else if(consoleLoaded && !debuggerAvailable) {
		status = "waiting_for_debugger";
	} else if(mcpReady && debuggerStopped) {
		status = "ready_debugger_stopped";
	} else if(mcpReady && clockGated) {
		status = "ready_paused";
	} else if(mcpReady) {
		status = "ready_running";
	}

	result << "{\"phase\":\"";

	switch(phase) {
		case EmuPhase::Idle:       result << "idle"; break;
		case EmuPhase::LoadingRom: result << "loading"; break;
		case EmuPhase::Running:    result << "running"; break;
		case EmuPhase::Error:      result << "error"; break;
	}

	result << "\",\"status\":\"" << status << "\""
		<< ",\"console_loaded\":" << (consoleLoaded ? "true" : "false")
		<< ",\"mcp_ready\":" << (mcpReady ? "true" : "false")
		<< ",\"debugger_available\":" << (debuggerAvailable ? "true" : "false")
		<< ",\"debugger_stopped\":";
	if(debuggerAvailable) result << (debuggerStopped ? "true" : "false");
	else result << "null";
	const char* clockOwner = phase != EmuPhase::Running ? "none"
		: (debuggerStopped ? "debugger" : (clockGated ? "mcp" : "emulator"));
	result << ",\"clock_owner\":\"" << clockOwner << "\"";

	// Add mode when running. Report "paused" if either the MCP clock gate is closed
	// (!freeRunning) OR the debugger has broken — both cases mean emu is not advancing.
	if(phase == EmuPhase::Running) {
		bool stopped = clockGated || debuggerStopped;
		result << ",\"mode\":\"" << (stopped ? "paused" : "free") << "\"";
	} else {
		result << ",\"mode\":\"unavailable\"";
	}

	// Always emit frame_count and pc with stable types so clients never need to
	// infer readiness from a field being absent.
	if(consoleLoaded) {
		result << ",\"frame_count\":" << _emu->GetFrameCount();
		if(dbg) {
			ConsoleType ct = _emu->GetConsoleType();
			CpuType cpuType = GetMainCpuType(ct);
			uint32_t pc = dbg->GetProgramCounter(cpuType, false);
			result << ",\"pc\":" << pc;
		} else result << ",\"pc\":null";
	} else result << ",\"frame_count\":0,\"pc\":null";

	if(phase == EmuPhase::Error) result << ",\"error\":\"rom_load_failed\"";

	result << "}";
	return OkResponse(cmd.id, result.str());
}

// ============================================================================
// Debugger Command Implementations
// ============================================================================

std::string McpServer::ExecSetBreakpoints(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	ConsoleType ct = console->GetConsoleType();
	CpuType cpuType = GetMainCpuType(ct);
	MemoryType memType = GetCpuMemoryType(ct);

	// Parse breakpoints JSON array
	// Each element: {"id":N, "address":ADDR, "type":FLAGS, "enabled":true, "startAddress":ADDR, "endAddress":ADDR, "condition":"..."}
	// type uses BreakpointTypeFlags: Read=1, Write=2, Execute=4
	std::vector<Breakpoint> bps;
	std::string arr = cmd.breakpointsJson;
	if(arr.empty()) arr = "[]";

	// Simple parse: find each {...} in the array
	size_t pos = 0;
	while(pos < arr.size()) {
		size_t start = arr.find('{', pos);
		if(start == std::string::npos) break;
		size_t end = arr.find('}', start);
		if(end == std::string::npos) break;

		std::string obj = arr.substr(start, end - start + 1);

		Breakpoint bp;
		bp._id = (uint32_t)ExtractInt(obj, "id", 0);
		bp._cpuType = cpuType;
		std::string memName = ExtractString(obj, "memory_type");
		if(memName == "prg" || memName == "nesPrgRom" || memName == "PrgRom") {
			bp._memoryType = MemoryType::NesPrgRom;
		} else if(memName == "nametableRam" || memName == "nesNametableRam") {
			bp._memoryType = MemoryType::NesNametableRam;
		} else if(memName == "chrRam" || memName == "nesChrRam") {
			bp._memoryType = MemoryType::NesChrRam;
		} else if(memName == "ppuMemory" || memName == "nesPpuMemory") {
			bp._memoryType = MemoryType::NesPpuMemory;
		} else if(memName == "paletteRam" || memName == "nesPaletteRam") {
			bp._memoryType = MemoryType::NesPaletteRam;
		} else if(memName == "spriteRam" || memName == "nesSpriteRam") {
			bp._memoryType = MemoryType::NesSpriteRam;
		} else {
			bp._memoryType = memType;
		}
		bp._type = (BreakpointTypeFlags)ExtractInt(obj, "type", (int)BreakpointTypeFlags::Execute);
		bp._enabled = ExtractInt(obj, "enabled", 1) != 0;
		bp._markEvent = false;
		bp._ignoreDummyOperations = false;
		memset(bp._condition, 0, sizeof(bp._condition));

		int addr = ExtractInt(obj, "address", -1);
		int startAddr = ExtractInt(obj, "startAddress", addr);
		int endAddr = ExtractInt(obj, "endAddress", startAddr);
		bp._startAddr = startAddr;
		bp._endAddr = endAddr;

		std::string condition = ExtractString(obj, "condition");
		if(!condition.empty() && condition.size() < sizeof(bp._condition) - 1) {
			memcpy(bp._condition, condition.c_str(), condition.size());
		}

		bps.push_back(bp);
		pos = end + 1;
	}

	dbg->SetBreakpoints(bps.data(), (uint32_t)bps.size());

	std::ostringstream result;
	result << "{\"count\":" << bps.size() << "}";
	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecStepInstruction(McpTypedCommand& cmd)
{
	// Emu thread path: declare intention, return immediately
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	int count = cmd.count;
	if(count < 1) count = 1;

	{
		std::lock_guard<std::mutex> lock(_coreState.intentionMutex);
		_coreState.pendingStepInstruction = true;
		_coreState.pendingStepCount = count;
	}

	return OkResponse(cmd.id, R"({"accepted":true})");
}

std::string McpServer::ExecStepInstructionDirect(McpTypedCommand& cmd)
{
	// TCP thread path: debugger is already stopped, we can call Step directly
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	ConsoleType ct = console->GetConsoleType();
	CpuType cpuType = GetMainCpuType(ct);

	int count = cmd.count;
	if(count < 1) count = 1;

	// Step() clears _waitForBreakResume, which unblocks the emu thread's SleepUntilResume.
	// The emu thread then runs `count` instructions and calls SleepUntilResume again.
	dbg->Step(cpuType, count, StepType::Step);

	// Wait for execution to stop again (emu thread enters SleepUntilResume)
	int timeout = 5000; // 5 seconds max
	while(!dbg->IsExecutionStopped() && timeout > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		timeout--;
	}

	if(!dbg->IsExecutionStopped()) {
		return ErrorResponse(cmd.id, "step timeout - execution did not stop");
	}

	// Return CPU state after stepping
	return ExecGetCpuState(cmd);
}

std::string McpServer::ExecStepFrameDirect(McpTypedCommand& cmd)
{
	// TCP thread path: debugger is already stopped (e.g. after a BP hit).
	// Use dbg->Step(PpuFrame) so the emu thread runs N frames with normal
	// rendering, then re-enters SleepUntilResume. This is what F8 does in
	// the native UI — and crucially, it keeps the window alive (no black
	// screen) because rendering goes through the normal present path.
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	ConsoleType ct = console->GetConsoleType();
	CpuType cpuType = GetMainCpuType(ct);

	int count = cmd.count;
	if(count < 1) count = 1;
	if(count > 3600) count = 3600;

	dbg->Step(cpuType, count, StepType::PpuFrame);

	// Wait for execution to stop again (5s max — N frames at 60Hz ~ N*16.7ms)
	int timeout = 10000;
	while(!dbg->IsExecutionStopped() && timeout > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		timeout--;
	}

	if(!dbg->IsExecutionStopped()) {
		return ErrorResponse(cmd.id, "step_frame timeout - execution did not stop");
	}

	return ExecGetCpuState(cmd);
}

std::string McpServer::ExecContinue(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	// Set freeRunning so the emu loop keeps running frames
	_coreState.freeRunning = true;

	// Debugger::Run() clears _waitForBreakResume, unblocking SleepUntilResume.
	// Emu resumes running. Will stop again at next breakpoint.
	dbg->Run();

	return OkResponse(cmd.id, R"({"resumed":true})");
}

std::string McpServer::ExecGetCpuState(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	ConsoleType ct = console->GetConsoleType();
	CpuType cpuType = GetMainCpuType(ct);

	// For NES, get the specific state type
	if(ct == ConsoleType::Nes) {
		NesCpuState state = {};
		dbg->GetCpuState(state, cpuType);

		std::ostringstream result;
		result << "{\"cpu_type\":\"nes\""
			<< ",\"pc\":" << state.PC
			<< ",\"a\":" << (int)state.A
			<< ",\"x\":" << (int)state.X
			<< ",\"y\":" << (int)state.Y
			<< ",\"sp\":" << (int)state.SP
			<< ",\"ps\":" << (int)state.PS
			<< ",\"cycle_count\":" << state.CycleCount
			<< ",\"flags\":{"
			<< "\"carry\":" << ((state.PS & 0x01) ? "true" : "false")
			<< ",\"zero\":" << ((state.PS & 0x02) ? "true" : "false")
			<< ",\"interrupt\":" << ((state.PS & 0x04) ? "true" : "false")
			<< ",\"decimal\":" << ((state.PS & 0x08) ? "true" : "false")
			<< ",\"overflow\":" << ((state.PS & 0x40) ? "true" : "false")
			<< ",\"negative\":" << ((state.PS & 0x80) ? "true" : "false")
			<< "}}";
		return OkResponse(cmd.id, result.str());
	}

	// Generic fallback: just return PC
	uint32_t pc = dbg->GetProgramCounter(cpuType, false);
	std::ostringstream result;
	result << "{\"cpu_type\":\"generic\",\"pc\":" << pc << "}";
	return OkResponse(cmd.id, result.str());
}

std::string McpServer::ExecGetTrace(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	ConsoleType ct = console->GetConsoleType();
	CpuType cpuType = GetMainCpuType(ct);

	// Auto-enable trace logging on first call
	if(!_coreState.traceEnabled) {
		ITraceLogger* logger = dbg->GetTraceLogger(cpuType);
		if(logger) {
			TraceLoggerOptions opts = {};
			opts.Enabled = true;
			opts.IndentCode = false;
			opts.UseLabels = false;
			memset(opts.Condition, 0, sizeof(opts.Condition));
			memset(opts.Format, 0, sizeof(opts.Format));
			// Minimal format: just the disassembly. PC + cycle come from
			// TraceRow.ProgramCounter and ByteCode separately.
			static constexpr char fmt[] = "[Disassembly]";
			static_assert(sizeof(fmt) <= sizeof(opts.Format), "trace format buffer too small");
			memcpy(opts.Format, fmt, sizeof(fmt));
			logger->SetOptions(opts);
			_coreState.traceEnabled = true;
		}
	}

	int maxCount = cmd.count;
	if(maxCount < 1) maxCount = 1;
	if(maxCount > 30000) maxCount = 30000;

	std::vector<TraceRow> rows(maxCount);
	uint32_t count = dbg->GetExecutionTrace(rows.data(), 0, (uint32_t)maxCount);

	std::ostringstream result;
	result << "{\"count\":" << count << ",\"trace\":[";
	for(uint32_t i = 0; i < count; i++) {
		if(i > 0) result << ",";
		result << "{\"pc\":" << rows[i].ProgramCounter;
		result << ",\"size\":" << (int)rows[i].ByteCodeSize;
		result << ",\"bytes\":[";
		for(int b = 0; b < rows[i].ByteCodeSize && b < 8; b++) {
			if(b > 0) result << ",";
			result << (int)rows[i].ByteCode[b];
		}
		result << "]";

		// Add disassembly from LogOutput (escape for JSON)
		std::string logStr(rows[i].LogOutput, rows[i].LogSize);
		result << ",\"disassembly\":\"";
		for(char c : logStr) {
			if(c == '"') result << "\\\"";
			else if(c == '\\') result << "\\\\";
			else if(c == '\n') result << "\\n";
			else if(c == '\r') result << "\\r";
			else if(c == '\t') result << "\\t";
			else if((unsigned char)c >= 0x20) result << c;
		}
		result << "\"}";
	}
	result << "]}";
	return OkResponse(cmd.id, result.str());
}

// ============================================================================
// Native Analysis Commands — run entirely in emu thread, zero TCP round-trips
// ============================================================================

std::string McpServer::ExecScanVariablesInternal(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	// NES-only: direct RAM access
	if(console->GetConsoleType() != ConsoleType::Nes) {
		return ErrorResponse(cmd.id, "scan_variables_internal only supports NES");
	}

	NesConsole* nes = dynamic_cast<NesConsole*>(console);
	if(!nes) {
		return ErrorResponse(cmd.id, "failed to get NES console");
	}

	NesMemoryManager* memMgr = nes->GetMemoryManager();
	if(!memMgr) {
		return ErrorResponse(cmd.id, "no memory manager");
	}

	uint8_t* cpuRam = memMgr->GetInternalRam();
	if(!cpuRam) {
		return ErrorResponse(cmd.id, "no internal RAM");
	}

	int buttonMask = cmd.buttons;
	int frames = cmd.frames;
	int trials = cmd.trials;
	if(frames < 1) frames = 1;
	if(frames > 3600) frames = 3600;
	if(trials < 1) trials = 1;
	if(trials > 10) trials = 10;

	const int RAM_SIZE = 0x800;

	// Per-address: accumulate deltas across trials
	// deltas[addr] = vector of signed deltas, one per trial where it changed
	struct AddrInfo {
		std::vector<int> deltas;
	};
	std::vector<AddrInfo> addrInfo(RAM_SIZE);

	BaseControlManager* ctrlMgr = console->GetControlManager();
	shared_ptr<BaseControlDevice> controller;
	if(ctrlMgr) {
		controller = ctrlMgr->GetControlDevice(0, 0);
	}

	for(int trial = 0; trial < trials; trial++) {
		// Snapshot before
		uint8_t ramBefore[0x800];
		memcpy(ramBefore, cpuRam, RAM_SIZE);

		// Apply input and run frames
		if(controller) {
			ControlDeviceState state;
			state.State.push_back((uint8_t)(buttonMask & 0xFF));
			controller->SetRawState(state);
			controller->RefreshStateBuffer();
		}

		for(int f = 0; f < frames; f++) {
			// Re-apply input each frame (Mesen clears it)
			if(controller) {
				ControlDeviceState state;
				state.State.push_back((uint8_t)(buttonMask & 0xFF));
				controller->SetRawState(state);
				controller->RefreshStateBuffer();
			}
			console->RunFrame();
		}

		// Clear input
		if(controller) {
			ControlDeviceState state;
			state.State.push_back(0);
			controller->SetRawState(state);
			controller->RefreshStateBuffer();
		}

		// Snapshot after and diff
		for(int addr = 0; addr < RAM_SIZE; addr++) {
			int delta = (int)cpuRam[addr] - (int)ramBefore[addr];
			if(delta != 0) {
				if(delta > 127) delta -= 256;
				if(delta < -128) delta += 256;
				addrInfo[addr].deltas.push_back(delta);
			}
		}
	}

	// Build candidates: only addresses that changed in ALL trials
	struct Candidate {
		int addr;
		std::vector<int> deltas;
		double avgDelta;
		bool allSame;
		std::string pattern;
		std::string stability;
	};
	std::vector<Candidate> candidates;

	for(int addr = 0; addr < RAM_SIZE; addr++) {
		auto& info = addrInfo[addr];
		if((int)info.deltas.size() < trials) continue;

		Candidate c;
		c.addr = addr;
		c.deltas = info.deltas;

		double sum = 0;
		for(int d : c.deltas) sum += d;
		c.avgDelta = sum / c.deltas.size();

		c.allSame = true;
		for(size_t i = 1; i < c.deltas.size(); i++) {
			if(c.deltas[i] != c.deltas[0]) { c.allSame = false; break; }
		}

		if(c.allSame) {
			c.stability = "high";
			c.pattern = c.deltas[0] > 0 ? "consistent_increment" :
			            c.deltas[0] < 0 ? "consistent_decrement" : "no_change";
		} else {
			c.stability = "medium";
			bool allPos = true, allNeg = true;
			for(int d : c.deltas) {
				if(d <= 0) allPos = false;
				if(d >= 0) allNeg = false;
			}
			c.pattern = allPos ? "variable_increment" :
			            allNeg ? "variable_decrement" : "variable";
		}

		candidates.push_back(c);
	}

	// Sort: high stability first, then by address
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		if(a.stability != b.stability) return a.stability == "high";
		return a.addr < b.addr;
	});

	// Serialize JSON once
	std::ostringstream result;
	result << "{\"button\":" << buttonMask
	       << ",\"frames_per_trial\":" << frames
	       << ",\"trials\":" << trials
	       << ",\"total_candidates\":" << candidates.size()
	       << ",\"candidates\":[";

	for(size_t i = 0; i < candidates.size(); i++) {
		if(i > 0) result << ",";
		auto& c = candidates[i];
		result << "{\"address\":\"0x";
		// 4-digit hex
		char hexBuf[8];
		snprintf(hexBuf, sizeof(hexBuf), "%04X", c.addr);
		result << hexBuf << "\"";
		result << ",\"deltas\":[";
		for(size_t j = 0; j < c.deltas.size(); j++) {
			if(j > 0) result << ",";
			result << c.deltas[j];
		}
		result << "]";
		result << ",\"delta_avg\":" << std::fixed;
		result.precision(2);
		result << c.avgDelta;
		result << ",\"stability\":\"" << c.stability << "\"";
		result << ",\"pattern\":\"" << c.pattern << "\"";
		result << "}";
	}

	result << "]}";
	return OkResponse(cmd.id, result.str());
}

// ============================================================================
// Callstack + Write Log — for narrate_frames tool
// ============================================================================

std::string McpServer::ExecGetCallstack(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console) {
		return ErrorResponse(cmd.id, "no console");
	}

	DebuggerRequest dbgRequest = _emu->GetDebugger(true);
	Debugger* dbg = dbgRequest.GetDebugger();
	if(!dbg) {
		return ErrorResponse(cmd.id, "debugger unavailable");
	}

	CpuType cpuType = GetMainCpuType(console->GetConsoleType());
	CallstackManager* mgr = dbg->GetCallstackManager(cpuType);
	if(!mgr) {
		return ErrorResponse(cmd.id, "no callstack manager");
	}

	// CallstackManager::GetCallstack writes ALL frames into the buffer regardless
	// of size — internal limit is 511 (see CallstackManager::Push). Buffer must
	// be >= 512 to avoid stack overflow / heap corruption.
	std::vector<StackFrameInfo> framesBuf(512);
	uint32_t size = 0;
	mgr->GetCallstack(framesBuf.data(), size);
	if(size > 512) size = 512;
	// Truncate response to a sensible top-of-stack subset to keep JSON compact
	uint32_t reportSize = size > 64 ? 64 : size;
	uint32_t firstIdx = size > reportSize ? size - reportSize : 0;

	std::ostringstream r;
	r << "{\"count\":" << size << ",\"reported\":" << reportSize << ",\"frames\":[";
	bool firstEmitted = true;
	for(uint32_t i = firstIdx; i < size; i++) {
		if(!firstEmitted) r << ",";
		firstEmitted = false;
		const char* flag = "none";
		if((int)framesBuf[i].Flags & (int)StackFrameFlags::Nmi) flag = "nmi";
		else if((int)framesBuf[i].Flags & (int)StackFrameFlags::Irq) flag = "irq";
		r << "{\"source\":" << framesBuf[i].Source
		  << ",\"target\":" << framesBuf[i].Target
		  << ",\"return\":" << framesBuf[i].Return
		  << ",\"return_sp\":" << framesBuf[i].ReturnStackPointer
		  << ",\"flags\":\"" << flag << "\"}";
	}
	r << "]}";
	return OkResponse(cmd.id, r.str());
}

std::string McpServer::ExecSetWriteLog(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) {
		return ErrorResponse(cmd.id, "emulator not running");
	}

	int s = cmd.startAddr & 0xFFFF;
	int e = cmd.endAddr & 0xFFFF;
	if(e < s) std::swap(s, e);

	int rawCap = cmd.count;
	if(rawCap < 1024) rawCap = 1024;
	if(rawCap > 1024 * 1024) rawCap = 1024 * 1024;

	McpWriteLog::Instance().Configure((uint16_t)s, (uint16_t)e, cmd.enabled, (size_t)rawCap);

	std::ostringstream r;
	r << "{\"enabled\":" << (cmd.enabled ? "true" : "false")
	  << ",\"start\":" << s
	  << ",\"end\":" << e
	  << ",\"cap\":" << rawCap << "}";
	return OkResponse(cmd.id, r.str());
}

std::string McpServer::ExecGetWriteLog(McpTypedCommand& cmd)
{
	int maxN = cmd.count;
	if(maxN < 1) maxN = 1;
	if(maxN > 100000) maxN = 100000;
	bool clear = cmd.enabled;

	std::vector<McpWriteLogEntry> entries;
	bool overflow = false;
	uint64_t dropped = 0;
	McpWriteLog::Instance().Drain(entries, (size_t)maxN, clear, overflow, dropped);

	std::ostringstream r;
	r << "{\"count\":" << entries.size()
	  << ",\"overflow\":" << (overflow ? "true" : "false")
	  << ",\"dropped\":" << dropped
	  << ",\"entries\":[";
	for(size_t i = 0; i < entries.size(); i++) {
		if(i > 0) r << ",";
		r << "{\"cycle\":" << entries[i].Cycle
		  << ",\"frame\":" << entries[i].Frame
		  << ",\"pc\":" << entries[i].Pc
		  << ",\"addr\":" << entries[i].Addr
		  << ",\"value\":" << (int)entries[i].Value
		  << ",\"prg_pc\":" << entries[i].PrgPc
		  << ",\"prg_addr\":" << entries[i].PrgAddr
		  << "}";
	}
	r << "]}";
	return OkResponse(cmd.id, r.str());
}

// ============================================================================
// IInputProvider — called by Mesen's input system every frame
// ============================================================================

bool McpServer::SetInput(BaseControlDevice* device)
{
	{
		std::lock_guard<std::mutex> lock(_coreState.inputSequenceMutex);
		if(_coreState.inputSequenceEnabled && device->GetPort() == (uint8_t)_coreState.inputSequencePort) {
			uint32_t frame = _emu->GetFrameCount();
			if(frame != _coreState.inputSequenceLastFrame) {
				if(_coreState.inputSequenceIndex >= _coreState.inputSequence.size()) {
					_coreState.inputSequenceEnabled = false;
					return false;
				}
				_coreState.inputSequenceCurrentButtons = _coreState.inputSequence[_coreState.inputSequenceIndex++];
				_coreState.inputSequenceLastFrame = frame;
			}
			ControlDeviceState state;
			state.State.push_back(_coreState.inputSequenceCurrentButtons);
			device->SetRawState(state);
			device->RefreshStateBuffer();
			return true;
		}
	}
	if(_coreState.stickyInputPort < 0) return false;
	if(device->GetPort() != (uint8_t)_coreState.stickyInputPort) return false;

	// ponytail: OR sticky bits onto human state ONLY for standard pads,
	// so keyboard + MCP can drive the controller together. Other devices
	// (zapper, keyboard, mouse, system action manager) keep the legacy replace
	// behavior to avoid corrupting their custom state layout.
	ControllerType type = device->GetControllerType();
	bool isStandardPad =
		type == ControllerType::NesController ||
		type == ControllerType::FamicomController ||
		type == ControllerType::FamicomControllerP2 ||
		type == ControllerType::SnesController;

	uint8_t sticky = (uint8_t)(_coreState.stickyInputButtons & 0xFF);
	if(isStandardPad) {
		ControlDeviceState current = device->GetRawState();
		if(current.State.empty()) current.State.push_back(sticky);
		else current.State[0] |= sticky;
		device->SetRawState(current);
	} else {
		ControlDeviceState state;
		state.State.push_back(sticky);
		device->SetRawState(state);
	}
	device->RefreshStateBuffer();
	return true;
}
