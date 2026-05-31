#pragma once
#include "pch.h"

struct PrgLocation
{
	int32_t Bank;
	uint32_t PrgOffset;
};

struct PrgWindow
{
	uint16_t CpuStart;
	uint16_t CpuEnd;
	int32_t Bank;
	int32_t PrgOffsetStart;
	bool InPrgRom;
};

namespace PrgMap
{
	//Pure resolution helpers — used by BaseMapper and exercised directly from unit tests.
	//Pre: prgPages is an array of 256 entries, one pointer per CPU $00xx page.
	inline optional<PrgLocation> ResolveOffset(uint16_t cpuAddr, uint8_t* const* prgPages, const uint8_t* prgRom, uint32_t prgSize, uint16_t pageSize)
	{
		if(cpuAddr < 0x2000) return std::nullopt;
		uint8_t* addr = prgPages[cpuAddr >> 8] + (uint8_t)cpuAddr;
		if(addr >= prgRom && addr < prgRom + prgSize) {
			uint32_t prgOffset = (uint32_t)(addr - prgRom);
			PrgLocation loc;
			loc.PrgOffset = prgOffset;
			loc.Bank = (pageSize > 0 && (prgOffset % pageSize) == 0) ? (int32_t)(prgOffset / pageSize) : -1;
			return loc;
		}
		return std::nullopt;
	}

	inline vector<PrgWindow> BuildMap(uint8_t* const* prgPages, const uint8_t* prgRom, uint32_t prgSize)
	{
		vector<PrgWindow> result;
		int p = 0x80;
		while(p < 0x100) {
			PrgWindow w;
			w.CpuStart = (uint16_t)(p << 8);
			uint8_t* pagePtr = prgPages[p];
			bool inPrg = pagePtr >= prgRom && pagePtr < prgRom + prgSize;
			w.InPrgRom = inPrg;
			w.PrgOffsetStart = inPrg ? (int32_t)(pagePtr - prgRom) : -1;

			int q = p + 1;
			while(q < 0x100) {
				uint8_t* prevPtr = prgPages[q - 1];
				uint8_t* curPtr = prgPages[q];
				bool curInPrg = curPtr >= prgRom && curPtr < prgRom + prgSize;
				if(curInPrg != inPrg) break;
				if(inPrg && (curPtr - prevPtr) != 0x100) break;
				if(!inPrg && curPtr != prevPtr + 0x100) break;
				q++;
			}
			w.CpuEnd = (uint16_t)((q << 8) - 1);

			if(inPrg) {
				uint32_t windowSize = (uint32_t)((q - p) * 0x100);
				if(windowSize > 0 && ((uint32_t)w.PrgOffsetStart % windowSize) == 0) {
					w.Bank = (int32_t)((uint32_t)w.PrgOffsetStart / windowSize);
				} else {
					w.Bank = -1;
				}
			} else {
				w.Bank = -1;
			}
			result.push_back(w);
			p = q;
		}
		return result;
	}
}
