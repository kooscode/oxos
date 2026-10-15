// SMBus host controller (MCPX, I/O 0xC000), SMC services, firmware return (spec/12).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

constexpr USHORT SMB_STATUS = 0xC000, SMB_CONTROL = 0xC002, SMB_ADDRESS = 0xC004, SMB_DATA = 0xC006,
                 SMB_COMMAND = 0xC008;
constexpr UCHAR SMB_STATUS_COLLISION = 0x02, SMB_STATUS_PROTOCOL = 0x04,
                SMB_STATUS_DONE = 0x10, SMB_STATUS_TIMEOUT = 0x20;
constexpr NTSTATUS STATUS_IO_TIMEOUT_ = (NTSTATUS)0xC00000B5, STATUS_IO_DEVICE_ERROR_ = (NTSTATUS)0xC0000185;

static KEVENT HalpSMBusLock;
static bool HalpSMBusLockReady;

static void HalpAcquireSMBus() {
    if (!HalpSMBusLockReady) {
        KeInitializeEvent(&HalpSMBusLock, SynchronizationEvent, TRUE);
        HalpSMBusLockReady = true;
    }
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
        KeWaitForSingleObject(&HalpSMBusLock, Executive, KernelMode, FALSE, nullptr);
}

static void HalpReleaseSMBus() {
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
        KeSetEvent(&HalpSMBusLock, 0, FALSE);
}

// One byte/word transaction, polled. The reference retries collisions/protocol errors up to 20 times, 2 ms apart.
static NTSTATUS HalpSMBusTransaction(UCHAR Address, UCHAR Command, BOOLEAN WordData, bool Read, USHORT *Value) {
    for (int attempt = 0; attempt < 20; attempt++) {
        outb(SMB_ADDRESS, Read ? (UCHAR)(Address | 1) : (UCHAR)(Address & 0xFE));
        outb(SMB_COMMAND, Command);
        if (!Read)
            outw(SMB_DATA, *Value);
        outw(SMB_STATUS, inw(SMB_STATUS)); // clear
        outb(SMB_CONTROL, WordData ? 0x0B : 0x0A); // start, byte/word data (no interrupt: polled)
        UCHAR status = 0;
        for (ULONG us = 0; us < 50000; us += 10) {
            status = inb(SMB_STATUS);
            if (status & (SMB_STATUS_DONE | SMB_STATUS_TIMEOUT | SMB_STATUS_COLLISION | SMB_STATUS_PROTOCOL))
                break;
            KeStallExecutionProcessor(10);
        }
        if (status & SMB_STATUS_DONE) {
            if (Read)
                *Value = WordData ? inw(SMB_DATA) : (USHORT)inb(SMB_DATA);
            return STATUS_SUCCESS;
        }
        if (!(status & (SMB_STATUS_COLLISION | SMB_STATUS_PROTOCOL)))
            return (status & SMB_STATUS_TIMEOUT) ? STATUS_IO_TIMEOUT_ : STATUS_IO_DEVICE_ERROR_;
        KeStallExecutionProcessor(2000);
    }
    return STATUS_IO_DEVICE_ERROR_;
}

XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue,
                                       PULONG DataValue) {
    HalpAcquireSMBus();
    USHORT v = 0;
    NTSTATUS status = HalpSMBusTransaction(SlaveAddress, CommandCode, ReadWordValue, true, &v);
    HalpReleaseSMBus();
    *DataValue = v;
    return status;
}

XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue,
                                        ULONG DataValue) {
    HalpAcquireSMBus();
    USHORT v = (USHORT)DataValue;
    NTSTATUS status = HalpSMBusTransaction(SlaveAddress, CommandCode, WriteWordValue, false, &v);
    HalpReleaseSMBus();
    return status;
}

// --- SMC (SMBus address 0x20)

constexpr UCHAR SMC_ADDRESS = 0x20, SMC_RESET = 0x02, SMC_TRAY_STATE = 0x03, SMC_SCRATCH = 0x1B;
static volatile ULONG HalpTrayState = 0xFFFFFFFF, HalpTrayStateChangeCount; // updated by SMC events

// Tray state: 0x10 open/closing, 0x40 no media, 0x60 media present (other values read as 0x10).
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount) {
    ULONG state = HalpTrayState, count = HalpTrayStateChangeCount;
    NTSTATUS status = STATUS_SUCCESS;
    if (state == 0xFFFFFFFF) {
        ULONG v;
        status = HalReadSMBusValue(SMC_ADDRESS, SMC_TRAY_STATE, FALSE, &v);
        if (NT_SUCCESS(status)) {
            state = v & 0x70;
            if (state != 0x40 && state != 0x60)
                state = 0x10;
        }
    }
    *TrayState = state;
    if (TrayStateChangeCount)
        *TrayStateChangeCount = count;
    return status;
}

