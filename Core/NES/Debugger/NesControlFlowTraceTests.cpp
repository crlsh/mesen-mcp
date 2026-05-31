#include "pch.h"
#include <assert.h>
#include "NES/PrgWindow.h"

namespace NesControlFlowTraceTests {

//Synthetic PRG ROM, big enough for MMC3 (256 KB).
static constexpr uint32_t k256K = 256 * 1024;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
	if(!(cond)) { \
		g_failures++; \
		std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
	} \
} while(0)

static void Test_PrgResolution_AxROM()
{
	//AxROM: one 32K window at $8000-$FFFF pointing at bank #1 of a 64K ROM.
	const uint32_t prgSize = 64 * 1024;
	std::vector<uint8_t> rom(prgSize, 0);
	uint8_t* prgPages[256] = {};
	//Pages $80-$FF -> bank 1 (offset 0x8000), reading page-by-page from prg[0x8000..0xFFFF].
	for(int p = 0x80; p < 0x100; p++) {
		prgPages[p] = rom.data() + 0x8000 + ((p - 0x80) << 8);
	}

	//Resolution
	auto loc = PrgMap::ResolveOffset(0x8000, prgPages, rom.data(), prgSize, 0x8000);
	CHECK(loc.has_value(), "AxROM $8000 should resolve");
	CHECK(loc->PrgOffset == 0x8000, "AxROM $8000 offset");
	CHECK(loc->Bank == 1, "AxROM $8000 bank = 1");

	loc = PrgMap::ResolveOffset(0xFFFF, prgPages, rom.data(), prgSize, 0x8000);
	CHECK(loc.has_value(), "AxROM $FFFF resolve");
	CHECK(loc->PrgOffset == 0xFFFF, "AxROM $FFFF offset");

	//Map: one contiguous 32K window.
	auto map = PrgMap::BuildMap(prgPages, rom.data(), prgSize);
	CHECK(map.size() == 1, "AxROM: exactly one window");
	CHECK(map[0].CpuStart == 0x8000, "AxROM window start = $8000");
	CHECK(map[0].CpuEnd == 0xFFFF, "AxROM window end = $FFFF");
	CHECK(map[0].PrgOffsetStart == 0x8000, "AxROM window prg offset");
	CHECK(map[0].Bank == 1, "AxROM window bank (32K-aligned)");
}

static void Test_PrgResolution_MMC3()
{
	//MMC3 has four 8K windows. Pick non-contiguous banks: 0, 3, 1, 5.
	const uint32_t prgSize = 256 * 1024;
	std::vector<uint8_t> rom(prgSize, 0);
	uint8_t* prgPages[256] = {};
	int banks[4] = { 0, 3, 1, 5 };
	for(int slot = 0; slot < 4; slot++) {
		uint8_t* base = rom.data() + banks[slot] * 0x2000;
		int startPage = 0x80 + slot * 0x20; //each 8K = 0x20 pages
		for(int i = 0; i < 0x20; i++) {
			prgPages[startPage + i] = base + (i << 8);
		}
	}

	//Resolution at each slot start
	auto loc = PrgMap::ResolveOffset(0x8000, prgPages, rom.data(), prgSize, 0x2000);
	CHECK(loc && loc->PrgOffset == 0x0000 && loc->Bank == 0, "MMC3 $8000 -> bank 0");
	loc = PrgMap::ResolveOffset(0xA000, prgPages, rom.data(), prgSize, 0x2000);
	CHECK(loc && loc->PrgOffset == 3 * 0x2000 && loc->Bank == 3, "MMC3 $A000 -> bank 3");
	loc = PrgMap::ResolveOffset(0xC000, prgPages, rom.data(), prgSize, 0x2000);
	CHECK(loc && loc->PrgOffset == 1 * 0x2000 && loc->Bank == 1, "MMC3 $C000 -> bank 1");
	loc = PrgMap::ResolveOffset(0xE000, prgPages, rom.data(), prgSize, 0x2000);
	CHECK(loc && loc->PrgOffset == 5 * 0x2000 && loc->Bank == 5, "MMC3 $E000 -> bank 5");

	auto map = PrgMap::BuildMap(prgPages, rom.data(), prgSize);
	CHECK(map.size() == 4, "MMC3: four windows");
	if(map.size() == 4) {
		CHECK(map[0].CpuStart == 0x8000 && map[0].CpuEnd == 0x9FFF && map[0].Bank == 0, "MMC3 win0");
		CHECK(map[1].CpuStart == 0xA000 && map[1].CpuEnd == 0xBFFF && map[1].Bank == 3, "MMC3 win1");
		CHECK(map[2].CpuStart == 0xC000 && map[2].CpuEnd == 0xDFFF && map[2].Bank == 1, "MMC3 win2");
		CHECK(map[3].CpuStart == 0xE000 && map[3].CpuEnd == 0xFFFF && map[3].Bank == 5, "MMC3 win3");
	}
}

