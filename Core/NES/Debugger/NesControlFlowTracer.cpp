#include "pch.h"
#include "NES/Debugger/NesControlFlowTracer.h"
#include "NES/NesConsole.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"

namespace {
	void AppendHex16(string& out, uint16_t value)
	{
		static const char* digits = "0123456789ABCDEF";
		out += "\"0x";
		out += digits[(value >> 12) & 0xF];
		out += digits[(value >> 8) & 0xF];
		out += digits[(value >> 4) & 0xF];
		out += digits[value & 0xF];
		out += '"';
	}

	void AppendHex8(string& out, uint8_t value)
	{
		static const char* digits = "0123456789ABCDEF";
		out += "\"0x";
		out += digits[(value >> 4) & 0xF];
		out += digits[value & 0xF];
		out += '"';
	}

	void AppendHex24(string& out, uint32_t value)
	{
		static const char* digits = "0123456789ABCDEF";
		out += "\"0x";
		out += digits[(value >> 20) & 0xF];
		out += digits[(value >> 16) & 0xF];
		out += digits[(value >> 12) & 0xF];
		out += digits[(value >> 8) & 0xF];
		out += digits[(value >> 4) & 0xF];
		out += digits[value & 0xF];
		out += '"';
	}

	void AppendUInt(string& out, uint32_t value)
	{
		char buf[16];
		int n = snprintf(buf, sizeof(buf), "%u", value);
		out.append(buf, n);
	}

	void AppendInt(string& out, int32_t value)
	{
		char buf[16];
		int n = snprintf(buf, sizeof(buf), "%d", value);
		out.append(buf, n);
	}
}

NesControlFlowTracer::NesControlFlowTracer(NesConsole* console)
	: _console(console)
{
}

NesControlFlowTracer::~NesControlFlowTracer()
{
	Stop();
}

void NesControlFlowTracer::Start(const string& filename)
{
	if(_enabled) {
		return;
	}
	_outputBuffer.clear();
	_outputBuffer.reserve(64 * 1024);
	_outputFile.open(filename, ios::out | ios::binary);
	_enabled = _outputFile.is_open();
	_mapperStateId = 0;
}

void NesControlFlowTracer::Stop()
{
	if(_enabled) {
		_enabled = false;
		if(_outputFile) {
			if(!_outputBuffer.empty()) {
				_outputFile << _outputBuffer;
				_outputBuffer.clear();
			}
			_outputFile.close();
		}
	}
}

void NesControlFlowTracer::FlushIfNeeded()
{
	if(_outputBuffer.size() > 32 * 1024) {
		_outputFile << _outputBuffer;
		_outputBuffer.clear();
	}
}

void NesControlFlowTracer::EmitLine(string& line)
{
	line += '\n';
	_outputBuffer += line;
	FlushIfNeeded();
}

void NesControlFlowTracer::AppendFrameLocation(string& out)
{
	BaseNesPpu* ppu = _console->GetPpu();
	out += "\"frame\":";
	AppendUInt(out, ppu ? ppu->GetFrameCount() : 0);
	out += ",\"scanline\":";
	AppendInt(out, ppu ? ppu->GetCurrentScanline() : 0);
	out += ",\"cycle\":";
	AppendUInt(out, ppu ? ppu->GetCurrentCycle() : 0);
}

void NesControlFlowTracer::AppendPrgFields(string& out, const char* cpuKey, const char* bankKey, const char* offsetKey, int32_t cpuAddr)
{
	int32_t bank = -1;
	int32_t prgOffset = -1;
	if(cpuAddr >= 0) {
		BaseMapper* mapper = _console->GetMapper();
		if(mapper) {
			auto loc = mapper->ResolveCpuAddressToPrgOffset((uint16_t)cpuAddr);
			if(loc.has_value()) {
				bank = loc->Bank;
				prgOffset = (int32_t)loc->PrgOffset;
			}
		}
	}

	out += '"';
	out += cpuKey;
	out += "\":";
	if(cpuAddr < 0) {
		out += "null";
	} else {
		AppendHex16(out, (uint16_t)cpuAddr);
	}

	out += ",\"";
	out += bankKey;
	out += "\":";
	if(bank < 0) {
		out += "null";
	} else {
		AppendInt(out, bank);
	}

	out += ",\"";
	out += offsetKey;
	out += "\":";
	if(prgOffset < 0) {
		out += "null";
	} else {
		AppendHex24(out, (uint32_t)prgOffset);
	}
}

