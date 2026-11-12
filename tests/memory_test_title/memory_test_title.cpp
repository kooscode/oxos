// OXOS memory test title (nxdk + SDL3): measures how much memory a title is given, for the 64/128 MB comparison between
// kernels (tests/memory_test.py runs it and reads the results). The same XBE is run twice: as built (nxdk sets the
// XBE init flag 0x4, "limit to 64 MB", like retail games) and with that flag cleared.
//
// Measurements, in order (results in XkMemoryTest, also shown on screen):
//   1. the XBE's init flags, as loaded at 0x10000 (header offset 0x124)
//   2. MmQueryStatistics at the start: total and available physical pages
//   3. a contiguous page requested from the upper 64 MB only (MmAllocateContiguousMemoryEx, 0x4000000-0x7FFFFFF):
//      its physical address, or 0 when the kernel has no memory there or refuses it
//   4. ordinary title memory: 1 MB blocks (NtAllocateVirtualMemory, committed) until the kernel says no; every page
//      is touched and looked up (MmGetPhysicalAddress): total bytes, pages above 64 MB, highest physical address
//   5. MmQueryStatistics at the end
// SDL (tools/sdl_screen) starts only after the measurements, once the title memory of step 4 is freed again, so it
// takes nothing from what is measured; the XBE itself is larger with SDL in it (about 1 MB).
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "sdl_screen.h"

namespace {
constexpr unsigned int Block = 1024 * 1024;
constexpr unsigned int MaxBlocks = 256;
constexpr unsigned int Above64MB = 0x4000000;
} // namespace

// The results, for tests/memory_test.py. The layout is fixed (the test reads it).
volatile struct {
    char Magic[8];                   // "XKMEMTST"
    unsigned int Version, Self;      // 1; the block's own address (0 in copies of the image)
    unsigned int Done;               // 1 when every measurement is in
    unsigned int InitFlags;          // 1. the XBE's init flags
    unsigned int TotalPages, AvailablePages; // 2. MmQueryStatistics at the start
    unsigned int HighContiguous;     // 3. physical address of the contiguous page above 64 MB, or 0
    unsigned int Blocks, Bytes;      // 4. 1 MB blocks committed, bytes in total
    unsigned int PagesAbove64MB, HighestPhysical;
    unsigned int FailStatus;         //    the status that ended it (0 = MaxBlocks reached)
    unsigned int TotalPagesEnd, AvailablePagesEnd; // 5. MmQueryStatistics at the end
} XkMemoryTest = {{'X', 'K', 'M', 'E', 'M', 'T', 'S', 'T'}, 1};

namespace {

void Stats(volatile unsigned int &total, volatile unsigned int &available) {
    MM_STATISTICS s{};
    s.Length = sizeof(s);
    if (NT_SUCCESS(MmQueryStatistics(&s))) {
        total = s.TotalPhysicalPages;
        available = s.AvailablePages;
    }
}

} // namespace

int main() {
    XkMemoryTest.Self = reinterpret_cast<unsigned int>(&XkMemoryTest);

    // 1. Init flags of the running XBE (the header is loaded at 0x10000).
    XkMemoryTest.InitFlags = *reinterpret_cast<volatile unsigned int *>(0x10124);

    // 2. What the kernel reports.
    Stats(XkMemoryTest.TotalPages, XkMemoryTest.AvailablePages);

    // 3. A contiguous page from the upper 64 MB only.
    if (PVOID high = MmAllocateContiguousMemoryEx(4096, Above64MB, 0x7FFFFFF, 0, PAGE_READWRITE)) {
        XkMemoryTest.HighContiguous = static_cast<unsigned int>(MmGetPhysicalAddress(high));
        MmFreeContiguousMemory(high);
    }

    // 4. Ordinary memory until the kernel refuses; kept until step 5 has run, so the total is what the title can have.
    static PVOID blocks[MaxBlocks];
    for (unsigned int b = 0; b < MaxBlocks; b++) {
        PVOID &base = blocks[b];
        SIZE_T size = Block;
        NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!NT_SUCCESS(status)) {
            base = nullptr;
            XkMemoryTest.FailStatus = static_cast<unsigned int>(status);
            break;
        }
        for (unsigned int o = 0; o < size; o += 4096) {
            auto page = static_cast<volatile unsigned char *>(base) + o;
            *page = 1; // committed pages are present; touch anyway
            auto pa = static_cast<unsigned int>(MmGetPhysicalAddress(const_cast<unsigned char *>(page)));
            if (pa >= Above64MB)
                XkMemoryTest.PagesAbove64MB = XkMemoryTest.PagesAbove64MB + 1;
            if (pa > XkMemoryTest.HighestPhysical)
                XkMemoryTest.HighestPhysical = pa;
        }
        XkMemoryTest.Blocks = XkMemoryTest.Blocks + 1;
        XkMemoryTest.Bytes = XkMemoryTest.Bytes + size;
    }

    // 5. What the kernel reports now.
    Stats(XkMemoryTest.TotalPagesEnd, XkMemoryTest.AvailablePagesEnd);
    XkMemoryTest.Done = 1;

    // The memory goes back for SDL, then the results go on the screen.
    for (PVOID &base : blocks) {
        SIZE_T size = 0;
        if (base)
            NtFreeVirtualMemory(&base, &size, MEM_RELEASE);
    }
    if (!sdl_screen::Open(false))
        DbgPrint("memory test: SDL: %s\n", SDL_GetError());
    using sdl_screen::Print;
    Print("OXOS memory test title\n\n");
    Print("init flags %08x (limit to 64 MB: %s)\n", XkMemoryTest.InitFlags,
          XkMemoryTest.InitFlags & 4 ? "yes" : "no");
    Print("kernel reports %u pages (%u MB), %u available\n", XkMemoryTest.TotalPages,
          XkMemoryTest.TotalPages / 256, XkMemoryTest.AvailablePages);
    Print("contiguous page above 64 MB: %08x\n", XkMemoryTest.HighContiguous);
    Print("title memory: %u MB in %u blocks, %u pages above 64 MB, highest %08x\n",
          XkMemoryTest.Bytes / Block, XkMemoryTest.Blocks, XkMemoryTest.PagesAbove64MB,
          XkMemoryTest.HighestPhysical);
    Print("ended by status %08x; at the end %u available\n", XkMemoryTest.FailStatus,
          XkMemoryTest.AvailablePagesEnd);
    sdl_screen::ShowForever();
}
