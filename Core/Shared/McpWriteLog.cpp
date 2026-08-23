#include "pch.h"
#include "Shared/McpWriteLog.h"

McpWriteLog& McpWriteLog::Instance()
{
	static McpWriteLog instance;
	return instance;
}

void McpWriteLog::Record(uint64_t cycle, uint32_t frame, uint16_t pc, uint16_t addr, uint8_t value, int32_t prgPc, int32_t prgAddr)
{
	// try_lock so the emu thread never blocks on a TCP-thread drain.
	// Skipping a few writes is far better than stalling the CPU clock.
	std::unique_lock<std::mutex> lock(_mtx, std::try_to_lock);
	if(!lock.owns_lock()) {
		_dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if(_buffer.size() >= _cap) {
		_overflow = true;
		// Auto-disarm so subsequent ProcessWrite calls short-circuit on the
		// atomic IsEnabled() check and avoid the lock entirely. The user
		// must drain + re-arm to resume logging.
		_enabled.store(false, std::memory_order_release);
		return;
	}
	McpWriteLogEntry e;
	e.Cycle = cycle;
	e.Frame = frame;
	e.Pc = pc;
	e.Addr = addr;
	e.Value = value;
	e._Pad = 0;
	e.PrgPc = prgPc;
	e.PrgAddr = prgAddr;
	_buffer.push_back(e);
}

void McpWriteLog::Configure(uint16_t startAddr, uint16_t endAddr, bool enabled, size_t cap)
{
	if(cap < 1024) cap = 1024;
	if(cap > 1024 * 1024) cap = 1024 * 1024;

	{
		std::lock_guard<std::mutex> lock(_mtx);
		_cap = cap;
		_buffer.clear();
		_buffer.reserve(cap < 8192 ? cap : 8192);
		_overflow = false;
		_dropped.store(0, std::memory_order_relaxed);
	}

	_start.store(startAddr, std::memory_order_relaxed);
	_end.store(endAddr, std::memory_order_relaxed);
	_enabled.store(enabled, std::memory_order_release);
}

void McpWriteLog::Drain(std::vector<McpWriteLogEntry>& out, size_t maxEntries, bool clear, bool& overflowFlag, uint64_t& dropped)
{
	std::lock_guard<std::mutex> lock(_mtx);
	size_t n = _buffer.size();
	if(n > maxEntries) n = maxEntries;
	out.assign(_buffer.begin(), _buffer.begin() + n);
	overflowFlag = _overflow;
	dropped = _dropped.load(std::memory_order_relaxed);
	if(clear) {
		_buffer.clear();
		_overflow = false;
		_dropped.store(0, std::memory_order_relaxed);
	}
}

void McpWriteLog::Clear()
{
	std::lock_guard<std::mutex> lock(_mtx);
	_buffer.clear();
	_overflow = false;
	_dropped.store(0, std::memory_order_relaxed);
}