void NesControlFlowTracer::AppendPrgMap(string& out, const char* key, const vector<PrgWindow>& windows)
{
	out += '"';
	out += key;
	out += "\":[";
	for(size_t i = 0; i < windows.size(); i++) {
		if(i > 0) out += ',';
		const PrgWindow& w = windows[i];
		out += "{\"cpuStart\":";
		AppendHex16(out, w.CpuStart);
		out += ",\"cpuEnd\":";
		AppendHex16(out, w.CpuEnd);
		out += ",\"bank\":";
		if(w.InPrgRom && w.Bank >= 0) {
			AppendInt(out, w.Bank);
		} else {
			out += "null";
		}
		out += ",\"prgOffsetStart\":";
		if(w.InPrgRom && w.PrgOffsetStart >= 0) {
			AppendHex24(out, (uint32_t)w.PrgOffsetStart);
		} else {
			out += "null";
		}
		out += '}';
	}
	out += ']';
}

void NesControlFlowTracer::LogReset(uint16_t dstPc)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"RESET\",\"opcode\":null,";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", -1);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", dstPc);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogNmiEntry(uint16_t srcPc, uint16_t dstPc)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"NMI_ENTRY\",\"opcode\":null,";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", dstPc);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogIrqEntry(uint16_t srcPc, uint16_t dstPc)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"IRQ_ENTRY\",\"opcode\":null,";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", dstPc);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogJsr(uint16_t srcPc, uint16_t dstPc)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"JSR\",\"opcode\":\"0x20\",";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", dstPc);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogJmpAbs(uint16_t srcPc, uint16_t dstPc)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"JMP_ABS\",\"opcode\":\"0x4C\",";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", dstPc);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogJmpIndirect(uint16_t srcPc, uint16_t ptr, uint16_t ptrValue)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"JMP_INDIRECT\",\"opcode\":\"0x6C\",";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", ptrValue);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += ",\"ptr\":";
	AppendHex16(line, ptr);
	line += ",\"ptrValue\":";
	AppendHex16(line, ptrValue);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogRts(uint16_t srcPc, uint16_t pulledAddress, uint16_t dstPc, uint8_t stackBefore, uint8_t stackAfter)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"RTS\",\"opcode\":\"0x60\",";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", dstPc);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += ",\"stackBefore\":";
	AppendHex8(line, stackBefore);
	line += ",\"stackAfter\":";
	AppendHex8(line, stackAfter);
	line += ",\"pulledAddress\":";
	AppendHex16(line, pulledAddress);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogRti(uint16_t srcPc, uint16_t pulledAddress, uint8_t stackBefore, uint8_t stackAfter)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"RTI\",\"opcode\":\"0x40\",";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", pulledAddress);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += ",\"stackBefore\":";
	AppendHex8(line, stackBefore);
	line += ",\"stackAfter\":";
	AppendHex8(line, stackAfter);
	line += ",\"pulledAddress\":";
	AppendHex16(line, pulledAddress);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogBrk(uint16_t srcPc, uint16_t dstPc, bool nmi)
{
	if(!_enabled) return;
	string line = "{";
	AppendFrameLocation(line);
	line += nmi ? ",\"type\":\"BRK\",\"opcode\":\"0x00\",\"divertedToNmi\":true,"
	            : ",\"type\":\"BRK\",\"opcode\":\"0x00\",\"divertedToNmi\":false,";
	AppendPrgFields(line, "srcCpu", "srcBank", "srcPrgOffset", srcPc);
	line += ",";
	AppendPrgFields(line, "dstCpu", "dstBank", "dstPrgOffset", dstPc);
	line += ",\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += '}';
	EmitLine(line);
}

void NesControlFlowTracer::LogMapperWrite(uint16_t addr, uint8_t value, const vector<PrgWindow>& before, const vector<PrgWindow>& after)
{
	if(!_enabled) return;
	_mapperStateId++;
	string line = "{";
	AppendFrameLocation(line);
	line += ",\"type\":\"MAPPER_WRITE\",\"opcode\":null,\"srcCpu\":null,\"srcBank\":null,\"srcPrgOffset\":null,\"dstCpu\":null,\"dstBank\":null,\"dstPrgOffset\":null,\"mapperStateId\":";
	AppendUInt(line, _mapperStateId);
	line += ",\"addr\":";
	AppendHex16(line, addr);
	line += ",\"value\":";
	AppendHex8(line, value);
	line += ',';
	AppendPrgMap(line, "prgMapBefore", before);
	line += ',';
	AppendPrgMap(line, "prgMapAfter", after);
	line += '}';
	EmitLine(line);
}
