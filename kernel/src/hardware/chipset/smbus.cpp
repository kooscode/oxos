// SMBus host controller (MCPX, I/O 0xC000), SMC services, firmware return (spec/stock/12).
#include <hardware/chipset/chipset.h>
#include <hardware/video/video.h>
#include <arch/arch.h>
#include <diagnostics/diagnostics.h>
#include <core/core.h>
#include <titles/titles.h>
#include <extensions/extensions.h>

namespace xk::hardware::chipset {

// The SMC (SMBus address 0x20) and its registers that more than one section uses.
constexpr UCHAR SMC_ADDRESS = 0x20, SMC_RESET = 0x02, SMC_TRAY_STATE = 0x03, SMC_SCRATCH = 0x1B;

static volatile ULONG CurrentTrayState = 0xFFFFFFFF, TrayStateChanges; // updated by SMC events
static volatile UCHAR ShutdownPending;                                 // a shutdown or reset is under way
static ULONG SmcScratch;                                               // the scratch register as last written
volatile LONG ConsoleStopping;                                         // an extension's launch or reset is under way: the first one wins
// Boot flags (the reference's 0x8003B138, kept across quick reboots), derived from the SMC at cold boot (spec/stock/12 §4): 0x08 SMC "reset on eject" off (0x19
// = 1), 0x10/0x20/0x80 from the scratch register, 0x40 tray ejected at boot.
XK_PERSIST volatile ULONG BootFlags;

// --- SMBus host controller (MCPX, I/O 0xC000)
constexpr USHORT SMB_STATUS = 0xC000, SMB_CONTROL = 0xC002, SMB_ADDRESS = 0xC004, SMB_DATA = 0xC006, SMB_COMMAND = 0xC008, SMB_FIFO = 0xC009;
constexpr UCHAR SMB_STATUS_COLLISION = 0x02, SMB_STATUS_PROTOCOL = 0x04,
                SMB_STATUS_DONE = 0x10, SMB_STATUS_TIMEOUT = 0x20;
constexpr NTSTATUS STATUS_IO_TIMEOUT_ = (NTSTATUS)0xC00000B5, STATUS_IO_DEVICE_ERROR_ = (NTSTATUS)0xC0000185;
constexpr NTSTATUS SMB_TRY_AGAIN = 0x103; // an attempt ended in a collision or protocol error

// One transaction at a time: threads wait for the lock; callers at DISPATCH_LEVEL and above cannot wait, and go ahead.
static KEVENT SMBusLock;
static bool SMBusLockReady;

// Diagnostics.
volatile ULONG SMBusRaisedWhileBusy; // SMBus calls at DISPATCH_LEVEL+ while a thread held the lock
volatile ULONG SMBusLastBlock;       // (host status << 8) | byte count of the last block transfer

static void AcquireSMBus() {
    if (!SMBusLockReady) {
        KeInitializeEvent(&SMBusLock, SynchronizationEvent, TRUE);
        SMBusLockReady = true;
    }
    if (KeGetCurrentIrql() >= DISPATCH_LEVEL && !SMBusLock.Header.SignalState) {
        ULONG n = SMBusRaisedWhileBusy;
        SMBusRaisedWhileBusy = n + 1;
        if (n < 8) {
            XK_SERIAL("smbus: called at IRQL %u from %p while a thread holds the bus\n", KeGetCurrentIrql(), __builtin_return_address(0));
        }
    }
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
        KeWaitForSingleObject(&SMBusLock, Executive, KernelMode, FALSE, nullptr);
}

static void ReleaseSMBus() {
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
        KeSetEvent(&SMBusLock, 0, FALSE);
}

// Transfer size: 1 byte, or with the word flag 2 bytes, or 4 bytes for the 32-bit devices at 0xE0-0xE7 (the 1.6 video encoder; the 5838 kernel's rule,
// spec/stock/20 §1).
static ULONG SMBusSize(UCHAR Address, BOOLEAN WordFlag) {
    return !WordFlag ? 1 : (Address & 0xF8) == 0xE0 ? 4 : 2;
}

// One attempt: SMB_TRY_AGAIN for a collision or protocol error (try again), else the result.
static NTSTATUS SMBusAttempt(UCHAR Address, UCHAR Command, ULONG Size, bool Read, ULONG *Value, UCHAR *Status) {
    outb(SMB_ADDRESS, Read ? (UCHAR)(Address | 1) : (UCHAR)(Address & 0xFE));
    outb(SMB_COMMAND, Command);
    if (!Read && Size == 4) {
        for (int i = 0; i < 4; i++)
            outb(SMB_FIFO, (UCHAR)(*Value >> (8 * i)));
        outb(SMB_DATA, 4);
    } else if (!Read) {
        outw(SMB_DATA, (USHORT)*Value);
    }
    outw(SMB_STATUS, inw(SMB_STATUS)); // clear
    outb(SMB_CONTROL, Size == 4 ? 0x0D : Size == 2 ? 0x0B : 0x0A); // start; block/word/byte (polled)
    UCHAR status = 0;
    for (ULONG us = 0; us < 50000; us += 10) {
        status = inb(SMB_STATUS);
        if (status & (SMB_STATUS_DONE | SMB_STATUS_TIMEOUT | SMB_STATUS_COLLISION | SMB_STATUS_PROTOCOL))
            break;
        KeStallExecutionProcessor(10);
    }
    if (status & SMB_STATUS_DONE) {
        if (Read && Size == 4) {
            SMBusLastBlock = (ULONG)status << 8 | inb(SMB_DATA);
            if ((inb(SMB_DATA) & 0x3F) != 4)
                return STATUS_IO_DEVICE_ERROR_;
            ULONG v = 0;
            for (int i = 0; i < 4; i++)
                v |= (ULONG)inb(SMB_FIFO) << (8 * i);
            *Value = v;
        } else if (Read) {
            *Value = Size == 2 ? inw(SMB_DATA) : inb(SMB_DATA);
        }
        return STATUS_SUCCESS;
    }
    if (Size == 4)
        SMBusLastBlock = (ULONG)status << 8 | 0xFF;
    *Status = status;
    if (!(status & (SMB_STATUS_COLLISION | SMB_STATUS_PROTOCOL)))
        return (status & SMB_STATUS_TIMEOUT) ? STATUS_IO_TIMEOUT_ : STATUS_IO_DEVICE_ERROR_;
    return SMB_TRY_AGAIN;
}

// One byte/word/block transaction, polled. The reference retries collisions/protocol errors up to 20 times, 2 ms apart. A 4-byte transfer uses the block
// protocol: the bytes go through the FIFO (0xC009), low byte first, with the byte count in the data register. Each attempt runs with interrupts disabled:
// callers at DISPATCH_LEVEL and above (DPCs, a title's interrupt routine) cannot take the lock, and one of them preempting a transaction half-way overwrote its
// address, command and data registers (a write could reach the wrong SMC register). An attempt takes well under 1 ms.
static NTSTATUS SMBusTransaction(UCHAR Address, UCHAR Command, ULONG Size, bool Read, ULONG *Value) {
    XK_MARK("mark: SMBus transaction");
    for (int attempt = 0; attempt < 20; attempt++) {
        UCHAR status;
        ULONG flags = save_flags_cli();
        NTSTATUS result = SMBusAttempt(Address, Command, Size, Read, Value, &status);
        restore_flags(flags);
        if (result != SMB_TRY_AGAIN) {
            XK_MARK(NT_SUCCESS(result) ? "mark: SMBus done" : "mark: SMBus failed");
            return result;
        }
        XK_MARK("mark: SMBus retry");
        KeStallExecutionProcessor(2000);
    }
    return STATUS_IO_DEVICE_ERROR_;
}

XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue) {
    AcquireSMBus();
    ULONG v = 0;
    NTSTATUS status = SMBusTransaction(SlaveAddress, CommandCode, SMBusSize(SlaveAddress, ReadWordValue), true, &v);
    ReleaseSMBus();
    *DataValue = v;
    return status;
}

XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue) {
    XK_MARK("mark: SMBus write, taking the lock");
    AcquireSMBus();
    ULONG v = DataValue;
    NTSTATUS status = SMBusTransaction(SlaveAddress, CommandCode, SMBusSize(SlaveAddress, WriteWordValue), false, &v);
    ReleaseSMBus();
    return status;
}

// --- SMC tray state

// Tray state: 0x10 open/closing, 0x40 no media, 0x60 media present (other values read as 0x10).
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount) {
    ULONG state = CurrentTrayState, count = TrayStateChanges;
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

static LIST_ENTRY ShutdownList = {&ShutdownList, &ShutdownList};

XBAPI void NTAPI HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION ShutdownRegistration, BOOLEAN Register) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (!Register) {
        for (PLIST_ENTRY e = ShutdownList.Flink; e != &ShutdownList; e = e->Flink) {
            if (e == &ShutdownRegistration->ListEntry) {
                RemoveEntryList(e);
                break;
            }
        }
    } else {
        PLIST_ENTRY e = ShutdownList.Flink;
        while (e != &ShutdownList &&
               CONTAINING_RECORD(e, HAL_SHUTDOWN_REGISTRATION, ListEntry)->Priority >= ShutdownRegistration->Priority)
            e = e->Flink;
        InsertTailList(e, &ShutdownRegistration->ListEntry); // in front of the first lower priority
    }
    KfLowerIrql(old);
}

