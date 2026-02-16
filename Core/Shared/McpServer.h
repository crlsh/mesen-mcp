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

// Typed command — core thread never sees JSON
enum class McpCommandType {
	LoadRom,
	StepFrame,
	ReadMemory,
	WriteMemory,
	SetInput,
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

// Core state contract — every MCP tool validates against this
struct McpCoreState {
	int consoleType = -1;
	bool romLoaded = false;
	bool externalControl = false;
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

	bool IsExternalControlled() const { return _coreState.externalControl; }
	McpCoreState& GetCoreState() { return _coreState; }
};
