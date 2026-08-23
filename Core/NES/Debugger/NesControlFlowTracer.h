#pragma once
#include "pch.h"
#include <unordered_map>
#include "NES/PrgWindow.h"

class NesConsole;
class BaseMapper;
class BaseNesPpu;

class NesControlFlowTracer
{
private:
	struct DedupKey
	{
		uint8_t type;       //0=RESET 1=NMI 2=IRQ 3=JSR 4=JMP_ABS 5=JMP_IND 6=RTS 7=RTI 8=BRK 9=MAPPER_WRITE
		uint32_t a;         //control flow: srcPrgOffset (or 0xFFFFFFFF for null); MAPPER_WRITE: addr<<8|value
		uint32_t b;         //control flow: dstPrgOffset (or 0xFFFFFFFF for null); MAPPER_WRITE: prgMapAfter hash
		uint32_t mapperStateId;

		bool operator==(const DedupKey& o) const
		{
			return type == o.type && a == o.a && b == o.b && mapperStateId == o.mapperStateId;
		}
	};

	struct DedupKeyHash
	{
		size_t operator()(const DedupKey& k) const noexcept
		{
			//FNV-ish 64-bit mix of the 4 components
			uint64_t h = 1469598103934665603ULL;
			auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
			mix(k.type);
			mix(k.a);
			mix(k.b);
			mix(k.mapperStateId);
			return (size_t)h;
		}
	};

	struct DedupEntry
	{
		uint64_t count = 0;
		uint32_t firstFrame = 0;
		uint32_t lastFrame = 0;
		string firstLineJson;   //the first JSON we emitted to the file for this key, used for the summary
	};

	NesConsole* _console = nullptr;
	bool _enabled = false;
	bool _deduplicate = false;
	bool _graphDeduplicate = false;
	uint32_t _eventMask = 0x3FF;
	string _summaryPath;
	string _outputBuffer;
	ofstream _outputFile;

	//Bumped on every MAPPER_WRITE - cheap proxy for "mapper state changed".
	//V2 will replace this with a content-hashed identifier carrying provenance.
	uint32_t _mapperStateId = 0;

	std::unordered_map<DedupKey, DedupEntry, DedupKeyHash> _seen;
	uint64_t _eventsSeen = 0;
	uint64_t _uniqueEvents = 0;
	uint64_t _newSinceLastStats = 0;

	void FlushIfNeeded();
	void AppendFrameLocation(string& out);
	void AppendCpuAddr(string& out, const char* key, int32_t cpuAddr);
	void AppendPrgFields(string& out, const char* cpuKey, const char* bankKey, const char* offsetKey, int32_t cpuAddr);
	void AppendPrgMap(string& out, const char* key, const vector<PrgWindow>& windows);
	void EmitLine(string& line);

	//Returns true if event is new (first time seen). On dup, increments counts only.
	bool MaybeRecord(uint8_t type, uint32_t a, uint32_t b);
	uint32_t PrgOffsetForCpu(int32_t cpuAddr);
	uint32_t HashPrgMap(const vector<PrgWindow>& windows);
	void EmitOrDedupLine(string& line, uint8_t type, uint32_t a, uint32_t b);
	void WriteSummary();

public:
	NesControlFlowTracer(NesConsole* console);
	~NesControlFlowTracer();

	void Start(const string& filename, bool deduplicate = false, const string& summaryPath = "",
		uint32_t eventMask = 0x3FF, bool graphDeduplicate = false);
	void Stop();
	__forceinline bool IsEnabled() const { return _enabled; }

	struct Stats
	{
		uint64_t eventsSeen;
		uint64_t uniqueEvents;
		uint64_t newEventsSinceLastStats;
	};

	//Reads counters and resets newEventsSinceLastStats.
	Stats ConsumeStats();

	void LogReset(uint16_t dstPc);
	void LogNmiEntry(uint16_t srcPc, uint16_t dstPc);
	void LogIrqEntry(uint16_t srcPc, uint16_t dstPc);
	void LogJsr(uint16_t srcPc, uint16_t dstPc);
	void LogJmpAbs(uint16_t srcPc, uint16_t dstPc);
	void LogJmpIndirect(uint16_t srcPc, uint16_t ptr, uint16_t ptrValue);
	void LogRts(uint16_t srcPc, uint16_t pulledAddress, uint16_t dstPc, uint8_t stackBefore, uint8_t stackAfter);
	void LogRti(uint16_t srcPc, uint16_t pulledAddress, uint8_t stackBefore, uint8_t stackAfter);
	void LogBrk(uint16_t srcPc, uint16_t dstPc, bool nmi);
	void LogMapperWrite(uint16_t addr, uint8_t value, const vector<PrgWindow>& before, const vector<PrgWindow>& after);
};