static void RunShutdownNotifications() {
    for (;;) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        PLIST_ENTRY e = IsListEmpty(&ShutdownList) ? nullptr : RemoveHeadList(&ShutdownList);
        KfLowerIrql(old);
        if (!e)
            break;
        auto r = CONTAINING_RECORD(e, HAL_SHUTDOWN_REGISTRATION, ListEntry);
        CallTitleRoutine((PVOID)r->NotificationRoutine, r);
    }
}

XBAPI BOOLEAN NTAPI HalIsResetOrShutdownPending() {
    return ShutdownPending != 0;
}

XBAPI void NTAPI HalWriteSMCScratchRegister(ULONG ScratchRegister) {
    SmcScratch = ScratchRegister;
    HalWriteSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, ScratchRegister);
}

XBAPI void NTAPI HalEnableSecureTrayEject() {
    if (BootFlags & 8) {
        BootFlags &= ~8;
        while (!NT_SUCCESS(HalWriteSMBusValue(SMC_ADDRESS, 0x19, FALSE, 0))) {
            // Retry until the SMC accepts the write.
        }
    }
}

// Boot status on the front LED (spec/oxos_internal/03_debug_serial.md §6): an SMC LED sequence (register 0x08: bits 7-4 red, 3-0 green, one bit per quarter of
// the cycle) in custom mode (0x07 = 1).
void SetBootLed(UCHAR Pattern) {
    HalWriteSMBusValue(SMC_ADDRESS, 0x08, FALSE, Pattern);
    HalWriteSMBusValue(SMC_ADDRESS, 0x07, FALSE, 1);
}

// --- Firmware return: quick reboot and reset (spec/stock/12 §2)
// Stop bus masters that could write into memory the restarted kernel hands out again.
extern "C" void KiQuickRebootQuiesce() {
    outb(0x21, 0xFF); // mask both PICs
    outb(0xA1, 0xFF);
    outb(0xFF60, 0);  // IDE bus master off
    // OHCI host controller resets (USB 0 and 1). A controller leaves its reset in USBSUSPEND, so put it back in USBRESET (HcControl 0), the state a title finds
    // after a cold boot: a title's USB library that finds the controller suspended resumes it with a 20 ms sleep inside XInitDevices, and a title whose other
    // thread asks for its input devices meanwhile initializes them twice (spec/oxos_internal/25 §5).
    static const ULONG OHCI[] = {0xFED00000, 0xFED08000};
    for (ULONG base : OHCI) {
        auto hc = (volatile ULONG *)base;
        hc[2] = 1; // HcCommandStatus: HostControllerReset (done within 10 us)
        for (int i = 0; i < 10000 && (hc[2] & 1); i++) {
            // Wait for the reset to finish (within 10 us; bounded).
        }
        hc[1] = 0; // HcControl: USBRESET, every list off
    }
    // AC'97: both output channels stopped. The codec's cold reset is left alone, as the stock kernel leaves it: asserting it on a codec that had been running
    // froze the codec until the power was removed, about one quick reboot in 13 (spec/oxos_internal/46 §2).
    *(volatile UCHAR *)0xFEC0011B = 0;  // PCM out: stopped
    *(volatile UCHAR *)0xFEC0017B = 0;  // S/PDIF out: stopped
    *(volatile ULONG *)0xFD000140 = 0; // NV2A interrupts off
    // NIC: transmitter and receiver off (spec/oxos_internal/25 §7). A receiver left on writes every frame the network sends into the receive buffers of the
    // title that just ended, memory the next title is given. Resetting the whole NIC here (TxRxControl, rings: tried 2026-10-29) left it in a state the next
    // title's network start-up waited on forever (XBMC4Gamers started by EvoX's dashboard).
    *(volatile ULONG *)0xFEF00084 = 0; // NvTxControl
    *(volatile ULONG *)0xFEF00094 = 0; // NvRxControl
    for (int i = 0; i < 100000 && (*(volatile ULONG *)0xFEF00098 & 1); i++) {
        // Wait for a frame being received to finish (NvRxStatus busy; bounded).
    }
}