// --- Shutdown notifications (priority-ordered, highest first) and SMC housekeeping

typedef void(NTAPI *PHAL_SHUTDOWN_NOTIFICATION)(struct _HAL_SHUTDOWN_REGISTRATION *ShutdownRegistration);
typedef struct _HAL_SHUTDOWN_REGISTRATION {
    PHAL_SHUTDOWN_NOTIFICATION NotificationRoutine;
    LONG Priority;
    LIST_ENTRY ListEntry;
} HAL_SHUTDOWN_REGISTRATION, *PHAL_SHUTDOWN_REGISTRATION;

static LIST_ENTRY HalpShutdownList = {&HalpShutdownList, &HalpShutdownList};
static volatile UCHAR HalpShutdownPending;
static ULONG HalpSmcScratch;
// Boot flags (the reference's 0x8003B138, kept across quick reboots), derived from the SMC at cold boot (spec/12 §4):
// 0x08 SMC "reset on eject" off (0x19 = 1), 0x10/0x20/0x80 from the scratch register, 0x40 tray ejected at boot.
XK_PERSIST volatile ULONG HalpBootFlags;

XBAPI void NTAPI HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION ShutdownRegistration, BOOLEAN Register) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (!Register) {
        for (PLIST_ENTRY e = HalpShutdownList.Flink; e != &HalpShutdownList; e = e->Flink)
            if (e == &ShutdownRegistration->ListEntry) {
                RemoveEntryList(e);
                break;
            }
    } else {
        PLIST_ENTRY e = HalpShutdownList.Flink;
        while (e != &HalpShutdownList &&
               CONTAINING_RECORD(e, HAL_SHUTDOWN_REGISTRATION, ListEntry)->Priority >= ShutdownRegistration->Priority)
            e = e->Flink;
        InsertTailList(e, &ShutdownRegistration->ListEntry); // in front of the first lower priority
    }
    KfLowerIrql(old);
}

static void HalpRunShutdownNotifications() {
    for (;;) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        PLIST_ENTRY e = IsListEmpty(&HalpShutdownList) ? nullptr : RemoveHeadList(&HalpShutdownList);
        KfLowerIrql(old);
        if (!e)
            break;
        auto r = CONTAINING_RECORD(e, HAL_SHUTDOWN_REGISTRATION, ListEntry);
        r->NotificationRoutine(r);
    }
}

XBAPI BOOLEAN NTAPI HalIsResetOrShutdownPending() { return HalpShutdownPending != 0; }

static void HalpSmcRequest(UCHAR Register, UCHAR Value);
XBAPI void NTAPI HalInitiateShutdown() { HalpSmcRequest(SMC_RESET, 0x80); } // power off

XBAPI void NTAPI HalWriteSMCScratchRegister(ULONG ScratchRegister) {
    HalpSmcScratch = ScratchRegister;
    HalWriteSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, ScratchRegister);
}

XBAPI void NTAPI HalEnableSecureTrayEject() {
    if (HalpBootFlags & 8) {
        HalpBootFlags &= ~8;
        while (!NT_SUCCESS(HalWriteSMBusValue(SMC_ADDRESS, 0x19, FALSE, 0))) {
        }
    }
}

// Stop bus masters that could write into memory the restarted kernel hands out again.
extern "C" void KiQuickRebootQuiesce() {
    outb(0x21, 0xFF); // mask both PICs
    outb(0xA1, 0xFF);
    outb(0xFF60, 0);  // IDE bus master off
    *(volatile ULONG *)0xFED00008 = 1; // OHCI host controller resets (USB 0 and 1)
    *(volatile ULONG *)0xFED08008 = 1;
    *(volatile ULONG *)0xFD000140 = 0; // NV2A interrupts off
    *(volatile ULONG *)0xFEF00084 = 0; // NIC: tx/rx DMA off
}
extern "C" void KiQuickReboot() __attribute__((noreturn));
extern "C" volatile UCHAR KiQuickRebootFlag;

// Routine 2 (quick reboot) runs the shutdown notifications and re-enters the kernel; every other routine resets at
// once: 4 (fatal error) first leaves scratch 2 for the next boot (spec/12 §2).
XBAPI void NTAPI HalReturnToFirmware(ULONG Routine) {
    if (Routine != 2) {
        if (Routine == 4)
            HalWriteSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, 2);
        if (KeGetCurrentIrql() < DISPATCH_LEVEL)
            HalWriteSMBusValue(SMC_ADDRESS, SMC_RESET, FALSE, 1);
        outb(0xCF9, 0x0E); // full reset through the reset control register
        for (;;)
            ;
    }
    HalpRunShutdownNotifications();
    KiQuickReboot();
}

// --- SMC events (spec/12 §4). The SMC raises EXTSMI# (ACPI GPE0 bit 1) on the SCI, IRQ 12. The reference runs an
// asynchronous SMBus state machine; ours hands the work to a system thread that uses the same register writes.

