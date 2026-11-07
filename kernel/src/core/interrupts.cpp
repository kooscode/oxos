// IRQL, software interrupts and hardware interrupt dispatch (spec/stock/06 §1-3).
//
// Device interrupts are masked in the PIC as soon as IRQL reaches their level (eager masking), so
// a hardware interrupt never arrives at or below the current IRQL and nothing has to be replayed.
// APC/DISPATCH software interrupts are pending bits delivered when IRQL drops below them.
#include <xk/core.h>
#include <xk/serial.h>
#include <xk/arch.h>
#include <xk/hardware.h>
#include <xk/io.h>
#include <xk/memory.h>

constexpr ULONG SWI_APC = 1u << APC_LEVEL, SWI_DISPATCH = 1u << DISPATCH_LEVEL;

static volatile ULONG KiPendingSoftwareInterrupts;
static PKINTERRUPT KiInterruptObjects[16];
volatile ULONG KiIrqCounts[16]; // diagnostics: interrupts taken per IRQ line (the serial log prints them)

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

XBAPI KIRQL NTAPI KeGetCurrentIrql() {
    return KiPcr.Irql;
}

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

// KeRaiseIrqlToDpcLevel is in src/arch/in_game_reset.S: KfRaiseIrql(DISPATCH_LEVEL), plus the in-game reset hook.

XBAPI KIRQL NTAPI KeRaiseIrqlToSynchLevel() {
    return KfRaiseIrql(SYNCH_LEVEL);
}

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

#if XK_DEBUG_SERIAL
// Hang dump (serial builds): when the sampler finds the idle thread three times in a row (15 s with nothing to run
// at every sample), every thread's state, what it waits on, and the first return addresses into the title's image
// found on its stack, so a title that waits for ever names its call site and the object.
static void KiDumpThreads() {
    PLIST_ENTRY head = &KiSystemProcess.ThreadListHead;
    ULONG count = 0;
    for (PLIST_ENTRY e = head->Flink; e != head && count < 64; e = e->Flink, count++) {
        PKTHREAD t = CONTAINING_RECORD(e, KTHREAD, ThreadListEntry);
        XK_SERIAL("hang: thread %p state %u reason %u priority %d wait status %08lx alertable %u\n", t,
                  (unsigned)t->State, (unsigned)t->WaitReason, (int)t->Priority, (ULONG)t->WaitStatus,
                  (unsigned)t->Alertable);
        if (t->State == Waiting) {
            PKWAIT_BLOCK first = t->WaitBlockList, b = first;
            for (ULONG i = 0; b && i < 8; i++) {
                auto object = (DISPATCHER_HEADER *)b->Object;
                XK_SERIAL("hang:   waits on %p type %u signal %ld (wait type %d)\n", object,
                          object ? (unsigned)object->Type : 0u, object ? (long)object->SignalState : 0L,
                          (int)b->WaitType);
                b = b->NextWaitBlock;
                if (b == first)
                    break;
            }
        }
        // The thread's I/O requests that have not completed: operation, the device and driver now holding it.
        PLIST_ENTRY irps = &((PETHREAD)t)->IrpList;
        ULONG n = 0;
        for (PLIST_ENTRY i = irps->Flink; i && i != irps && n < 8; i = i->Flink, n++) {
            PIRP irp = CONTAINING_RECORD(i, IRP, ThreadListEntry);
            PFILE_OBJECT file = irp->Tail.Overlay.OriginalFileObject;
            PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
            PDEVICE_OBJECT device = stack->DeviceObject;
            XK_SERIAL("hang:   irp %p location %d major %u device %p type %u dispatch %p file %p (device %p event "
                      "%p) length %lx offset %lx:%08lx status %08lx\n", irp, (int)irp->CurrentLocation,
                      (unsigned)stack->MajorFunction, device, device ? (unsigned)device->DeviceType : 0u,
                      device && device->DriverObject ? (PVOID)device->DriverObject->MajorFunction[stack->MajorFunction]
                                                     : nullptr,
                      file, file ? file->DeviceObject : nullptr, file ? (PVOID)&file->Event : nullptr,
                      stack->Parameters.Read.Length, (ULONG)(stack->Parameters.Read.ByteOffset.QuadPart >> 32),
                      (ULONG)stack->Parameters.Read.ByteOffset.QuadPart, (ULONG)irp->IoStatus.Status);
        }
        auto sp = (PULONG)t->KernelStack, top = (PULONG)t->StackBase;
        if (sp < (PULONG)t->StackLimit || sp >= top)
            continue;
        ULONG found = 0;
        XK_SERIAL("hang:   title return addresses:");
        // Only values that follow a call instruction in a mapped page: call rel32 (E8), call [abs] (FF 15), or a
        // register / memory call (FF /2, 2 to 7 bytes long).
        for (; sp < top && found < 24; sp++) {
            ULONG r = *sp;
            if (r < 0x11000 || r >= 0x800000 || !MmIsAddressValid((PVOID)(r - 7)) || !MmIsAddressValid((PVOID)r))
                continue;
            auto c = (const UCHAR *)r;
            bool call = c[-5] == 0xE8 || (c[-6] == 0xFF && c[-5] == 0x15);
            for (int size = 2; !call && size <= 7; size++)
                call = c[-size] == 0xFF && ((c[-size + 1] >> 3) & 7) == 2;
            if (call) {
                XK_SERIAL(" %08lx", r);
                found++;
            }
        }
        XK_SERIAL("\n");
    }
}
#endif

// Called from the IRQ entry stubs with interrupts disabled.
extern "C" void KiDispatchHardwareInterrupt(ULONG irq) {
    if (HalpIsSpuriousIrq(irq))
        return;
    // The start-up watchdog (core/startup.cpp) gets the interrupted eip: in the IRQ stub's frame, past this function's
    // frame and the registers the stub saved.
    if (irq == 0)
        XkBootMarkWatchdog(((PULONG)__builtin_frame_address(0) + 3)[9], KiPcr.Irql);
#if XK_DEBUG_SERIAL
    // Sampler (serial builds): every 5 s, where the processor was interrupted, so a hang names its code.
    if (irq == 0 && KeTickCount % 5000 == 0) {
        XK_SERIAL("sample: eip %08lx irql %u thread %p\n", ((PULONG)__builtin_frame_address(0) + 3)[9],
                  (unsigned)KiPcr.Irql, KiPcr.PrcbData.CurrentThread);
        static ULONG idleSamples;
        idleSamples = KiPcr.PrcbData.CurrentThread == &KiIdleThread ? idleSamples + 1 : 0;
        if (idleSamples == 3 || idleSamples == 12)
            KiDumpThreads();
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
    // Interrupt-storm detector (serial builds): more than 5000 interrupts of one line within one clock
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