// A power cycle through the SMC (register 0x02 = 0x40, the SMC's power-cycle command, xboxdevwiki "PIC"), so the next boot is a cold one: the in-game
// reset's reboot (spec/oxos_internal/22 §6), now the kernel's last resort and oxos_console_reset(OXOS_RESET_COLD). A reset (0x02 = 1, then port 0xCF9) while a
// title is using the GPU left most of the X-codes' GPU and memory set-up undone at the next boot (spec/oxos_internal/22 §5). The SMBus is polled, so this works
// at any IRQL. If the SMC does not cut the power within two seconds, the reference reset follows.
void PowerCycle() {
    for (int i = 0; i < 5 && !NT_SUCCESS(HalWriteSMBusValue(SMC_ADDRESS, 0x02, FALSE, 0x40)); i++) {
        // Retry until the SMC accepts the power cycle (up to five tries).
    }
    for (ULONG ms = 0; ms < 2000; ms++)
        KeStallExecutionProcessor(1000);
    HalReturnToFirmware(1);
}

// oxos_console_reset (spec/oxos_internal/34 §7.3): 1 powers the console off and on (PowerCycle), 2 resets it through the SMC. Returns only on failure.
NTSTATUS ResetConsole(ULONG Kind) {
    if (Kind != 1 && Kind != 2)
        return (NTSTATUS)0xC000000D; // STATUS_INVALID_PARAMETER
    if (InterlockedExchange(&ConsoleStopping, 1))
        return (NTSTATUS)0x80000011; // STATUS_DEVICE_BUSY
    XK_SERIAL("reset: an extension asks for a %s\n", Kind == 1 ? "power cycle" : "reset");
    if (Kind == 1)
        PowerCycle();
    HalReturnToFirmware(1);
    return (NTSTATUS)0xC0000001; // STATUS_UNSUCCESSFUL: not reached
}

// Routine 2 (quick reboot) runs the shutdown notifications and re-enters the kernel; every other routine resets at once: 4 (fatal error) first leaves scratch 2
// for the next boot (spec/stock/12 §2).
XBAPI void NTAPI HalReturnToFirmware(ULONG Routine) {
    XK_SERIAL("firmware: HalReturnToFirmware(%lu) from %p\n", Routine, __builtin_return_address(0));
#if XK_DEBUG_SERIAL
    {
        auto ldp = (const ULONG *)LaunchDataPage;
        if (ldp) {
            XK_SERIAL("firmware: launch data type %#lx title %08lx path '%.40s' data %lx %lx %lx\n", ldp[0], ldp[1], (const char *)(ldp + 2), ldp[0x100],
                      ldp[0x101], ldp[0x102]);
        }
    }
#endif
    if (Routine != 2) {
        if (Routine == 4)
            HalWriteSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, 2);
        // OXOS: the SMC's reset alone, and port 0xCF9 only if the SMC has not reset the console within two seconds. The reference writes 0xCF9 right after the
        // SMC request: the chipset then restarts and runs the X-codes at once, and the SMC's reset, which arrives later over the SMBus, can undo their GPU and
        // memory set-up (1 warm reboot in 10 on the 1.4 test console: every X-code setting at its power-on value, the next boot stopped; spec/oxos_internal/22
        // §7). The SMBus is polled, so the request works at any IRQL.
        for (int i = 0; i < 5 && !NT_SUCCESS(HalWriteSMBusValue(SMC_ADDRESS, SMC_RESET, FALSE, 1)); i++) {
            // Retry until the SMC accepts the reset (up to five tries).
        }
        for (ULONG ms = 0; ms < 2000; ms++)
            KeStallExecutionProcessor(1000);
        outb(0xCF9, 0x0E); // full reset through the reset control register
        for (;;) {
            // Wait for the reset to take the console down.
        }
    }
    // The kernel extensions' exit event, their threads' end and Unload (spec/oxos_internal/34 §4.3).
    extensions::TitleExiting();
    RunShutdownNotifications();
    KiQuickReboot();
}

