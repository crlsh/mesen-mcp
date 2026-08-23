#pragma once
#include "pch.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <string>
#include <vector>
#include "Shared/Interfaces/IInputProvider.h"

class Emulator;
class Socket;
class BaseControlDevice;

// Emulator phase state machine
enum class EmuPhase {
	Idle,
	LoadingRom,
	Running,
	Error
};

// Typed command — core thread never sees JSON
enum class McpCommandType {
	LoadRom,
	StepFrame,
	ReadMemory,
	WriteMemory,
	SetInput,
	Reset,
	GetState,
	Run,
	Pause,
	// Debugger commands
	SetBreakpoints,
	StepInstruction,
	Continue,
	GetCpuState,
	GetTrace,
	ScanVariablesInternal,
	GetCallstack,
	SetWriteLog,
	GetWriteLog,
	SetSpeed,
	StartControlFlowTrace,
	StopControlFlowTrace,
	GetControlFlowTraceStats,
	WriteMemoryBlock,
	SaveStateFile,
	LoadStateFile,
	GetNesRuntimeState,
	QueueInputSequence,
	ClearInputSequence,
	GetInputSequenceStatus,
	WaitForBreak,
	StartOracleCapture,
	StopOracleCapture,
	GetOracleCaptureStatus
};

struct McpTypedCommand {
	McpCommandType type;
	int id = 0;

	// Params (used depending on type)
	std::string path;        // load_rom
	int address = 0;         // read_memory, write_memory
	int value = 0;           // write_memory
	int count = 1;           // step_frame, read_memory (size), step_instruction, get_trace
	std::string memoryType;  // read_memory: optional, e.g. "chrRam", "ppuMemory"
	int port = 0;            // set_input
	int buttons = 0;         // set_input
	std::string breakpointsJson;  // set_breakpoints: raw JSON array
	int frames = 30;             // scan_variables_internal
	int trials = 3;              // scan_variables_internal
	int startAddr = 0;           // set_write_log: range start
	int endAddr = 0xFFFF;        // set_write_log: range end
	bool enabled = false;        // set_write_log: toggle / get_write_log: drain flag
	int speed = 100;             // set_speed: 0=unlimited, 100=normal, up to 5000
	bool deduplicate = false;    // start_control_flow_trace: online dedup
	int eventMask = 0x3FF;
	bool graphDeduplicate = false;
	std::string summaryPath;     // start_control_flow_trace: where to write the dedup summary on Stop()
	std::vector<int> values;     // block writes / frame-indexed input

	// Response channel: core sets, TCP thread waits (hard 30s timeout)
	std::string response;
	bool responseReady = false;
	std::mutex mutex;
	std::condition_variable cv;

	void SetResponse(const std::string& resp) {
		std::lock_guard<std::mutex> lock(mutex);
		response = resp;
		responseReady = true;
		cv.notify_one();
	}

	std::string WaitForResponse() {
		std::unique_lock<std::mutex> lock(mutex);
		bool ok = cv.wait_for(lock, std::chrono::seconds(30), [this] { return responseReady; });
		if(!ok) return "{\"ok\":false,\"error\":\"timeout\",\"id\":0}";
		return response;
	}
};

// Core state — phase machine with pending intentions
struct McpCoreState {
	std::atomic<EmuPhase> phase{EmuPhase::Idle};
	std::string pendingRomPath;
	int pendingFrameCount = 0;  // step_frame intention
	int pendingInputPort = -1;  // set_input intention (-1 = no pending input)
	int pendingInputButtons = 0;  // set_input intention

	// Sticky input: re-applied every frame until cleared by set_input(0)
	int stickyInputPort = -1;
	int stickyInputButtons = 0;
	bool pendingReset = false;  // reset intention
	bool freeRunning = false;   // true = emu runs frames normally, false = step_frame only

	// Debugger intentions
	bool pendingStepInstruction = false;
	int pendingStepCount = 1;
	bool traceEnabled = false;

	std::mutex intentionMutex;
	std::mutex inputSequenceMutex;
	std::vector<uint8_t> inputSequence;
	size_t inputSequenceIndex = 0;
	uint32_t inputSequenceLastFrame = UINT32_MAX;
	uint8_t inputSequenceCurrentButtons = 0;
	int inputSequencePort = 0;
	bool inputSequenceEnabled = false;
};

class McpServer : public IInputProvider {
private:
	Emulator* _emu;
	std::unique_ptr<std::thread> _listenThread;
	std::unique_ptr<Socket> _listener;
	std::atomic<bool> _stop;
	uint16_t _port;

