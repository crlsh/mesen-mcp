#pragma once
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstddef>

struct McpWriteLogEntry
{
	uint64_t Cycle;
	uint32_t Frame;
	uint16_t Pc;
	uint16_t Addr;
	uint8_t Value;
	uint8_t _Pad;
	int32_t PrgPc;   // PRG-ROM offset of the writing instruction, -1 if not in PRG (RAM/etc.)
	int32_t PrgAddr; // PRG-ROM/WRAM offset of the destination, -1 if not mapped
};

class McpWriteLog
{
public:
	static McpWriteLog& Instance();

	bool IsEnabled() const { return _enabled.load(std::memory_order_relaxed); }

	bool InRange(uint16_t addr) const
	{
		uint16_t s = _start.load(std::memory_order_relaxed);
		uint16_t e = _end.load(std::memory_order_relaxed);
		return addr >= s && addr <= e;
	}

	void Record(uint64_t cycle, uint32_t frame, uint16_t pc, uint16_t addr, uint8_t value, int32_t prgPc, int32_t prgAddr);

	void Configure(uint16_t startAddr, uint16_t endAddr, bool enabled, size_t cap);

	void Drain(std::vector<McpWriteLogEntry>& out, size_t maxEntries, bool clear, bool& overflowFlag, uint64_t& dropped);

	void Clear();

private:
	McpWriteLog() = default;
	McpWriteLog(const McpWriteLog&) = delete;
	McpWriteLog& operator=(const McpWriteLog&) = delete;

	std::atomic<bool> _enabled{false};
	std::atomic<uint16_t> _start{0};
	std::atomic<uint16_t> _end{0xFFFF};

	std::mutex _mtx;
	std::vector<McpWriteLogEntry> _buffer;
	size_t _cap = 65536;
	bool _overflow = false;
	std::atomic<uint64_t> _dropped{0};
};