// --- SMC events (spec/stock/12 §4). The SMC raises EXTSMI# (ACPI GPE0 bit 1) on the SCI, IRQ 12. The reference runs an
// asynchronous SMBus state machine; ours runs the same register writes in a DPC (SmcDpcRoutine).

constexpr UCHAR SMC_AV_PACK = 0x04, SMC_SHUTDOWN_NOTICE = 0x0B, SMC_TRAY_EJECT = 0x0C, SMC_OS_RESPONSE = 0x0D, SMC_INTERRUPT_REASON = 0x11,
                SMC_RESET_ON_EJECT = 0x19;
volatile ULONG SmcEventCount, SmcLastReason, SciCount; // diagnostics (boot tests)
// These two survive quick reboots, as in the reference (its STICKY section, 0x8003AFEC and 0x8003AFE8): a title started from the dashboard still knows the
// cold-boot AV pack (re-plugging the same cable must not reset) and counts as a running title for eject and tray events even before its image is loaded.
XK_PERSIST static ULONG BootAvPack = 7;            // AV pack at cold boot (7 = none)
XK_PERSIST static volatile BOOLEAN SmcEventsArmed; // set once the first image is loaded; never cleared

// An eject handled before the first image is loaded: a title launch is refused.
static volatile BOOLEAN EjectBeforeLaunch;

// The SMC DPC, queued by the SCI interrupt for SMC events, or with a shutdown or reset write (register, value) to make.
static volatile UCHAR RequestRegister, RequestValue;
static KDPC SmcDpc;
static KTIMER SmcRetryTimer; // the SMBus was in use when the DPC ran
static KINTERRUPT SciInterrupt;

static void WriteSmc(UCHAR Register, ULONG Value) {
    while (!NT_SUCCESS(HalWriteSMBusValue(SMC_ADDRESS, Register, FALSE, Value))) {
        // Retry until the SMC accepts the write.
    }
}

// Cold boot (the reference's 0x80054f2d): AV pack, events pending from before the kernel, the scratch register.
void InitSmcColdBoot() {
    ULONG v;
    if (NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_AV_PACK, FALSE, &v)))
        BootAvPack = v & 7;
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
        BootFlags |= 0x10;
    if (scratch & 2)
        BootFlags |= 0x20; // the previous boot ended in a fatal error
    if (scratch & 8)
        BootFlags |= 0x80;
    if (eject) {
        BootFlags |= 0x48;
        HalWriteSMBusValue(SMC_ADDRESS, SMC_RESET_ON_EJECT, FALSE, 1);
        HalWriteSMBusValue(SMC_ADDRESS, SMC_TRAY_EJECT, FALSE, 0); // open the tray
        CurrentTrayState = 0x20;
    }
}

// Final step of every SMC-driven shutdown or reset (the reference's 0x80014867 chain).
[[noreturn]] static void SmcShutdown(UCHAR Register, UCHAR Value) {
    ShutdownPending = 1;
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
    WriteSmc(Register, Value);
    for (;;)
        __asm__ volatile("sti; hlt");
}

static void RequestSmc(UCHAR Register, UCHAR Value) {
    ShutdownPending = 1;
    RequestRegister = Register;
    RequestValue = Value;
    KeInsertQueueDpc(&SmcDpc, nullptr, nullptr);
}

XBAPI void NTAPI HalInitiateShutdown() {
    RequestSmc(SMC_RESET, 0x80); // power off
}

