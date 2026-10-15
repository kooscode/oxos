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
static ULONG HalpTrayState = 0xFFFFFFFF, HalpTrayStateChangeCount;

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
volatile ULONG HalpResetOnEjectEnabled; // boot-flags bit 3 of the reference

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

XBAPI void NTAPI HalReturnToFirmware(ULONG Routine);
XBAPI void NTAPI HalInitiateShutdown() {
    HalpShutdownPending = 2;
    HalReturnToFirmware(0);
}

XBAPI void NTAPI HalWriteSMCScratchRegister(ULONG ScratchRegister) {
    HalpSmcScratch = ScratchRegister;
    HalWriteSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, ScratchRegister);
}

XBAPI void NTAPI HalEnableSecureTrayEject() {
    if (HalpResetOnEjectEnabled) {
        HalpResetOnEjectEnabled = 0;
        while (!NT_SUCCESS(HalWriteSMBusValue(SMC_ADDRESS, 0x19, FALSE, 0))) {
        }
    }
}

// Routines: 0 halt, 1 reboot, 2 quick reboot, 3 debugger reboot, 4 fatal-error reboot.
XBAPI void NTAPI HalReturnToFirmware(ULONG Routine) {
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
        HalpRunShutdownNotifications();
    if (Routine == 4)
        HalWriteSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, 2); // the dashboard shows the error on next boot
    // Quick reboot (re-entering the kernel without the BIOS) is not supported yet: reset instead.
    HalWriteSMBusValue(SMC_ADDRESS, SMC_RESET, FALSE, Routine == 0 ? 0x80 : 0x01);
    for (;;) {
        cli();
        halt();
    }
}
