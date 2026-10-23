// Boot the kernel core in the host test process, then run the tests on a kernel thread.
#include "host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/ob.h>

extern "C" unsigned char KiBootStack[];
void KiInitializeDpcs();
PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void ExInitializePool();

namespace host {
std::string CurrentTestName;

// What the 2BL leaves (kernel/boot/bl.cpp): its page directory at 0xF000 (identity and 0x80000000 large pages for
// 256 MB, the self-map, the GPU window, flash) and the kernel image at 0x10000 (here only its PE header, which the
// memory manager reads for the image extent).
static void Make2blState() {
    memset(Phys(0), 0, PHYS_SIZE);
    auto pd = (uint32_t *)Phys(0xF000);
    for (uint32_t i = 0; i < 0x40; i++) {
        pd[i] = (i << 22) | 0xE3;
        pd[0x200 + i] = (i << 22) | 0xE3;
    }
    pd[0x300] = 0xF000 | 0x63;
    for (uint32_t i = 0; i < 4; i++)
        pd[0x3F4 + i] = (0xFD000000 + (i << 22)) | 0xFB;
    pd[0x3FF] = 0xFFC00000 | 0xE3;

    uint8_t *image = Phys(0x10000);
    image[0] = 'M', image[1] = 'Z';
    *(uint32_t *)(image + 0x3C) = 0x80;
    uint8_t *nt = image + 0x80;
    memcpy(nt, "PE\0\0", 4);
    *(uint16_t *)(nt + 4) = 0x14C;  // i386
    *(uint16_t *)(nt + 6) = 1;      // sections
    *(uint16_t *)(nt + 20) = 0xE0;  // optional header size
    *(uint32_t *)(nt + 24 + 56) = 0x40000; // SizeOfImage: 256 KB, about the real kernel's
    uint8_t *section = nt + 24 + 0xE0;
    memcpy(section, ".text\0\0\0", 8);
    *(uint32_t *)(section + 8) = 0x30000;  // VirtualSize
    *(uint32_t *)(section + 12) = 0x1000;  // VirtualAddress
    Cr3 = 0xF000;
}

struct RunArgs {
    int (*body)(void *);
    void *context;
};

static void NTAPI TestThread(PVOID p) {
    auto a = (RunArgs *)p;
    int rc = a->body(a->context);
    fflush(stdout);
    fflush(stderr);
    exit(rc); // from the kernel thread: the idle loop never returns
}

int BootAndRun(int (*body)(void *), void *context) {
    Make2blState();
    KiInitializeProcessor();
    HalpInitPic();
    HalpInitPit();
    MmInitSystem(false);
    KiInitializeDpcs();
    KiInitializeTimers();
    KiInitializeScheduler(KiBootStack + 0x3000, KiBootStack);
    ObInitSystem();
    HalEnableSystemInterrupt(0, Latched); // the clock
    static RunArgs args;
    args = {body, context};
    if (!KiCreateSystemThread(TestThread, &args, 1u << 20, 8)) {
        fprintf(stderr, "host: cannot create the test thread\n");
        return 96;
    }
    KfLowerIrql(DISPATCH_LEVEL);
    KiIdleLoop();
}

} // namespace host

namespace {
// Resets the deadlock guard and names the test in the harness's own reports.
class HarnessListener : public testing::EmptyTestEventListener {
    void OnTestStart(const testing::TestInfo &info) override {
        host::IdleTicksThisTest = 0;
        host::CurrentTestName = std::string(info.test_suite_name()) + "." + info.name();
    }
};
} // namespace

static int RunTests(void *) {
    testing::UnitTest::GetInstance()->listeners().Append(new HarnessListener);
    return RUN_ALL_TESTS();
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return host::BootAndRun(RunTests, nullptr);
}
