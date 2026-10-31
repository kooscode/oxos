// Game-trainer support (spec/oxos-internal/23-trainer-hook.md): the title-start call has the exact bytes dashboards
// search for, their patch round-trips on it, and the low memory their trainer stub uses stays the dashboard's.
// The kernel image itself is checked at link time too (tools/check_kernel_layout.py, check 3).
#include "kernel_test.h"

#include <algorithm>
#include <cstring>

#include <xk/title.h>

// Compiled from the same header as the kernel's title thread (src/xe/loader.cpp). Never called.
__attribute__((noinline, used)) static void TitleStartForTest() {
    XeCallTitleEntry();
}

namespace {
// The first `pattern` in [code, code + size), as a dashboard searches the kernel image byte by byte.
UCHAR *Find(UCHAR *code, size_t size, const UCHAR (&pattern)[6]) {
    auto hit = std::search(code, code + size, pattern, pattern + 6);
    return hit == code + size ? nullptr : hit;
}
} // namespace

TEST(Trainer, TitleStartIsTheExactCallDashboardsSearchFor) {
    UCHAR code[64];
    memcpy(code, (const void *)&TitleStartForTest, sizeof(code));
    UCHAR *call = Find(code, sizeof(code), XE_TITLE_CALL);
    ASSERT_NE(call, nullptr) << "the title start must be FF 15 28 01 01 00 (call [0x00010128])";
    EXPECT_EQ(Find(call + 1, code + sizeof(code) - call - 1, XE_TITLE_CALL), nullptr) << "exactly one call";
    EXPECT_EQ(Find(code, sizeof(code), XE_TITLE_CALL_HOOKED), nullptr);
}

TEST(Trainer, DashboardHookAndUnhookRoundTrip) {
    UCHAR code[64];
    memcpy(code, (const void *)&TitleStartForTest, sizeof(code));
    // Install (XBMC4Gamers 0x116F89): the call's address field becomes 0x8000000C.
    UCHAR *call = Find(code, sizeof(code), XE_TITLE_CALL);
    ASSERT_NE(call, nullptr);
    *(ULONG *)(call + 2) = 0x8000000C;
    EXPECT_EQ(Find(code, sizeof(code), XE_TITLE_CALL), nullptr);
    // Remove (XBMC4Gamers 0x117352, at every start): found again and put back.
    UCHAR *hooked = Find(code, sizeof(code), XE_TITLE_CALL_HOOKED);
    ASSERT_EQ(hooked, call);
    *(ULONG *)(hooked + 2) = 0x00010128;
    EXPECT_EQ(memcmp(code, (const void *)&TitleStartForTest, sizeof(code)), 0);
}

TEST(Trainer, PageZeroIsMappedPersistentAndNeverAllocated) {
    // The stub pointer lives at physical 0xC, the stub's own slots at 0x10 and 0x14; all must survive the quick
    // reboot into the game, so page 0 stays mapped read-write and persistent and is never handed out.
    uint32_t pte = PfnEntry(0);
    EXPECT_EQ(pte & (PTE_PRESENT | PTE_WRITE | PTE_PERSIST), PTE_PRESENT | PTE_WRITE | PTE_PERSIST);
    EXPECT_EQ(pte & ~0xFFFu, 0u);
    auto slots = (volatile ULONG *)0x8000000C;
    ULONG saved[3] = {slots[0], slots[1], slots[2]};
    slots[0] = 0x80123000, slots[1] = 0x11111111, slots[2] = 0x22222222;
    EXPECT_EQ(((uint32_t *)host::Phys(0))[3], 0x80123000u);
    EXPECT_EQ(((uint32_t *)host::Phys(0))[5], 0x22222222u);
    slots[0] = saved[0], slots[1] = saved[1], slots[2] = saved[2];
    EXPECT_EQ(MmAllocateContiguousMemoryEx(4096, 0, 0xFFF, 0, PAGE_READWRITE), nullptr);
}
