// Independent consistency checks on kernel data structures (read from physical memory).
#include "../kernel_test.h"

#include <cstdio>
#include <vector>

std::string CheckPfnDatabase() {
    char msg[200];
    uint32_t total = 0;
    std::vector<uint8_t> seen(MM_PAGES);
    for (uint32_t color = 0; color < 32; color++) {
        USHORT packed = MmRetailPfnRegion.FreePagesByColor[color].PackedPfnFlink, prev = 0xFFFE;
        uint32_t n = 0;
        while (packed != 0xFFFE) {
            uint32_t pfn = ((uint32_t)packed << 4) | color;
            if (pfn >= MM_PAGES || seen[pfn]) {
                snprintf(msg, sizeof(msg), "colour %u: pfn %#x out of range or listed twice", color, pfn);
                return msg;
            }
            seen[pfn] = 1;
            uint32_t e = PfnEntry(pfn);
            if (e & (PTE_PRESENT | 0x10000)) {
                snprintf(msg, sizeof(msg), "colour %u: free pfn %#x has entry %08x (present or busy)", color, pfn, e);
                return msg;
            }
            if ((USHORT)(e >> 16) != prev) {
                snprintf(msg, sizeof(msg), "colour %u: pfn %#x back link %04x, expected %04x", color, pfn, e >> 16, prev);
                return msg;
            }
            prev = packed;
            packed = (USHORT)e;
            if (++n > MM_PAGES)
                return "a colour list loops";
        }
        if (MmRetailPfnRegion.FreePagesByColor[color].PackedPfnBlink != prev) {
            snprintf(msg, sizeof(msg), "colour %u: tail %04x, last entry %04x", color,
                     MmRetailPfnRegion.FreePagesByColor[color].PackedPfnBlink, prev);
            return msg;
        }
        total += n;
    }
    if (total != MmAvailablePages || total != MmRetailPfnRegion.AvailablePages) {
        snprintf(msg, sizeof(msg), "%u pages on the lists, MmAvailablePages %lu, region %lu", total, MmAvailablePages,
                 MmRetailPfnRegion.AvailablePages);
        return msg;
    }
    return "";
}

namespace {
struct SpawnArgs {
    std::function<void()> fn;
};
void NTAPI SpawnStart(PVOID p) {
    auto a = (SpawnArgs *)p;
    a->fn();
    delete a;
}
} // namespace

PKTHREAD Spawn(std::function<void()> fn, LONG priority) {
    return KiCreateSystemThread(SpawnStart, new SpawnArgs{std::move(fn)}, 0x10000, priority);
}

void Join(PKTHREAD thread) {
    KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, nullptr);
    ObfDereferenceObject(thread);
}
