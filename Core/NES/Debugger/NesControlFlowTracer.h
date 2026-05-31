#pragma once
#include "pch.h"
#include "NES/PrgWindow.h"

class NesConsole;
class BaseMapper;
class BaseNesPpu;

class NesControlFlowTracer
{
private:
	NesConsole* _console = nullptr;
	bool _enabled = false;
	string _outputBuffer;
	ofstream _outputFile;

	//Bumped on every MAPPER_WRITE - cheap proxy for "mapper state changed".
	//V2 will replace this with a content-hashed identifier carrying provenance.
	uint32_t _mapperStateId = 0;

	void FlushIfNeeded();
	void AppendFrameLocation(string& out);
	void AppendCpuAddr(string& out, const char* key, int32_t cpuAddr);
	void AppendPrgFields(string& out, const char* cpuKey, const char* bankKey, const char* offsetKey, int32_t cpuAddr);
	void AppendPrgMap(string& out, const char* key, const vector<PrgWindow>& windows);
	void EmitLine(string& line);

public:
	NesControlFlowTracer(NesConsole* console);
	~NesControlFlowTracer();

	void Start(const string& filename);
	void Stop();
	__forceinline bool IsEnabled() const { return _enabled; }

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
