// Elimination series (experiment, XK_EXPERIMENT_STAGE 1-3): with no title, keep the core kernel busy the way a title
// does (pool, system memory, contiguous memory, page moves, thread creation, waits, copies across RAM) and check every
// byte, so a stage that freezes or corrupts memory on real hardware shows it. Reports every 5 s.
#ifdef XK_EXPERIMENT_STAGE
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/serial.h>
#include <xk/ob.h>

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);

// Which loads run (bits): 1 pool, 2 system memory, 4 contiguous memory, 8 threads; 0 = none (heartbeat only).
#ifndef XK_STRESS_MASK
#ifdef XK_EXPERIMENT_STRESS_NO_CONTIGUOUS
#define XK_STRESS_MASK 11
#else
#define XK_STRESS_MASK 15
#endif
#endif
static volatile ULONG KiStressRounds, KiStressErrors, KiStressThreads;

static ULONG KiStressRandom(ULONG &x) {
    x = x * 1664525 + 1013904223;
    return x >> 8;
}

static bool KiStressCheck(const ULONG *p, ULONG words, ULONG seed) {
    for (ULONG i = 0; i < words; i++)
        if (p[i] != (seed ^ (i * 0x9E3779B9))) {
            KiStressErrors = KiStressErrors + 1;
            if (KiStressErrors <= 8)
                XK_SERIAL("*** stress: %p word %lu is %08lx, want %08lx\n", p + i, i, p[i], seed ^ (i * 0x9E3779B9));
            return false;
        }
    return true;
}

static void KiStressFill(ULONG *p, ULONG words, ULONG seed) {
    for (ULONG i = 0; i < words; i++)
        p[i] = seed ^ (i * 0x9E3779B9);
}

static void NTAPI KiStressShortThread(PVOID) { KiStressThreads = KiStressThreads + 1; }

static void NTAPI KiStressThread(PVOID Context) {
    ULONG x = (ULONG)Context * 7919 + 1;
    PVOID blocks[64] = {};
    ULONG sizes[64] = {};
    for (;;) {
        // Pool: replace a random block, checking the old one's contents first.
        for (int k = 0; k < ((XK_STRESS_MASK & 1) ? 64 : 0); k++) {
            ULONG i = KiStressRandom(x) % 64;
            if (blocks[i]) {
                KiStressCheck((ULONG *)blocks[i], sizes[i] / 4, (ULONG)blocks[i]);
                ExFreePool(blocks[i]);
            }
            sizes[i] = 4 + (KiStressRandom(x) % (KiStressRandom(x) % 4 ? 1000 : 9000)) & ~3u;
            blocks[i] = ExAllocatePoolWithTag(sizes[i], 'srtS');
            if (blocks[i])
                KiStressFill((ULONG *)blocks[i], sizes[i] / 4, (ULONG)blocks[i]);
        }
        // System memory and contiguous memory (contiguous requests move pages that are in use).
        ULONG bytes = (1 + KiStressRandom(x) % 16) * PAGE_SIZE;
        if (auto s = (XK_STRESS_MASK & 2) ? (ULONG *)MmAllocateSystemMemory(bytes, PAGE_READWRITE) : nullptr) {
            KiStressFill(s, bytes / 4, bytes);
            KiStressCheck(s, bytes / 4, bytes);
            MmFreeSystemMemory(s, bytes);
        }
        bytes = (1 + KiStressRandom(x) % 32) * PAGE_SIZE;
#if XK_STRESS_MASK & 4
        if (auto c = (ULONG *)MmAllocateContiguousMemory(bytes)) {
            KiStressFill(c, bytes / 4, 0xC0C0 ^ bytes);
            KiStressCheck(c, bytes / 4, 0xC0C0 ^ bytes);
            MmFreeContiguousMemory(c);
        }
#endif
        // A short-lived thread now and then, and a short wait.
        if ((XK_STRESS_MASK & 8) && KiStressRandom(x) % 8 == 0)
            if (PKTHREAD t = KiCreateSystemThread(KiStressShortThread, nullptr, 0x3000, 8)) {
                KeWaitForSingleObject(t, Executive, KernelMode, FALSE, nullptr);
                ObfDereferenceObject(t);
            }
        LARGE_INTEGER t;
        t.QuadPart = -10000LL * (KiStressRandom(x) % 3);
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        KiStressRounds = KiStressRounds + 1;
    }
}

