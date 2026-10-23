// Out of memory: every allocating entry point fails cleanly (a status or nullptr, nothing leaked, nothing half
// built), and works again once memory is back.
#include "kernel_test.h"

#include <vector>

extern volatile ULONG ExpPoolBlocks;
struct KFLOATING_SAVE_ {
    UCHAR Flags, Irql, Reserved[6];
    PVOID Buffer;
    ULONG Reserved2[2];
    PKTHREAD Thread;
};
XBAPI NTSTATUS NTAPI KeSaveFloatingPointState(KFLOATING_SAVE_ *FloatSave);
XBAPI NTSTATUS NTAPI RtlAnsiStringToUnicodeString(PUNICODE_STRING DestinationString, const ANSI_STRING *SourceString,
                                                  BOOLEAN AllocateDestinationString);
XBAPI NTSTATUS NTAPI NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES ObjectAttributes, TIMER_TYPE TimerType);

namespace {
constexpr NTSTATUS NO_RESOURCES = (NTSTATUS)0xC000009A;

// Takes every free physical page (leaving `leave`), then with `drainPool` every small pool block that could still
// be handed out (the free lists and the current page: no new pool page can be had any more); gives everything back
// when it goes out of scope. Pages first, so that the pool cannot start a new page; pool pages that become entirely
// free go back to the memory manager when the blocks are freed.
struct Exhaust {
    std::vector<ULONG> pages;
    std::vector<void *> blocks;
    explicit Exhaust(ULONG leave = 0, bool drainPool = true) {
        while (MmAvailablePages > leave)
            pages.push_back(MiAllocatePage(MmSystemMemoryUsage, nullptr, false));
        if (drainPool)
            for (ULONG units = 127; units >= 1; units--)
                while (void *p = ExAllocatePool(units * 32 - 8))
                    blocks.push_back(p);
    }
    ~Exhaust() {
        for (ULONG p : pages)
            MiFreePage(p);
        for (void *b : blocks)
            ExFreePool(b);
    }
};
} // namespace

TEST(NoMemory, PagesAndPool) {
    ULONG before = MmAvailablePages;
    {
        Exhaust all;
        EXPECT_EQ(MiAllocatePage(MmSystemMemoryUsage, nullptr), 0u);
        EXPECT_EQ(MmAllocateSystemMemory(4096, PAGE_READWRITE), nullptr);
        EXPECT_EQ(MmCreateKernelStack(0x3000, FALSE), nullptr);
        EXPECT_EQ(MmAllocateContiguousMemory(4096), nullptr);
        EXPECT_EQ(ExAllocatePool(5000), nullptr);
        EXPECT_EQ(ExAllocatePool(24), nullptr);
        EXPECT_EQ(MmMapIoSpace(0x200000, 0x1000, PAGE_READWRITE) != nullptr, true)
            << "I/O mappings need no pages while the system PTE range has room";
    }
    EXPECT_EQ(MmAvailablePages, before);
    EXPECT_EQ(CheckPfnDatabase(), "");
}

TEST(NoMemory, MappedPagesUndoWhenAPageTableTakesTheLastPage) {
    // Exactly N free pages and a request for N mapped pages whose PTEs need a new page table: the table takes one
    // page, the last data page cannot be had, and everything taken so far is given back.
    std::vector<void *> fill;
    // Fill the committed system PTE range so that the next reservation commits a new page table.
    for (;;) {
        void *p = MiReserveSystemPtes(1);
        if ((ULONG)p >= 0xD0000000 + (ULONG)0x400000 * 2) { // two page tables' worth used: reached fresh ground
            MiReleaseSystemPtes(p, 1);
            break;
        }
        fill.push_back(p);
    }
    ULONG pages = 8;
    ULONG before;
    {
        Exhaust most(pages, false);
        before = MmAvailablePages;
        void *p = MmAllocateSystemMemory(pages * 4096 + 0x400000, PAGE_READWRITE); // beyond the committed range
        EXPECT_EQ(p, nullptr);
        EXPECT_LE(MmAvailablePages, before);
    }
    for (void *p : fill)
        MiReleaseSystemPtes(p, 1);
    EXPECT_EQ(CheckPfnDatabase(), "");
}

TEST(NoMemory, ObjectsHandlesAndThreads) {
    ULONG handles = ObpObjectHandleTable.HandleCount, blocks = ExpPoolBlocks;
    {
        Exhaust all;
        HANDLE h = (HANDLE)1;
        EXPECT_EQ(NtCreateEvent(&h, nullptr, NotificationEvent, FALSE), NO_RESOURCES);
        EXPECT_EQ(h, nullptr);
        EXPECT_EQ(NtCreateTimer(&h, nullptr, NotificationTimer), NO_RESOURCES);
        EXPECT_EQ(PsCreateSystemThread(&h, nullptr, nullptr, nullptr, FALSE), NO_RESOURCES);
        KFLOATING_SAVE_ save = {};
        EXPECT_EQ(KeSaveFloatingPointState(&save), NO_RESOURCES);
        PVOID base = (PVOID)0x00C00000;
        SIZE_T size = 0x10000;
        EXPECT_NE(NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS)
            << "no pool for the VAD";
    }
    EXPECT_EQ(ObpObjectHandleTable.HandleCount, handles);
    EXPECT_EQ(ExpPoolBlocks, blocks) << "nothing leaked from the failed creations";
    HANDLE h;
    EXPECT_EQ(NtCreateEvent(&h, nullptr, NotificationEvent, FALSE), STATUS_SUCCESS) << "works again";
    NtClose(h);
}

TEST(NoMemory, HandleTableCannotGrow) {
    HANDLE ev;
    ASSERT_EQ(NtCreateEvent(&ev, nullptr, NotificationEvent, FALSE), STATUS_SUCCESS);
    // Use up the free handle slots, then take the memory a new sub-table would need.
    std::vector<HANDLE> dups;
    while (ObpObjectHandleTable.FirstFreeTableEntry != -1) {
        HANDLE d;
        ASSERT_EQ(NtDuplicateObject(ev, &d, 0), STATUS_SUCCESS);
        dups.push_back(d);
    }
    {
        Exhaust all;
        HANDLE d = (HANDLE)1;
        EXPECT_NE(NtDuplicateObject(ev, &d, 0), STATUS_SUCCESS);
        EXPECT_EQ(d, nullptr);
    }
    for (HANDLE d : dups)
        NtClose(d);
    NtClose(ev);
}

TEST(NoMemory, StringsAndNames) {
    ANSI_STRING a;
    RtlInitAnsiString(&a, "a name long enough to need its own buffer");
    UNICODE_STRING u;
    {
        Exhaust all;
        EXPECT_NE(RtlAnsiStringToUnicodeString(&u, &a, TRUE), STATUS_SUCCESS);
        OBJECT_STRING name;
        RtlInitAnsiString(&name, "\\NoMemoryEvent");
        OBJECT_ATTRIBUTES at = {nullptr, &name, OBJ_CASE_INSENSITIVE};
        HANDLE h = (HANDLE)1;
        EXPECT_EQ(NtCreateEvent(&h, &at, NotificationEvent, FALSE), NO_RESOURCES);
    }
    OBJECT_STRING name;
    RtlInitAnsiString(&name, "\\NoMemoryEvent");
    PVOID o;
    EXPECT_NE(ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &o), STATUS_SUCCESS)
        << "a failed creation leaves no name behind";
}
