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

	void AppendUInt64(string& out, uint64_t value)
	{
		char buf[32];
		int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
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

void NesControlFlowTracer::Start(const string& filename, bool deduplicate, const string& summaryPath,
	uint32_t eventMask, bool graphDeduplicate)
{
	if(_enabled) {
		return;
	}
	_outputBuffer.clear();
	_outputBuffer.reserve(64 * 1024);
	_outputFile.open(filename, ios::out | ios::binary);
	_enabled = _outputFile.is_open();
	_mapperStateId = 0;
	_deduplicate = deduplicate;
	_graphDeduplicate = graphDeduplicate;
	_eventMask = eventMask;
	_summaryPath = summaryPath;
	_seen.clear();
	_eventsSeen = 0;
	_uniqueEvents = 0;
	_newSinceLastStats = 0;
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
		if(_deduplicate) {
			WriteSummary();
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

uint32_t NesControlFlowTracer::PrgOffsetForCpu(int32_t cpuAddr)
{
	if(cpuAddr < 0) return 0xFFFFFFFFu;
	BaseMapper* mapper = _console->GetMapper();
	if(!mapper) return 0xFFFFFFFFu;
	auto loc = mapper->ResolveCpuAddressToPrgOffset((uint16_t)cpuAddr);
	if(!loc.has_value()) return 0xFFFFFFFFu;
	return (uint32_t)loc->PrgOffset;
}

uint32_t NesControlFlowTracer::HashPrgMap(const vector<PrgWindow>& windows)
{
	//Cheap stable hash of the resulting bank layout. Two identical writes (addr+value)
	//under the same mapperStateId produce the same hash; that's enough for dedup.
	uint32_t h = 2166136261u;
	for(const PrgWindow& w : windows) {
		uint32_t v = (uint32_t)(w.Bank & 0xFFFF) | ((uint32_t)(w.PrgOffsetStart & 0xFFFFFF) << 16);
		h = (h ^ v) * 16777619u;
	}
	return h;
}

bool NesControlFlowTracer::MaybeRecord(uint8_t type, uint32_t a, uint32_t b)
{
	BaseNesPpu* ppu = _console->GetPpu();
	uint32_t frame = ppu ? ppu->GetFrameCount() : 0;
	DedupKey k{ type, a, b, (_graphDeduplicate && type != 9) ? 0 : _mapperStateId };
	auto it = _seen.find(k);
	if(it == _seen.end()) {
		DedupEntry e;
		e.count = 1;
		e.firstFrame = frame;
		e.lastFrame = frame;
		_seen.emplace(k, std::move(e));
		_uniqueEvents++;
		_newSinceLastStats++;
		return true;
	}
	it->second.count++;
	it->second.lastFrame = frame;
	return false;
}

void NesControlFlowTracer::EmitOrDedupLine(string& line, uint8_t type, uint32_t a, uint32_t b)
{
	if(type >= 32 || (_eventMask & (1u << type)) == 0) return;
	_eventsSeen++;
	if(_deduplicate) {
		bool isNew = MaybeRecord(type, a, b);
		if(isNew) {
			//Stash a copy of the JSON line in the dedup entry for the summary.
			DedupKey k{ type, a, b, (_graphDeduplicate && type != 9) ? 0 : _mapperStateId };
			_seen[k].firstLineJson = line;
			EmitLine(line);
		}
		//Duplicates: nothing written to JSONL, count was already bumped above.
	} else {
		EmitLine(line);
	}
}

void NesControlFlowTracer::WriteSummary()
{
	if(_summaryPath.empty()) return;
	ofstream sf(_summaryPath, ios::out | ios::binary);
	if(!sf.is_open()) return;

	sf << "{\"eventsSeen\":";
	{ char b[32]; int n = snprintf(b, sizeof(b), "%llu", (unsigned long long)_eventsSeen); sf.write(b, n); }
	sf << ",\"uniqueEvents\":";
	{ char b[32]; int n = snprintf(b, sizeof(b), "%llu", (unsigned long long)_uniqueEvents); sf.write(b, n); }
	sf << ",\"events\":[";

	bool first = true;
	for(const auto& kv : _seen) {
		const DedupKey& k = kv.first;
		const DedupEntry& e = kv.second;
		if(!first) sf << ',';
		first = false;
		sf << "{\"type\":";
		switch(k.type) {
			case 0: sf << "\"RESET\""; break;
			case 1: sf << "\"NMI_ENTRY\""; break;
			case 2: sf << "\"IRQ_ENTRY\""; break;
			case 3: sf << "\"JSR\""; break;
			case 4: sf << "\"JMP_ABS\""; break;
			case 5: sf << "\"JMP_INDIRECT\""; break;
			case 6: sf << "\"RTS\""; break;
			case 7: sf << "\"RTI\""; break;
			case 8: sf << "\"BRK\""; break;
			case 9: sf << "\"MAPPER_WRITE\""; break;
			default: sf << "\"UNKNOWN\""; break;
		}
		if(k.type == 9) {
			//MAPPER_WRITE: a == addr<<8|value, b == prgMapAfter hash
			sf << ",\"addr\":\"0x";
			char hbuf[8]; snprintf(hbuf, sizeof(hbuf), "%04X", (uint16_t)((k.a >> 8) & 0xFFFF)); sf << hbuf;
			sf << "\",\"value\":\"0x";
			snprintf(hbuf, sizeof(hbuf), "%02X", (uint8_t)(k.a & 0xFF)); sf << hbuf;
			sf << "\",\"prgMapAfterHash\":\"0x";
			char hb[16]; snprintf(hb, sizeof(hb), "%08X", k.b); sf << hb;
			sf << "\"";
		} else {
			sf << ",\"srcPrgOffset\":";
			if(k.a == 0xFFFFFFFFu) sf << "null";
			else { char hb[16]; snprintf(hb, sizeof(hb), "\"0x%06X\"", k.a & 0xFFFFFF); sf << hb; }
			sf << ",\"dstPrgOffset\":";
			if(k.b == 0xFFFFFFFFu) sf << "null";
			else { char hb[16]; snprintf(hb, sizeof(hb), "\"0x%06X\"", k.b & 0xFFFFFF); sf << hb; }
		}
		sf << ",\"mapperStateId\":" << k.mapperStateId;
		sf << ",\"count\":";
		{ char b[32]; int n = snprintf(b, sizeof(b), "%llu", (unsigned long long)e.count); sf.write(b, n); }
		sf << ",\"firstFrame\":" << e.firstFrame;
		sf << ",\"lastFrame\":" << e.lastFrame;
		sf << '}';
	}

	sf << "]}";
	sf.close();
}

NesControlFlowTracer::Stats NesControlFlowTracer::ConsumeStats()
{
	Stats s{ _eventsSeen, _uniqueEvents, _newSinceLastStats };
	_newSinceLastStats = 0;
	return s;
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
	EmitOrDedupLine(line, 0, 0xFFFFFFFFu, PrgOffsetForCpu(dstPc));
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
	EmitOrDedupLine(line, 1, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(dstPc));
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
	EmitOrDedupLine(line, 2, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(dstPc));
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
	EmitOrDedupLine(line, 3, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(dstPc));
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
	EmitOrDedupLine(line, 4, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(dstPc));
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
	EmitOrDedupLine(line, 5, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(ptrValue));
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
	EmitOrDedupLine(line, 6, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(dstPc));
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
	EmitOrDedupLine(line, 7, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(pulledAddress));
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
	EmitOrDedupLine(line, 8, PrgOffsetForCpu(srcPc), PrgOffsetForCpu(dstPc));
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
	uint32_t a = ((uint32_t)addr << 8) | value;
	uint32_t b = HashPrgMap(after);
	EmitOrDedupLine(line, 9, a, b);
}
