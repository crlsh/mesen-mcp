#pragma once
#include "pch.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <string>
#include <vector>

class Emulator;
class Socket;

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
	GetState
};

struct McpTypedCommand {
	McpCommandType type;
	int id = 0;

	// Params (used depending on type)
	std::string path;        // load_rom
	int address = 0;         // read_memory, write_memory
	int value = 0;           // write_memory
	int count = 1;           // step_frame, read_memory (size)
	int port = 0;            // set_input
	int buttons = 0;         // set_input

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
	bool pendingReset = false;  // reset intention

	// Memory operation intentions (SINGLE-SLOT semantics)
	// ===============================================
	// read_memory() and write_memory() use single-slot async intention model:
	// - Each call sets pendingReadMemory/pendingWriteMemory intention flag
	// - ExecutePendingIntentions() executes in emu thread
	// - Concurrent calls overwrite: last intention wins (acceptable for single-client MCP)
	// - Operations execute WITHOUT stepping: write_memory() executes on next emu loop tick
	//
	// Contract:
	//   read_memory(addr)  → Returns {"accepted": true} immediately
	//   get_state()        → Returns {... "last_read": value} ONE-SHOT (then cleared)
	//   write_memory(addr, val) → Returns {"accepted": true} immediately
	//   (no "last_write" returned; write just executes)
	//
	// Validation:
	//   - read_memory: address must be 0x0000 - 0xFFFF (16-bit CPU bus)
	//   - write_memory: address must be 0x0000 - 0xFFFF, value must be 0-255
	//   - Invalid input → error response (no intention queued)
	//
	// State cleanup:
	//   - On ROM load (ExecutePendingIntentions), all memory read state cleared
	//   - Prevents stale "last_read" from previous ROM leaking into new ROM
	bool pendingReadMemory = false;
	bool pendingWriteMemory = false;
	uint32_t readAddress = 0;
	uint32_t writeAddress = 0;
	uint8_t writeValue = 0;

	// Memory read result (ONE-SHOT: consumed by get_state(), then cleared)
	// get_state() returns "last_read" only ONCE after read_memory() executes.
	// Second call to get_state() will NOT include "last_read" (one-shot semantics).
	uint8_t lastReadValue = 0;
	bool readReady = false;

	std::mutex intentionMutex;
};

class McpServer {
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

	// TCP thread only
	void ListenLoop();
	void HandleClient(std::unique_ptr<Socket> client);
	std::shared_ptr<McpTypedCommand> ParseCommand(const std::string& json);
	static std::string ExtractString(const std::string& json, const std::string& key);
	static int ExtractInt(const std::string& json, const std::string& key, int defaultVal = 0);

	// Core thread only
	std::string ExecuteCommand(McpTypedCommand& cmd);
	std::string ExecLoadRom(McpTypedCommand& cmd);
	std::string ExecStepFrame(McpTypedCommand& cmd);
	std::string ExecReadMemory(McpTypedCommand& cmd);
	std::string ExecWriteMemory(McpTypedCommand& cmd);
	std::string ExecSetInput(McpTypedCommand& cmd);
	std::string ExecReset(McpTypedCommand& cmd);
	std::string ExecGetState(McpTypedCommand& cmd);
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

	bool IsExternalControlled() const { return _coreState.phase == EmuPhase::Running; }
	McpCoreState& GetCoreState() { return _coreState; }
};