constexpr UCHAR SMC_AV_PACK = 0x04, SMC_SHUTDOWN_NOTICE = 0x0B, SMC_TRAY_EJECT = 0x0C, SMC_OS_RESPONSE = 0x0D,
                SMC_INTERRUPT_REASON = 0x11, SMC_RESET_ON_EJECT = 0x19;
volatile ULONG HalpSmcEventCount, HalpSmcLastReason, HalpSciCount; // diagnostics (boot tests)
static ULONG HalpBootAvPack = 7;                     // AV pack at cold boot (7 = none)
static volatile BOOLEAN HalpSmcEventsArmed;          // set once the first image is loaded
static volatile BOOLEAN HalpEjectBeforeLaunch;       // eject handled before that: a title launch is refused
static volatile UCHAR HalpRequestRegister, HalpRequestValue;
static KEVENT HalpSmcWork;
static KDPC HalpSmcDpc;
static KINTERRUPT HalpSciInterrupt;
extern ULONG XeConsoleGameRegion;
extern UCHAR XeTitleDiscAuthenticated;
extern ULONG XeDiscTitleId;
void AvpSmcSetOutputs(bool Enable);

static void HalpSmcWrite(UCHAR Register, ULONG Value) {
    while (!NT_SUCCESS(HalWriteSMBusValue(SMC_ADDRESS, Register, FALSE, Value))) {
    }
}

// Cold boot (the reference's 0x80054f2d): AV pack, events pending from before the kernel, the scratch register.
void HalpSmcInitColdBoot() {
    ULONG v;
    if (NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_AV_PACK, FALSE, &v)))
        HalpBootAvPack = v & 7;
    bool eject = false;
    if (NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_INTERRUPT_REASON, FALSE, &v))) {
        eject = v & 0x20;
        if (v & 0x24)
            HalWriteSMBusValue(SMC_ADDRESS, SMC_OS_RESPONSE, FALSE, 4);
    }
    ULONG scratch = 0;
    if (NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, &scratch)) && (scratch & 1))
        eject = true;
    HalWriteSMCScratchRegister(4);
    if (scratch & 4)
        HalpBootFlags |= 0x10;
    if (scratch & 2)
        HalpBootFlags |= 0x20; // the previous boot ended in a fatal error
    if (scratch & 8)
        HalpBootFlags |= 0x80;
    if (eject) {
        HalpBootFlags |= 0x48;
        HalWriteSMBusValue(SMC_ADDRESS, SMC_RESET_ON_EJECT, FALSE, 1);
        HalWriteSMBusValue(SMC_ADDRESS, SMC_TRAY_EJECT, FALSE, 0); // open the tray
        HalpTrayState = 0x20;
    }
}

// Final step of every SMC-driven shutdown or reset (the reference's 0x80014867 chain).
[[noreturn]] static void HalpSmcShutdown(UCHAR Register, UCHAR Value) {
    HalpShutdownPending = 1;
    HalWriteSMBusValue(SMC_ADDRESS, SMC_SHUTDOWN_NOTICE, FALSE, 1);
    outb(0x80D3, 5);
    if ((Register == SMC_OS_RESPONSE && Value == 1) || (Register == SMC_RESET && Value == 0x80)) {
        // Powering off: flush the disk cache and spin it down first, then give it 125 ms.
        outb(0x1F6, 0xA0);
        outb(0x1F7, 0xE7); // FLUSH CACHE
        for (ULONG i = 0; i < 1000000 && (inb(0x3F6) & 0x80); i++)
            KeStallExecutionProcessor(1);
        outb(0x1F7, 0xE0); // STANDBY IMMEDIATE
        KeStallExecutionProcessor(125000);
    }
    HalpSmcWrite(Register, Value);
    for (;;)
        __asm__ volatile("sti; hlt");
}

static void HalpSmcRequest(UCHAR Register, UCHAR Value) {
    HalpShutdownPending = 1;
    HalpRequestRegister = Register;
    HalpRequestValue = Value;
    KeSetEvent(&HalpSmcWork, 0, FALSE);
}

