#include "pch.h"
#include "Shared/McpExecutionController.h"

void McpExecutionController::SetState(McpExecutionState state)
{
	McpExecutionState previous = _state.exchange(state);
	if(previous != state) {
		_generation++;
	}
}

void McpExecutionController::Attach(bool consoleLoaded)
{
	SetState(consoleLoaded ? McpExecutionState::ClockPaused : McpExecutionState::Idle);
}

void McpExecutionController::Detach()
{
	SetState(McpExecutionState::Detached);
}

void McpExecutionController::BeginLoading()
{
	SetState(McpExecutionState::Loading);
}

void McpExecutionController::FinishLoading(bool success)
{
	SetState(success ? McpExecutionState::ClockPaused : McpExecutionState::Error);
}

bool McpExecutionController::RequestRun()
{
	McpExecutionState state = _state.load();
	if(state != McpExecutionState::ClockPaused && state != McpExecutionState::DebuggerPaused && state != McpExecutionState::BreakpointStopped) {
		return false;
	}
	SetState(McpExecutionState::Running);
	return true;
}

bool McpExecutionController::RequestPause()
{
	if(_state.load() != McpExecutionState::Running) {
		return false;
	}
	SetState(McpExecutionState::PauseRequested);
	return true;
}

bool McpExecutionController::RequestStep()
{
	McpExecutionState state = _state.load();
	if(state != McpExecutionState::ClockPaused && state != McpExecutionState::DebuggerPaused && state != McpExecutionState::BreakpointStopped) {
		return false;
	}
	SetState(McpExecutionState::Stepping);
	return true;
}

void McpExecutionController::CloseClock()
{
	if(IsAttached()) {
		SetState(McpExecutionState::ClockPaused);
	}
}

void McpExecutionController::CompleteClockStep()
{
	if(_state.load() == McpExecutionState::Stepping) {
		SetState(McpExecutionState::ClockPaused);
	}
}

void McpExecutionController::NotifyDebuggerStopped(bool breakpoint)
{
	McpExecutionState state = _state.load();
	if(state == McpExecutionState::Detached || state == McpExecutionState::Idle || state == McpExecutionState::Loading || state == McpExecutionState::Error) {
		return;
	}
	SetState(breakpoint ? McpExecutionState::BreakpointStopped : McpExecutionState::DebuggerPaused);
}

void McpExecutionController::NotifyDebuggerResumed()
{
	McpExecutionState state = _state.load();
	if(state == McpExecutionState::BreakpointStopped || state == McpExecutionState::DebuggerPaused || state == McpExecutionState::PauseRequested) {
		SetState(McpExecutionState::Running);
	}
}

McpExecutionSnapshot McpExecutionController::GetSnapshot() const
{
	return { _state.load(), _generation.load() };
}

bool McpExecutionController::IsAttached() const
{
	return _state.load() != McpExecutionState::Detached;
}

bool McpExecutionController::IsClockGated() const
{
	McpExecutionState state = _state.load();
	return state == McpExecutionState::Idle || state == McpExecutionState::Loading || state == McpExecutionState::ClockPaused || state == McpExecutionState::Error;
}

void McpExecutionController::Enqueue(std::function<void(McpExecutionPoint)> task)
{
	std::lock_guard<std::mutex> lock(_queueMutex);
	_queue.push(std::move(task));
}

uint32_t McpExecutionController::Pump(McpExecutionPoint point)
{
	uint32_t count = 0;
	while(true) {
		std::function<void(McpExecutionPoint)> task;
		{
			std::lock_guard<std::mutex> lock(_queueMutex);
			if(_queue.empty()) {
				break;
			}
			task = std::move(_queue.front());
			_queue.pop();
		}
		task(point);
		count++;
	}
	return count;
}
