// What the core sources reference outside the files under test: boot-time data from entry.S / interrupt_stubs.S, the
// serial log, and the HAL and executive pieces the tests do not cover. Each stub is the smallest correct behaviour.
#include "host.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>

#include <xk/core.h>
#include <xk/runtime.h>
#include <xk/types.h>
#include <xk/bootscreen.h>

using namespace xk; // the kernel's subsystems as storage::, core::, ...

// entry.S / interrupt_stubs.S
extern "C" {
alignas(16) unsigned char KiBootStack[KI_BOOT_STACK_SIZE];
void *KiTrapStubs[32];
void *KiIrqStubs[16];
void KiIgnoreInterrupt() {
    // The harness raises only the interrupts it models.
}

void KiDoubleFaultTask() {
    // No task switches on the host.
}

void KiNmiTask() {
    // No task switches on the host.
}

void KiTrap45() {
    // Debug service (int 2Dh): not raised in host tests.
}
}

// hardware/serial.cpp: the kernel log goes to the harness (shown for failing tests, or with OXOS_TEST_LOG=1).
namespace xk::hardware {

volatile BOOLEAN SerialPresent = TRUE;

void InitSerial() {
    // Nothing to set up: WriteSerial hands the text to the harness.
}

void WriteSerial(const char *Text, ULONG Length) {
    HostLogWrite(Text, Length);
}

void SerialPrint(const char *Format, ...) {
    char line[256];
    va_list ap;
    va_start(ap, Format);
    int n = RtlVsnprintf(line, sizeof(line), Format, ap);
    va_end(ap);
    HostLogWrite(line, n < 0 ? 0 : n >= (int)sizeof(line) ? sizeof(line) - 1 : (size_t)n);
}

} // namespace xk::hardware

// core/startup.cpp: the start-up marks and last words (CMOS records) are not under test here.
namespace xk::core {
volatile BOOLEAN BootMarksOn;

void BootMark(const char *) {
    // Not under test.
}

void BootMarkWatchdog(ULONG, UCHAR) {
    // Not under test.
}

void SaveBugCheckLastWords(ULONG, ULONG, ULONG) {
    // Not under test.
}

void SaveFaultTaskLastWords(ULONG, ULONG) {
    // Not under test.
}
} // namespace xk::core

// filesystems/cache.cpp: the file-system cache is not under test (no cache pages exist, so no page has this owner).
namespace xk::filesystems {
ULONG *CachePte(ULONG) {
    return nullptr;
}
} // namespace xk::filesystems

// arch/in_game_reset.S: KeRaiseIrqlToDpcLevel is assembly there (it also watches for the in-game reset combination);
// here it is what it does for kernel callers.
XBAPI KIRQL NTAPI KeRaiseIrqlToDpcLevel() {
    return KfRaiseIrql(DISPATCH_LEVEL);
}

// bootscreen/splash.cpp: no splash copy in host tests (no page holds one).
bool xk::bootscreen::IsSplashPage(ULONG) {
    return false;
}

// titles/nettitle.cpp: no network-delivered title in host tests (no page holds one).
namespace xk::titles {
bool IsNetbootTitlePage(ULONG) {
    return false;
}
} // namespace xk::titles
