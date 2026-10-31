// What the core sources reference outside the files under test: boot-time data from entry.S / isr.S, the serial
// log, and the HAL and executive pieces the tests do not cover. Each stub is the smallest correct behaviour.
#include <cstdarg>
#include <cstddef>
#include <cstdio>

#include <xk/types.h>
#include <xk/ke.h>

extern "C" void HostLogWrite(const char *s, size_t n);
XBAPI int __cdecl RtlVsnprintf(char *Buffer, SIZE_T Size, const char *Format, va_list Args);

// entry.S / isr.S
extern "C" {
alignas(16) unsigned char KiBootStack[0x3000];
void *KiTrapStubs[32];
void *KiIrqStubs[16];
void KiIgnoreInterrupt() {}
void KiDoubleFaultTask() {}
void KiNmiTask() {}
void KiTrap45() {} // debug service (int 2Dh): not raised in host tests
}

// hal/serial.cpp: the kernel log goes to the harness (shown for failing tests, or with OXOS_TEST_LOG=1).
volatile BOOLEAN HalpSerialPresent = TRUE;
void HalpSerialInit() {}
void HalpSerialWrite(const char *Text, ULONG Length) { HostLogWrite(Text, Length); }
void XkSerialPrint(const char *Format, ...) {
    char line[256];
    va_list ap;
    va_start(ap, Format);
    int n = RtlVsnprintf(line, sizeof(line), Format, ap);
    va_end(ap);
    HostLogWrite(line, n < 0 ? 0 : n >= (int)sizeof(line) ? sizeof(line) - 1 : (size_t)n);
}

// ke/startup.cpp: the start-up marks and last words (CMOS records) are not under test here.
extern "C" {
volatile BOOLEAN XkBootMarksOn;
void XkBootMark(const char *) {}
void XkBootMarkWatchdog(ULONG, UCHAR) {}
void KiSaveBugCheckLastWords(ULONG, ULONG, ULONG) {}
void KiSaveFaultTaskLastWords(ULONG, ULONG) {}
}

// io/fsc.cpp: the file-system cache is not under test (no cache pages exist, so no page has this owner).
ULONG *FscCachePte(ULONG) { return nullptr; }

// arch/igr.S: KeRaiseIrqlToDpcLevel is assembly there (it also watches for the in-game reset combination); here it
// is what it does for kernel callers.
XBAPI KIRQL NTAPI KeRaiseIrqlToDpcLevel() { return KfRaiseIrql(DISPATCH_LEVEL); }

// hal/splash.cpp: no splash copy in host tests (no page holds one).
bool HalpIsSplashPage(ULONG) { return false; }

// xe/nettitle.cpp: no network-delivered title in host tests (no page holds one).
bool MiIsNetbootTitlePage(ULONG) { return false; }
