// IRQL, software interrupts and hardware interrupt dispatch (spec/stock/06 §1-3).
//
// Device interrupts are masked in the PIC as soon as IRQL reaches their level (eager masking), so
// a hardware interrupt never arrives at or below the current IRQL and nothing has to be replayed.
// APC/DISPATCH software interrupts are pending bits delivered when IRQL drops below them.
#include <xk/serial.h>
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

constexpr ULONG SWI_APC = 1u << APC_LEVEL, SWI_DISPATCH = 1u << DISPATCH_LEVEL;

static volatile ULONG KiPendingSoftwareInterrupts;
static PKINTERRUPT KiInterruptObjects[16];
volatile ULONG KiIrqCounts[16]; // diagnostics: interrupts taken per IRQ line (the network log's heartbeat prints them)

// Interrupts are disabled on entry and exit; enabled while a handler runs.
void KiDeliverSoftwareInterrupts() {
    for (;;) {
        KIRQL current = KiPcr.Irql;
        if ((KiPendingSoftwareInterrupts & SWI_DISPATCH) && current < DISPATCH_LEVEL) {
            KiPendingSoftwareInterrupts &= ~SWI_DISPATCH;
            KiPcr.Irql = DISPATCH_LEVEL;
            sti();
            KiDispatchInterrupt();
            cli();
            KiPcr.Irql = current;
            continue;
        }
        if ((KiPendingSoftwareInterrupts & SWI_APC) && current < APC_LEVEL) {
            KiPendingSoftwareInterrupts &= ~SWI_APC;
            KiPcr.Irql = APC_LEVEL;
            sti();
            KiDeliverApc();
            cli();
            KiPcr.Irql = current;
            continue;
        }
        return;
    }
}

XBAPI KIRQL NTAPI KeGetCurrentIrql() { return KiPcr.Irql; }

XBAPI KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql) {
    KIRQL old = KiPcr.Irql;
    // Never lower (NT's rule; checked builds bug-check): the boot runs at HIGH_LEVEL, and the IDE driver's "raise to
    // the channel IRQL" would otherwise drop to 12 for a whole device probe, with the PIC mask and the IRQL apart.
    if (NewIrql < old)
        return old;
    if (NewIrql > DISPATCH_LEVEL) {
        ULONG flags = save_flags_cli();
        KiPcr.Irql = NewIrql;
        HalpSetIrqlMask(NewIrql);
        restore_flags(flags);
    } else {
        KiPcr.Irql = NewIrql;
    }
    return old;
}

XBAPI void FASTCALL KfLowerIrql(KIRQL NewIrql) {
    ULONG flags = save_flags_cli();
    KIRQL old = KiPcr.Irql;
    KiPcr.Irql = NewIrql;
    if (old > DISPATCH_LEVEL)
        HalpSetIrqlMask(NewIrql);
    if (KiPendingSoftwareInterrupts)
        KiDeliverSoftwareInterrupts();
    restore_flags(flags);
}

XBAPI KIRQL NTAPI KeRaiseIrqlToDpcLevel() { return KfRaiseIrql(DISPATCH_LEVEL); }

XBAPI KIRQL NTAPI KeRaiseIrqlToSynchLevel() { return KfRaiseIrql(SYNCH_LEVEL); }

XBAPI void FASTCALL HalRequestSoftwareInterrupt(KIRQL RequestIrql) {
    ULONG flags = save_flags_cli();
    KiPendingSoftwareInterrupts |= 1u << RequestIrql;
    if (RequestIrql > KiPcr.Irql)
        KiDeliverSoftwareInterrupts();
    restore_flags(flags);
}

XBAPI void FASTCALL HalClearSoftwareInterrupt(KIRQL RequestIrql) {
    ULONG flags = save_flags_cli();
    KiPendingSoftwareInterrupts &= ~(1u << RequestIrql);
    restore_flags(flags);
}

#if XK_NETLOG
// PC sampling (network-log builds, once the title runs): every 100 clock ticks the clock interrupt sends the
// interrupted EIP, IRQL, EFLAGS and the top stack words, so a console that spins at raised IRQL shows where, and one
// that freezes with interrupts off shows the last place it ran.
void HalpNetLogWrite(const char *Text, ULONG Length);
XBAPI int __cdecl RtlSnprintf(char *Buffer, SIZE_T Size, const char *Format, ...);
extern "C" volatile ULONG KiSampleTitlePc;
volatile ULONG KiSampleTitlePc;
extern "C" volatile ULONG KiPcRing[32]; // the interrupted EIP of the last 32 clock ticks (the watchdog prints them)
volatile ULONG KiPcRing[32];
void HalpNetWatchdog();
static void KiSamplePc(PULONG frame, KIRQL irql) {
    // frame: eight saved registers (pushal), the IRQ number pushed by the stub, then EIP, CS, EFLAGS of the interrupted code
    // (same privilege: no SS:ESP), then its stack.
    char line[160];
    int n = RtlSnprintf(line, sizeof(line), "pc %#lx irql %u efl %#lx stack %#lx %#lx %#lx %#lx\n", frame[9], irql,
                        frame[11], frame[12], frame[13], frame[14], frame[15]);
    HalpNetLogWrite(line, (ULONG)n);
}
#endif

#ifdef XK_EXPERIMENT_EARLY_SAMPLE
// Experiment: from the network log's start to "clock interrupt on", IRQ 0 stays unmasked at every IRQL and only
// samples the interrupted EIP (10 per second), so a hang in phase 1 shows where the CPU is.
extern "C" volatile ULONG KiEarlySample;
volatile ULONG KiEarlySample;
[[maybe_unused]] static ULONG KiEarlyTicks;
#endif