void HandleSmcEvents(ULONG reason) {
    bool manufacturing = titles::ConsoleGameRegion & 0x80000000;
    if (reason & 0x01) { // power button
        WriteSmc(SMC_OS_RESPONSE, 2);
        SmcShutdown(SMC_OS_RESPONSE, 1);
    }
    if ((reason & 0x10) && !manufacturing) // AV pack removed: outputs off
        video::SmcSetOutputs(false);
    if ((reason & 0x08) && !manufacturing) { // AV pack inserted: a different kind needs a reset
        ULONG v;
        if (NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_AV_PACK, FALSE, &v)) && (v & 7) != 7) {
            if ((v & 7) != BootAvPack)
                SmcShutdown(SMC_RESET, 1);
            video::SmcSetOutputs(true);
        }
    }
    if (reason & 0x20) { // eject button
        TrayStateChanges = TrayStateChanges + 1;
        CurrentTrayState = 0x20;
        WriteSmc(SMC_OS_RESPONSE, 4);
        if (!(BootFlags & 8)) {
            if (SmcEventsArmed) { // a title is running: reboot, and open the tray during the next boot
                HalWriteSMCScratchRegister(SmcScratch | 1);
                SmcShutdown(SMC_RESET, 1);
            }
            BootFlags |= 0x48;
            EjectBeforeLaunch = TRUE;
            WriteSmc(SMC_RESET_ON_EJECT, 1);
        }
        WriteSmc(SMC_TRAY_EJECT, 0);
    }
    if (reason & 0x04) { // tray closed (new media)
        CurrentTrayState = 0xFFFFFFFF;
        TrayStateChanges = TrayStateChanges + 1;
        titles::TitleDiscAuthenticated = 0;
        titles::DiscTitleId = 0;
        if (!(BootFlags & 8) && SmcEventsArmed)
            SmcShutdown(SMC_OS_RESPONSE, 8);
        WriteSmc(SMC_OS_RESPONSE, 4);
        if (!(BootFlags & 8)) {
            BootFlags |= 8;
            EjectBeforeLaunch = TRUE;
            WriteSmc(SMC_RESET_ON_EJECT, 1);
        }
    }
    if (reason & 0x40) { // tray opening
        TrayStateChanges = TrayStateChanges + 1;
        CurrentTrayState = 0;
    }
    if (reason & 0x02) { // tray state changed: read it from the SMC again
        TrayStateChanges = TrayStateChanges + 1;
        CurrentTrayState = 0xFFFFFFFF;
    }
}

// SMC events and shutdown requests run in a DPC, as the reference's (no thread: its stack would cost the title 3 pages, spec/stock/09 §7). The SMBus is polled,
// so it works at DISPATCH_LEVEL; when a passive-level caller is in the middle of a transaction (its lock is taken), the DPC tries again 1 ms later.
static void NTAPI SmcDpcRoutine(PKDPC, PVOID, PVOID, PVOID) {
    AcquireSMBus(); // initialises the lock on first use; no wait at DISPATCH_LEVEL
    if (!SMBusLock.Header.SignalState) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;
        KeSetTimer(&SmcRetryTimer, due, &SmcDpc);
        return;
    }
    KeResetEvent(&SMBusLock);
    if (RequestRegister)
        SmcShutdown(RequestRegister, RequestValue);
    ULONG reason;
    if (NT_SUCCESS(HalReadSMBusValue(SMC_ADDRESS, SMC_INTERRUPT_REASON, FALSE, &reason))) {
        SmcLastReason = reason;
        SmcEventCount = SmcEventCount + 1;
        XK_SERIAL("smc: event %#lx\n", reason);
        HandleSmcEvents(reason);
    }
    KeSetEvent(&SMBusLock, 0, FALSE);
}

// SCI: the PM timer overflow (folded into the performance counter) and the SMC's GPE.
static BOOLEAN NTAPI SciService(PKINTERRUPT, PVOID) {
    BOOLEAN handled = FALSE;
    SciCount = SciCount + 1;
    if (inw(0x8000) & 1) {
        outw(0x8000, 1);
        core::PerfCounterOverflow();
        handled = TRUE;
    }
    if (inw(0x8020) & 2) {
        outw(0x8020, 2);
        KeInsertQueueDpc(&SmcDpc, nullptr, nullptr);
        handled = TRUE;
    }
    return handled;
}

void StartSmcEvents() {
    KeInitializeDpc(&SmcDpc, SmcDpcRoutine, nullptr);
    KeInitializeTimerEx(&SmcRetryTimer, NotificationTimer);
    KIRQL irql;
    ULONG vector = HalGetInterruptVector(12, &irql);
    KeInitializeInterrupt(&SciInterrupt, SciService, nullptr, vector, irql, LevelSensitive, TRUE);
    KeConnectInterrupt(&SciInterrupt);
}

// The XBE loader, after the image is in memory (the reference's 0x80014cc9): from now on an eject resets. The dashboard turns the SMC's own reset-on-eject off
// on cold boot. Returns TRUE when an eject was handled before this launch (a disc title must then not start).
BOOLEAN ArmSmcEvents(BOOLEAN Dashboard) {
    SmcEventsArmed = TRUE;
    if (!KiQuickRebootFlag && Dashboard && !(BootFlags & 8)) {
        BootFlags |= 8;
        HalWriteSMBusValue(SMC_ADDRESS, SMC_RESET_ON_EJECT, FALSE, 1);
    }
    return EjectBeforeLaunch;
}

} // namespace xk::hardware::chipset