void HalpSmcHandleEvents(ULONG reason) {
    bool manufacturing = XeConsoleGameRegion & 0x80000000;
    if (reason & 0x01) { // power button
        HalpSmcWrite(SMC_OS_RESPONSE, 2);
        HalpSmcShutdown(SMC_OS_RESPONSE, 1);
    }
    if ((reason & 0x10) && !manufacturing) // AV pack removed: outputs off
        AvpSmcSetOutputs(false);
    if ((reason & 0x08) && !manufacturing) { // AV pack inserted: a different kind needs a reset
        ULONG v;
        if (NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_AV_PACK, FALSE, &v)) && (v & 7) != 7) {
            if ((v & 7) != HalpBootAvPack)
                HalpSmcShutdown(SMC_RESET, 1);
            AvpSmcSetOutputs(true);
        }
    }
    if (reason & 0x20) { // eject button
        HalpTrayStateChangeCount = HalpTrayStateChangeCount + 1;
        HalpTrayState = 0x20;
        HalpSmcWrite(SMC_OS_RESPONSE, 4);
        if (!(HalpBootFlags & 8)) {
            if (HalpSmcEventsArmed) { // a title is running: reboot, and open the tray during the next boot
                HalWriteSMCScratchRegister(HalpSmcScratch | 1);
                HalpSmcShutdown(SMC_RESET, 1);
            }
            HalpBootFlags |= 0x48;
            HalpEjectBeforeLaunch = TRUE;
            HalpSmcWrite(SMC_RESET_ON_EJECT, 1);
        }
        HalpSmcWrite(SMC_TRAY_EJECT, 0);
    }
    if (reason & 0x04) { // tray closed (new media)
        HalpTrayState = 0xFFFFFFFF;
        HalpTrayStateChangeCount = HalpTrayStateChangeCount + 1;
        XeTitleDiscAuthenticated = 0;
        XeDiscTitleId = 0;
        if (!(HalpBootFlags & 8) && HalpSmcEventsArmed)
            HalpSmcShutdown(SMC_OS_RESPONSE, 8);
        HalpSmcWrite(SMC_OS_RESPONSE, 4);
        if (!(HalpBootFlags & 8)) {
            HalpBootFlags |= 8;
            HalpEjectBeforeLaunch = TRUE;
            HalpSmcWrite(SMC_RESET_ON_EJECT, 1);
        }
    }
    if (reason & 0x40) { // tray opening
        HalpTrayStateChangeCount = HalpTrayStateChangeCount + 1;
        HalpTrayState = 0;
    }
    if (reason & 0x02) {
        HalpTrayStateChangeCount = HalpTrayStateChangeCount + 1;
        HalpTrayState = 0xFFFFFFFF;
    }
}

static void NTAPI HalpSmcThread(PVOID) {
    for (;;) {
        KeWaitForSingleObject(&HalpSmcWork, Executive, KernelMode, FALSE, nullptr);
        if (HalpRequestRegister)
            HalpSmcShutdown(HalpRequestRegister, HalpRequestValue);
        ULONG reason;
        if (!NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_INTERRUPT_REASON, FALSE, &reason)))
            continue;
        HalpSmcLastReason = reason;
        HalpSmcEventCount = HalpSmcEventCount + 1;
        HalpSmcHandleEvents(reason);
    }
}

static void NTAPI HalpSmcDpcRoutine(PKDPC, PVOID, PVOID, PVOID) { KeSetEvent(&HalpSmcWork, 0, FALSE); }

// SCI: the PM timer overflow (folded into the performance counter) and the SMC's GPE.
XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceCounter();
static BOOLEAN NTAPI HalpSciService(PKINTERRUPT, PVOID) {
    BOOLEAN handled = FALSE;
    HalpSciCount = HalpSciCount + 1;
    if (inw(0x8000) & 1) {
        outw(0x8000, 1);
        KeQueryPerformanceCounter();
        handled = TRUE;
    }
    if (inw(0x8020) & 2) {
        outw(0x8020, 2);
        KeInsertQueueDpc(&HalpSmcDpc, nullptr, nullptr);
        handled = TRUE;
    }
    return handled;
}

XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceCounter();
PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);

void HalpSmcStartEvents() {
    KeInitializeEvent(&HalpSmcWork, SynchronizationEvent, FALSE);
    KeInitializeDpc(&HalpSmcDpc, HalpSmcDpcRoutine, nullptr);
    KiCreateSystemThread(HalpSmcThread, nullptr, 0, 16);
    KIRQL irql;
    ULONG vector = HalGetInterruptVector(12, &irql);
    KeInitializeInterrupt(&HalpSciInterrupt, HalpSciService, nullptr, vector, irql, LevelSensitive, TRUE);
    KeConnectInterrupt(&HalpSciInterrupt);
}

// The XBE loader, after the image is in memory (the reference's 0x80014cc9): from now on an eject resets. The
// dashboard turns the SMC's own reset-on-eject off on cold boot. Returns TRUE when an eject was handled before
// this launch (a disc title must then not start).
BOOLEAN HalpSmcArmEvents(BOOLEAN Dashboard) {
    HalpSmcEventsArmed = TRUE;
    if (!KiQuickRebootFlag && Dashboard && !(HalpBootFlags & 8)) {
        HalpBootFlags |= 8;
        HalWriteSMBusValue(SMC_ADDRESS, SMC_RESET_ON_EJECT, FALSE, 1);
    }
    return HalpEjectBeforeLaunch;
}