extern "C" void XkBootMarkWatchdog(ULONG Eip, UCHAR Irql);

// Called from the IRQ entry stubs with interrupts disabled.
extern "C" void KiDispatchHardwareInterrupt(ULONG irq) {
    if (HalpIsSpuriousIrq(irq))
        return;
    if (irq == 0) // start-up watchdog (ke/startup.cpp); the frame as KiSamplePc reads it
        XkBootMarkWatchdog(((PULONG)__builtin_frame_address(0) + 3)[9], KiPcr.Irql);
#ifdef XK_EXPERIMENT_EARLY_SAMPLE
    if (irq == 0 && KiEarlySample) {
#if XK_NETLOG && !defined(XK_EARLY_SAMPLE_QUIET)
        if (++KiEarlyTicks % 100 == 0)
            KiSamplePc((PULONG)__builtin_frame_address(0) + 3, KiPcr.Irql);
#endif // quiet: only the start-up watchdog above runs (CMOS, any build)
        HalpEoi(0);
        return;
    }
#endif
#if XK_NETLOG
    // The stub's frame starts above our argument: EBP + 8 is irq, then the eight registers of pushal (built with
    // frame pointers: -fno-omit-frame-pointer).
    if (irq == 0 && KiSampleTitlePc) {
        PULONG frame = (PULONG)__builtin_frame_address(0) + 3;
        KiPcRing[KeTickCount % 32] = frame[9];
        if (KeTickCount % 100 == 0)
            KiSamplePc(frame, KiPcr.Irql);
        else if (KeTickCount % 100 == 50) {
            void HalpScreenRefresh(); // hal/splash.cpp (C linkage here)
            HalpScreenRefresh();
        }
        HalpNetWatchdog();
    }
#endif
    KIRQL old = KiPcr.Irql;
    KIRQL level = HalpGetIrqLevel(irq);
    KiPcr.Irql = level;
    HalpSetIrqlMask(level);
    HalpEoi(irq);
    KiPcr.PrcbData.InterruptCount++;
    if (irq < 16)
        KiIrqCounts[irq] = KiIrqCounts[irq] + 1;
#if XK_DEBUG_SERIAL
    // Interrupt-storm detector (serial and network-log builds): more than 5000 interrupts of one line within one clock
    // tick (1 ms) freezes the console; log it once and mask the line, so the system and its log keep running.
    if (irq) {
        static ULONG stormTick[16], stormCount[16];
        if (stormTick[irq] != KeTickCount) {
            stormTick[irq] = KeTickCount;
            stormCount[irq] = 0;
        }
        if (++stormCount[irq] == 5000) {
            XK_SERIAL("*** interrupt storm on IRQ %lu (%lu taken in all): masked\n", irq, KiIrqCounts[irq]);
            HalDisableSystemInterrupt(irq);
            KiPcr.Irql = old;
            HalpSetIrqlMask(old);
            return;
        }
    }
#endif

    sti();
    if (irq == 0) {
        KiClockInterrupt(old);
    } else if (PKINTERRUPT obj = KiInterruptObjects[irq]) {
        obj->ServiceCount++;
        obj->ServiceRoutine(obj, obj->ServiceContext);
    }
    cli();

    KiPcr.Irql = old;
    HalpSetIrqlMask(old);
    if (KiPendingSoftwareInterrupts)
        KiDeliverSoftwareInterrupts();
}

// --- Interrupt objects (spec/stock/06 §3). The IDT keeps fixed per-IRQ stubs that look up the
// connected object, so DispatchCode is only filled with a recognisable marker.

XBAPI void NTAPI KeInitializeInterrupt(PKINTERRUPT Interrupt, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext,
                                       ULONG Vector, KIRQL Irql, KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector) {
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->BusInterruptLevel = Vector - HAL_IRQ_BASE_VECTOR;
    Interrupt->Irql = Irql;
    Interrupt->ShareVector = ShareVector;
    Interrupt->Mode = (UCHAR)InterruptMode;
    Interrupt->Connected = FALSE;
    for (ULONG &w : Interrupt->DispatchCode)
        w = 0xCCCCCCCC; // int3: never executed
}

XBAPI BOOLEAN NTAPI KeConnectInterrupt(PKINTERRUPT Interrupt) {
    ULONG level = Interrupt->BusInterruptLevel;
    BOOLEAN connected = FALSE;
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    if (!Interrupt->Connected && level > 0 && level < 16 && !KiInterruptObjects[level]) {
        KiInterruptObjects[level] = Interrupt;
        Interrupt->Connected = TRUE;
        HalpSetIrqLevel(level, (KIRQL)Interrupt->Irql);
        HalEnableSystemInterrupt(level, (KINTERRUPT_MODE)Interrupt->Mode);
        connected = TRUE;
    }
    KfLowerIrql(old);
    return connected;
}

XBAPI BOOLEAN NTAPI KeDisconnectInterrupt(PKINTERRUPT Interrupt) {
    BOOLEAN wasConnected = Interrupt->Connected;
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    if (wasConnected) {
        ULONG level = Interrupt->BusInterruptLevel;
        HalDisableSystemInterrupt(level);
        KiInterruptObjects[level] = nullptr;
        HalpResetIrqLevel(level);
        Interrupt->Connected = FALSE;
    }
    KfLowerIrql(old);
    return wasConnected;
}
