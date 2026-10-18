// OXOS memory test title (nxdk): measures how much memory a title is given, for the 64/128 MB comparison between
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
#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define BLOCK (1024 * 1024)
#define MAX_BLOCKS 256

volatile struct {
    char Magic[8];                   // "XKMEMTST"
    unsigned int Version, Self;      // 1; the block's own address (0 in copies of the image)
    unsigned int Done;               // 1 when every measurement is in
    unsigned int InitFlags;          // 1. the XBE's init flags
    unsigned int TotalPages, AvailablePages; // 2. MmQueryStatistics at the start
    unsigned int HighContiguous;     // 3. physical address of the contiguous page above 64 MB, or 0
    unsigned int Blocks, Bytes;      // 4. 1 MB blocks committed, bytes in total
    unsigned int PagesAbove64MB, HighestPhysical;
    unsigned int FailStatus;         //    the status that ended it (0 = MAX_BLOCKS reached)
    unsigned int TotalPagesEnd, AvailablePagesEnd; // 5. MmQueryStatistics at the end
} XkMemoryTest = {{'X', 'K', 'M', 'E', 'M', 'T', 'S', 'T'}, 1};

static void Stats(volatile unsigned int *total, volatile unsigned int *available) {
    MM_STATISTICS s = {0};
    s.Length = sizeof(s);
    if (NT_SUCCESS(MmQueryStatistics(&s))) {
        *total = s.TotalPhysicalPages;
        *available = s.AvailablePages;
    }
}

int main(void) {
    XkMemoryTest.Self = (unsigned int)&XkMemoryTest;
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugClearScreen();
    debugPrint("OXOS memory test title\n\n");

    // 1. Init flags of the running XBE (the header is loaded at 0x10000).
    XkMemoryTest.InitFlags = *(volatile unsigned int *)0x10124;

    // 2. What the kernel reports.
    Stats(&XkMemoryTest.TotalPages, &XkMemoryTest.AvailablePages);

    // 3. A contiguous page from the upper 64 MB only.
    PVOID high = MmAllocateContiguousMemoryEx(4096, 0x4000000, 0x7FFFFFF, 0, PAGE_READWRITE);
    if (high) {
        XkMemoryTest.HighContiguous = (unsigned int)MmGetPhysicalAddress(high);
        MmFreeContiguousMemory(high);
    }

    // 4. Ordinary memory until the kernel refuses; never freed, so the total is what the title can have.
    for (unsigned int b = 0; b < MAX_BLOCKS; b++) {
        PVOID base = NULL;
        SIZE_T size = BLOCK;
        NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!NT_SUCCESS(status)) {
            XkMemoryTest.FailStatus = (unsigned int)status;
            break;
        }
        for (unsigned int o = 0; o < size; o += 4096) {
            volatile unsigned char *page = (volatile unsigned char *)base + o;
            *page = 1; // committed pages are present; touch anyway
            unsigned int pa = (unsigned int)MmGetPhysicalAddress((PVOID)page);
            if (pa >= 0x4000000)
                XkMemoryTest.PagesAbove64MB++;
            if (pa > XkMemoryTest.HighestPhysical)
                XkMemoryTest.HighestPhysical = pa;
        }
        XkMemoryTest.Blocks++;
        XkMemoryTest.Bytes += size;
    }

    // 5. What the kernel reports now.
    Stats(&XkMemoryTest.TotalPagesEnd, &XkMemoryTest.AvailablePagesEnd);
    XkMemoryTest.Done = 1;

    debugPrint("init flags %08x (limit to 64 MB: %s)\n", XkMemoryTest.InitFlags,
               XkMemoryTest.InitFlags & 4 ? "yes" : "no");
    debugPrint("kernel reports %u pages (%u MB), %u available\n", XkMemoryTest.TotalPages,
               XkMemoryTest.TotalPages / 256, XkMemoryTest.AvailablePages);
    debugPrint("contiguous page above 64 MB: %08x\n", XkMemoryTest.HighContiguous);
    debugPrint("title memory: %u MB in %u blocks, %u pages above 64 MB, highest %08x\n",
               XkMemoryTest.Bytes / BLOCK, XkMemoryTest.Blocks, XkMemoryTest.PagesAbove64MB,
               XkMemoryTest.HighestPhysical);
    debugPrint("ended by status %08x; at the end %u available\n", XkMemoryTest.FailStatus,
               XkMemoryTest.AvailablePagesEnd);
    for (;;)
        XVideoWaitForVBlank();
}