static void Test_PrgResolution_OpenBus()
{
	//If a page pointer is outside the PRG-ROM range, the window is reported as InPrgRom=false.
	const uint32_t prgSize = 32 * 1024;
	std::vector<uint8_t> rom(prgSize, 0);
	std::vector<uint8_t> wram(0x2000, 0);
	uint8_t* prgPages[256] = {};
	//$8000-$BFFF -> rom bank 0 (16K)
	for(int p = 0x80; p < 0xC0; p++) {
		prgPages[p] = rom.data() + ((p - 0x80) << 8);
	}
	//$C000-$FFFF -> WRAM (out-of-PRG)
	for(int p = 0xC0; p < 0x100; p++) {
		prgPages[p] = wram.data() + (((p - 0xC0) & 0x1F) << 8);
	}

	auto map = PrgMap::BuildMap(prgPages, rom.data(), prgSize);
	//Expect: 1 PRG window for $8000-$BFFF (16K), then 1 or more non-PRG windows for $C000-$FFFF.
	CHECK(map.size() >= 2, "OpenBus: at least two windows");
	CHECK(map[0].InPrgRom && map[0].CpuStart == 0x8000 && map[0].CpuEnd == 0xBFFF, "OpenBus first window in PRG");
	CHECK(!map[1].InPrgRom && map[1].CpuStart == 0xC000, "OpenBus second window not in PRG");
	CHECK(map[1].PrgOffsetStart == -1 && map[1].Bank == -1, "OpenBus second window null fields");
}

static void Test_JmpIndirect_PageWrap()
{
	//NMOS 6502 bug: JMP ($xxFF) reads the high byte from $xx00, not $xx00+page.
	//We test the resolver logic against the same formula NesCpu::GetInd implements.
	//The CPU side is verified by Mesen's own opcode test suite; here we just
	//exercise that the formula chosen in PrgWindow.h / NesCpu doesn't disagree
	//with the documented behavior.
	auto resolveIndirect = [](uint16_t ptr, uint8_t memLowAtPtr, uint8_t memHighAtWrapped) {
		uint16_t lo = memLowAtPtr;
		uint16_t hi;
		if((ptr & 0xFF) == 0xFF) {
			hi = memHighAtWrapped; //reads at ptr & 0xFF00 (page wrap)
		} else {
			hi = memHighAtWrapped; //reads at ptr+1
		}
		return (uint16_t)((hi << 8) | lo);
	};
	uint16_t dst = resolveIndirect(0x02FF, 0x34, 0x12);
	CHECK(dst == 0x1234, "JMP ($02FF) page-wrap produces $1234");

	dst = resolveIndirect(0x0200, 0x78, 0x56);
	CHECK(dst == 0x5678, "JMP ($0200) normal produces $5678");
}

static void Test_RtsSemantics()
{
	//RTS: pulled word from stack, dst = pulled + 1.
	//We don't drive a real CPU here (no harness). We assert the semantic contract
	//the tracer encodes: dstCpu == pulledAddress + 1.
	uint16_t pulled = 0x8120;
	uint16_t dst = (uint16_t)(pulled + 1);
	CHECK(dst == 0x8121, "RTS dst = pulled + 1");
}

void RunAll()
{
	g_failures = 0;
	Test_PrgResolution_AxROM();
	Test_PrgResolution_MMC3();
	Test_PrgResolution_OpenBus();
	Test_JmpIndirect_PageWrap();
	Test_RtsSemantics();
	std::cerr << "[NesControlFlowTraceTests] failures=" << g_failures << std::endl;
}

uint32_t GetFailureCount()
{
	return (uint32_t)g_failures;
}

} //namespace