ULONG KiStressPteRuns(ULONG *longest); // mm/init.cpp

static void NTAPI KiStressReporter(PVOID) {
    for (ULONG n = 1;; n++) {
        LARGE_INTEGER t;
        t.QuadPart = -50000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        ULONG longest, runs = KiStressPteRuns(&longest);
        XK_SERIAL("stress %lu: %lu rounds, %lu errors, %lu threads, %lu pages free; system PTE list %lu runs, longest %lu\n",
                  n * 5, KiStressRounds, KiStressErrors, KiStressThreads, MmAvailablePages, runs, longest);
    }
}

#ifdef XK_STRESS_NO_INTERRUPTS
// Variant: the same pool load on the start-up thread with interrupts off for good (no clock, no thread switch, no
// DPC): a freeze here cannot come from an interrupt landing at the wrong moment.
static void KiStressWithoutInterrupts() {
    __asm__ volatile("cli");
    XK_SERIAL("stress: pool load with interrupts off, on the start-up thread\n");
    ULONG x = 12345;
    PVOID blocks[64] = {};
    ULONG sizes[64] = {};
    for (ULONG round = 1;; round++) {
        for (int k = 0; k < 64; k++) {
            ULONG i = KiStressRandom(x) % 64;
            if (blocks[i]) {
                KiStressCheck((ULONG *)blocks[i], sizes[i] / 4, (ULONG)blocks[i]);
                ExFreePool(blocks[i]);
            }
            sizes[i] = 4 + (KiStressRandom(x) % (KiStressRandom(x) % 4 ? 1000 : 9000)) & ~3u;
            blocks[i] = ExAllocatePoolWithTag(sizes[i], 'srtS');
            if (blocks[i])
                KiStressFill((ULONG *)blocks[i], sizes[i] / 4, (ULONG)blocks[i]);
        }
        if (round % 2000 == 0)
            XK_SERIAL("stress (interrupts off): %lu rounds, %lu errors, %lu pages free\n", round, KiStressErrors,
                      MmAvailablePages);
    }
}
#endif

#ifdef XK_STRESS_MEMORY_ONLY
// Variant: memory traffic only (fill and check 1 MB, no kernel calls), interrupts on.
static ULONG KiStressBuffer[1u << 18];
static void NTAPI KiStressMemoryThread(PVOID Context) {
    ULONG seed = (ULONG)Context;
    ULONG *part = KiStressBuffer + (seed & 1) * (1u << 17);
    for (;;) {
        seed = seed * 1664525 + 1013904223;
        KiStressFill(part, 1u << 17, seed);
        KiStressCheck(part, 1u << 17, seed);
        KiStressRounds = KiStressRounds + 1;
    }
}
#endif

extern "C" void KiStartStressThreads() {
#ifdef XK_STRESS_NO_INTERRUPTS
    KiStressWithoutInterrupts();
#endif
#ifdef XK_STRESS_MEMORY_ONLY
    XK_SERIAL("stress: memory traffic only (fill and check 1 MB), two threads at priority 8\n");
    KiCreateSystemThread(KiStressMemoryThread, (PVOID)1, 0x4000, 8);
    KiCreateSystemThread(KiStressMemoryThread, (PVOID)2, 0x4000, 8);
    KiCreateSystemThread(KiStressReporter, nullptr, 0x3000, 16);
    return;
#endif
    XK_SERIAL("stress: loads %#x (1 pool, 2 system memory, 4 contiguous, 8 threads), two threads at priority 8\n",
              XK_STRESS_MASK);
    if (!XK_STRESS_MASK) {
        KiCreateSystemThread(KiStressReporter, nullptr, 0x3000, 16);
        return;
    }
    KiCreateSystemThread(KiStressThread, (PVOID)1, 0x4000, 8);
    KiCreateSystemThread(KiStressThread, (PVOID)2, 0x4000, 8);
    KiCreateSystemThread(KiStressReporter, nullptr, 0x3000, 16);
}
#endif
