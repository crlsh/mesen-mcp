#pragma once
#include "pch.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>

enum class McpExecutionState : uint8_t
{
	Detached,
	Idle,
	Loading,
	ClockPaused,
	DebuggerPaused,
	Running,
	PauseRequested,
	Stepping,
	BreakpointStopped,
	Error
};

enum class McpExecutionPoint : uint8_t
{
	MainLoop,
	DebuggerStop
};

struct McpExecutionSnapshot
{
	McpExecutionState State = McpExecutionState::Detached;
	uint64_t Generation = 0;
};

class McpExecutionController
{
private:
	std::atomic<McpExecutionState> _state { McpExecutionState::Detached };
	std::atomic<uint64_t> _generation { 0 };
	std::mutex _queueMutex;
	std::queue<std::function<void(McpExecutionPoint)>> _queue;

	void SetState(McpExecutionState state);

public:
	void Attach(bool consoleLoaded);
	void Detach();
	void BeginLoading();
	void FinishLoading(bool success);

	bool RequestRun();
	bool RequestPause();
	bool RequestStep();
	void CloseClock();
	void CompleteClockStep();
	void NotifyDebuggerStopped(bool breakpoint);
	void NotifyDebuggerResumed();

	McpExecutionSnapshot GetSnapshot() const;
	bool IsAttached() const;
	bool IsClockGated() const;

	void Enqueue(std::function<void(McpExecutionPoint)> task);
	uint32_t Pump(McpExecutionPoint point);
};