	// Command queue: TCP thread enqueues, core thread drains
	std::mutex _queueMutex;
	std::queue<std::shared_ptr<McpTypedCommand>> _commandQueue;

	McpCoreState _coreState;
	bool _inputProviderRegistered = false;

	// TCP thread only
	void ListenLoop();
	void HandleClient(std::unique_ptr<Socket> client);
	std::shared_ptr<McpTypedCommand> ParseCommand(const std::string& json);
	static std::string ExtractString(const std::string& json, const std::string& key);
	static int ExtractInt(const std::string& json, const std::string& key, int defaultVal = 0);
	static std::string ExtractJsonArray(const std::string& json, const std::string& key);
	static std::vector<int> ExtractIntArray(const std::string& json, const std::string& key);

	// Dual-path execution: direct on TCP thread when debugger is stopped
	bool CanExecuteDirect(McpCommandType type);
	std::string ExecuteCommandDirect(McpTypedCommand& cmd);

	// Core thread only (emu thread queue path)
	std::string ExecuteCommand(McpTypedCommand& cmd);
	std::string ExecLoadRom(McpTypedCommand& cmd);
	std::string ExecLoadRomDirect(McpTypedCommand& cmd);
	std::string ExecStepFrame(McpTypedCommand& cmd);
	std::string ExecReadMemory(McpTypedCommand& cmd);
	std::string ExecWriteMemory(McpTypedCommand& cmd);
	std::string ExecSetInput(McpTypedCommand& cmd);
	std::string ExecReset(McpTypedCommand& cmd);
	std::string ExecRun(McpTypedCommand& cmd);
	std::string ExecPause(McpTypedCommand& cmd);
	std::string ExecGetState(McpTypedCommand& cmd);

	// Debugger commands (can run on either thread depending on CanExecuteDirect)
	std::string ExecSetBreakpoints(McpTypedCommand& cmd);
	std::string ExecStepInstruction(McpTypedCommand& cmd);
	std::string ExecStepInstructionDirect(McpTypedCommand& cmd);
	std::string ExecStepFrameDirect(McpTypedCommand& cmd);
	std::string ExecContinue(McpTypedCommand& cmd);
	std::string ExecGetCpuState(McpTypedCommand& cmd);
	std::string ExecGetTrace(McpTypedCommand& cmd);
	std::string ExecScanVariablesInternal(McpTypedCommand& cmd);
	std::string ExecGetCallstack(McpTypedCommand& cmd);
	std::string ExecSetWriteLog(McpTypedCommand& cmd);
	std::string ExecGetWriteLog(McpTypedCommand& cmd);
	std::string ExecSetSpeed(McpTypedCommand& cmd);
	std::string ExecStartControlFlowTrace(McpTypedCommand& cmd);
	std::string ExecStopControlFlowTrace(McpTypedCommand& cmd);
	std::string ExecGetControlFlowTraceStats(McpTypedCommand& cmd);
	std::string ExecWriteMemoryBlock(McpTypedCommand& cmd);
	std::string ExecSaveStateFile(McpTypedCommand& cmd);
	std::string ExecLoadStateFile(McpTypedCommand& cmd);
	std::string ExecGetNesRuntimeState(McpTypedCommand& cmd);
	std::string ExecQueueInputSequence(McpTypedCommand& cmd);
	std::string ExecClearInputSequence(McpTypedCommand& cmd);
	std::string ExecGetInputSequenceStatus(McpTypedCommand& cmd);
	std::string ExecWaitForBreak(McpTypedCommand& cmd);
	std::string ExecStartOracleCapture(McpTypedCommand& cmd);
	std::string ExecStopOracleCapture(McpTypedCommand& cmd);
	std::string ExecGetOracleCaptureStatus(McpTypedCommand& cmd);

	static std::string OkResponse(int id, const std::string& resultJson);
	static std::string ErrorResponse(int id, const std::string& error);

public:
	McpServer(Emulator* emu, uint16_t port = 12345);
	~McpServer();

	void Start();
	void Stop();

	// Called from emulation thread to drain and execute queued commands
	void DrainCommandQueue();
	void ExecutePendingIntentions();

	bool IsExternalControlled() const { return _coreState.phase == EmuPhase::Running && !_coreState.freeRunning; }
	McpCoreState& GetCoreState() { return _coreState; }

	// IInputProvider — overrides controller state when MCP has sticky input
	bool SetInput(BaseControlDevice* device) override;
};
