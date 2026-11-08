// The first OHCI controller, driven by polling for the boot's controller read (spec/oxos_internal/32 §3): its
// registers (OpenHCI 1.0a), the root hub's ports, and the endpoint and transfer descriptors the controller follows,
// all in one uncached page. Control transfers wait for their end; interrupt-IN pipes are read without waiting.
// Boot-only.
#include <xk/usb_host.h>
#include <xk/core.h>
#include <xk/memory.h>
#include <xk/runtime.h>
#include <xk/serial.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

// OHCI registers (32-bit words; the controller at the address the kernel's PCI set-up gives USB 0).
constexpr ULONG OHCI_BASE = 0xFED00000;
constexpr ULONG HC_REVISION = 0x00, HC_CONTROL = 0x04, HC_COMMAND_STATUS = 0x08, HC_INTERRUPT_STATUS = 0x0C,
                HC_INTERRUPT_DISABLE = 0x14, HC_HCCA = 0x18, HC_CONTROL_HEAD_ED = 0x20, HC_BULK_HEAD_ED = 0x28,
                HC_FM_INTERVAL = 0x34, HC_PERIODIC_START = 0x40, HC_LS_THRESHOLD = 0x44, HC_RH_DESCRIPTOR_A = 0x48,
                HC_RH_STATUS = 0x50, HC_RH_PORT_STATUS = 0x54;
// HcControl: control-bulk ratio 4:1, the periodic and control lists on, the controller running (USBOPERATIONAL).
constexpr ULONG HC_CONTROL_RUN = 0x03 | 0x04 | 0x10 | 0x80;
constexpr ULONG HC_RESET = 0x01, HC_CONTROL_LIST_FILLED = 0x02; // HcCommandStatus
constexpr ULONG RH_SET_GLOBAL_POWER = 0x10000;                  // HcRhStatus
// HcRhPortStatus: the state bits, and what a write does.
constexpr ULONG PORT_CONNECTED = 0x1, PORT_ENABLED = 0x2, PORT_LOW_SPEED = 0x200, PORT_RESET_DONE = 0x100000;
constexpr ULONG PORT_SET_RESET = 0x10, PORT_SET_POWER = 0x100, PORT_CLEAR_CHANGES = 0x1F0000;

// An endpoint descriptor (ED) and a general transfer descriptor (TD): 16 bytes each, 16-byte aligned.
struct OHCI_ED {
    ULONG Info;  // function address, endpoint, direction, speed, skip, maximum packet size
    ULONG TailP; // the TD after the last one queued (never processed)
    ULONG HeadP; // the next TD; bit 0 halted, bit 1 the data toggle carried between TDs
    ULONG NextED;
};
struct OHCI_TD {
    ULONG Info;   // rounding, direction, delay interrupt, data toggle, error count, condition code
    ULONG Cbp;    // the next byte to move (0 once all moved)
    ULONG NextTD;
    ULONG Be;     // the buffer's last byte
};
static_assert(sizeof(OHCI_ED) == 16 && sizeof(OHCI_TD) == 16);

constexpr ULONG ED_IN = 2 << 11, ED_LOW_SPEED = 1 << 13, ED_SKIP = 1 << 14;
constexpr ULONG ED_HALTED = 1;
// TD Info: buffer rounding (a short packet ends the TD without an error), the direction (SETUP / OUT / IN), no
// interrupt, the data toggle (from the TD: DATA0, DATA1; or 0: carried in the ED), condition code "not accessed".
constexpr ULONG TD_ROUNDING = 1 << 18, TD_SETUP = 0 << 19, TD_OUT = 1 << 19, TD_IN = 2 << 19, TD_NO_INTERRUPT = 7 << 21,
                TD_DATA0 = 2 << 24, TD_DATA1 = 3 << 24, TD_NOT_ACCESSED = 0xFu << 28;

// The page: the HCCA (its 32 interrupt-table entries first), the EDs, the TDs and the buffers.
struct USB_HOST_PAGE {
    ULONG InterruptTable[32];
    UCHAR HccaRest[256 - 128];
    OHCI_ED Control;
    OHCI_ED Pipes[USB_HOST_PIPES];
    UCHAR Pad0[256 - 16 * (1 + USB_HOST_PIPES)];
    OHCI_TD ControlTds[4]; // SETUP, data, status, and the tail
    OHCI_TD PipeTds[USB_HOST_PIPES][2]; // the one queued, and the tail
    UCHAR Setup[8];
    UCHAR Pad1[8];
    UCHAR ControlData[256];
    UCHAR PipeData[USB_HOST_PIPES][2][64];
};
static_assert(sizeof(USB_HOST_PAGE) <= PAGE_SIZE, "one page: no buffer crosses a page boundary");

static USB_HOST_PAGE *UsbpPage;  // null while the controller is stopped
static ULONG UsbpPorts;          // the root hub's ports
static ULONG UsbpPipeQueued[USB_HOST_PIPES]; // per pipe: which of its two TDs is queued (the other is the tail)
static USHORT UsbpPipeSize[USB_HOST_PIPES];  // per pipe: its packet size (0: not open)

static volatile ULONG &UsbpRegister(ULONG Offset) {
    return *(volatile ULONG *)(OHCI_BASE + Offset);
}

static ULONG UsbpPhysical(const void *p) {
    return MmGetPhysicalAddress((PVOID)p);
}

static void UsbpSleep(ULONG Ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

// --- The controller and its root hub

bool UsbHostStart() {
    if ((UsbpRegister(HC_REVISION) & 0xFF) != 0x10) // OHCI 1.0
        return false;
    UsbpPage = (USB_HOST_PAGE *)MmAllocateContiguousMemoryEx(PAGE_SIZE, 0, 0x3FFFFFF, PAGE_SIZE,
                                                             PAGE_READWRITE | PAGE_NOCACHE);
    if (!UsbpPage)
        return false;
    RtlZeroMemory(UsbpPage, PAGE_SIZE);
    for (ULONG i = 0; i < USB_HOST_PIPES; i++)
        UsbpPipeSize[i] = 0;
    ULONG interval = UsbpRegister(HC_FM_INTERVAL) & 0x3FFF;
    if (!interval)
        interval = 0x2EDF; // 12000 bit times, less one: a 1 ms frame
    UsbpRegister(HC_COMMAND_STATUS) = HC_RESET;
    for (ULONG us = 0; us < 1000 && (UsbpRegister(HC_COMMAND_STATUS) & HC_RESET); us += 10)
        KeStallExecutionProcessor(10);
    // In USBSUSPEND after the reset: running again within 2 ms, as the specification asks.
    UsbpRegister(HC_HCCA) = UsbpPhysical(UsbpPage);
    UsbpPage->Control.Info = ED_SKIP;
    UsbpRegister(HC_CONTROL_HEAD_ED) = UsbpPhysical(&UsbpPage->Control);
    UsbpRegister(HC_BULK_HEAD_ED) = 0;
    UsbpRegister(HC_INTERRUPT_DISABLE) = 0xC000007F; // polled: no interrupts
    UsbpRegister(HC_INTERRUPT_STATUS) = 0x7F;
    ULONG largest = (interval - 210) * 6 / 7; // the largest packet a frame takes, as the specification computes it
    UsbpRegister(HC_FM_INTERVAL) = ((UsbpRegister(HC_FM_INTERVAL) & 0x80000000) ^ 0x80000000) | largest << 16 | interval;
    UsbpRegister(HC_PERIODIC_START) = interval * 9 / 10;
    UsbpRegister(HC_LS_THRESHOLD) = 0x628;
    UsbpRegister(HC_CONTROL) = HC_CONTROL_RUN;
    UsbpPorts = UsbpRegister(HC_RH_DESCRIPTOR_A) & 0xFF;
    if (UsbpPorts > 15)
        UsbpPorts = 15;
    UsbpRegister(HC_RH_STATUS) = RH_SET_GLOBAL_POWER;
    for (ULONG p = 0; p < UsbpPorts; p++)
        UsbpRegister(HC_RH_PORT_STATUS + 4 * p) = PORT_SET_POWER;
    // Power good (the root hub's own time, 2 ms units), then 100 ms for a device to settle after it is connected.
    UsbpSleep((UsbpRegister(HC_RH_DESCRIPTOR_A) >> 24) * 2 + 100);
    XK_SERIAL("usb: controller running, %lu ports\n", UsbpPorts);
    return true;
}

void UsbHostStop() {
    if (!UsbpPage)
        return;
    // A reset stops every list (no more DMA into the page) and leaves USBSUSPEND; USBRESET resets the bus as at
    // power-on, the state a title's USB driver expects (as hardware/chipset.cpp leaves it).
    UsbpRegister(HC_COMMAND_STATUS) = HC_RESET;
    for (ULONG us = 0; us < 1000 && (UsbpRegister(HC_COMMAND_STATUS) & HC_RESET); us += 10)
        KeStallExecutionProcessor(10);
    UsbpRegister(HC_CONTROL) = 0;
    UsbpRegister(HC_HCCA) = 0;
    UsbpRegister(HC_CONTROL_HEAD_ED) = 0;
    MmFreeContiguousMemory(UsbpPage);
    UsbpPage = nullptr;
}

ULONG UsbHostPorts() {
    return UsbpPorts;
}

bool UsbHostResetPort(ULONG Port, bool *LowSpeed) {
    volatile ULONG &port = UsbpRegister(HC_RH_PORT_STATUS + 4 * Port);
    if (!(port & PORT_CONNECTED))
        return false;
    port = PORT_SET_RESET;
    for (ULONG ms = 0; ms < 100 && !(port & PORT_RESET_DONE); ms += 5)
        UsbpSleep(5);
    port = PORT_CLEAR_CHANGES;
    UsbpSleep(10); // reset recovery
    *LowSpeed = port & PORT_LOW_SPEED;
    return port & PORT_ENABLED;
}

// --- Control transfers

// The condition code a TD ended with; 0xF while the controller has not finished it.
static ULONG UsbpCondition(const OHCI_TD &Td) {
    return Td.Info >> 28;
}

static void UsbpFillTd(OHCI_TD &Td, ULONG Info, const void *Buffer, ULONG Length, const OHCI_TD *Next) {
    Td.Info = Info | TD_NO_INTERRUPT | TD_NOT_ACCESSED;
    Td.Cbp = Length ? UsbpPhysical(Buffer) : 0;
    Td.Be = Length ? Td.Cbp + Length - 1 : 0;
    Td.NextTD = Next ? UsbpPhysical(Next) : 0;
}

LONG UsbHostControl(const USB_DEVICE &Device, UCHAR RequestType, UCHAR Request, USHORT Value, USHORT Index,
                    USHORT Length, PVOID Data) {
    if (!UsbpPage || Length > sizeof(UsbpPage->ControlData))
        return -1;
    USB_HOST_PAGE &page = *UsbpPage;
    bool in = RequestType & 0x80;
    UCHAR setup[8] = {RequestType, Request, (UCHAR)Value, (UCHAR)(Value >> 8), (UCHAR)Index, (UCHAR)(Index >> 8),
                      (UCHAR)Length, (UCHAR)(Length >> 8)};
    RtlCopyMemory(page.Setup, setup, sizeof(setup));
    if (!in && Length)
        RtlCopyMemory(page.ControlData, Data, Length);
    OHCI_TD *setupTd = &page.ControlTds[0], *dataTd = &page.ControlTds[1], *statusTd = &page.ControlTds[2],
            *tail = &page.ControlTds[3];
    UsbpFillTd(*setupTd, TD_SETUP | TD_DATA0, page.Setup, 8, Length ? dataTd : statusTd);
    if (Length)
        UsbpFillTd(*dataTd, (in ? TD_IN | TD_ROUNDING : TD_OUT) | TD_DATA1, page.ControlData, Length, statusTd);
    UsbpFillTd(*statusTd, (in ? TD_OUT : TD_IN) | TD_DATA1, nullptr, 0, tail);
    RtlZeroMemory(tail, sizeof(*tail));
    page.Control.Info = Device.Address | (Device.LowSpeed ? ED_LOW_SPEED : 0) | (ULONG)Device.MaxPacket0 << 16;
    page.Control.TailP = UsbpPhysical(tail);
    page.Control.HeadP = UsbpPhysical(setupTd);
    UsbpRegister(HC_COMMAND_STATUS) = HC_CONTROL_LIST_FILLED;
    ULONGLONG end = KeQueryInterruptTime() + 100 * 10000;
    while (UsbpCondition(*statusTd) == 0xF && !(page.Control.HeadP & ED_HALTED) && KeQueryInterruptTime() < end)
        KeStallExecutionProcessor(20);
    UsbpRegister(HC_INTERRUPT_STATUS) = 0x7F; // the done queue's write-back, and the frame bits
    bool done = UsbpCondition(*statusTd) == 0;
    // Idle again: skipped while its queue is emptied (after an error or a timeout the TDs are still on it).
    page.Control.Info |= ED_SKIP;
    UsbpSleep(1);
    page.Control.HeadP = page.Control.TailP;
    if (!done)
        return -1;
    ULONG moved = !Length || !dataTd->Cbp ? Length : dataTd->Cbp - UsbpPhysical(page.ControlData);
    if (in && moved)
        RtlCopyMemory(Data, page.ControlData, moved);
    return (LONG)moved;
}

// --- Interrupt-IN pipes, polled by the controller every frame

// Queues pipe Pipe's next read on TD Which; the other TD becomes the tail.
static void UsbpQueueRead(ULONG Pipe, ULONG Which) {
    USB_HOST_PAGE &page = *UsbpPage;
    OHCI_TD &td = page.PipeTds[Pipe][Which], &tail = page.PipeTds[Pipe][Which ^ 1];
    RtlZeroMemory(&tail, sizeof(tail));
    UsbpFillTd(td, TD_ROUNDING, page.PipeData[Pipe][Which], UsbpPipeSize[Pipe], &tail);
    UsbpPipeQueued[Pipe] = Which;
    page.Pipes[Pipe].TailP = UsbpPhysical(&tail);
}

bool UsbHostOpenPipe(ULONG Pipe, const USB_DEVICE &Device, UCHAR Endpoint, USHORT MaxPacket) {
    if (!UsbpPage || Pipe >= USB_HOST_PIPES || UsbpPipeSize[Pipe])
        return false;
    USB_HOST_PAGE &page = *UsbpPage;
    UsbpPipeSize[Pipe] = MaxPacket < 64 ? MaxPacket : 64;
    OHCI_ED &ed = page.Pipes[Pipe];
    ed.Info = Device.Address | (ULONG)(Endpoint & 0xF) << 7 | ED_IN | (Device.LowSpeed ? ED_LOW_SPEED : 0) |
              (ULONG)UsbpPipeSize[Pipe] << 16;
    ed.NextED = 0;
    UsbpQueueRead(Pipe, 0);
    ed.HeadP = UsbpPhysical(&page.PipeTds[Pipe][0]); // toggle carry 0: DATA0 first, as after SET_CONFIGURATION
    // Linked into the periodic list last, once complete: the first pipe from every interrupt-table entry, the others
    // after the one before.
    ULONG physical = UsbpPhysical(&ed);
    if (Pipe == 0) {
        for (ULONG &entry : page.InterruptTable)
            entry = physical;
    } else {
        page.Pipes[Pipe - 1].NextED = physical;
    }
    return true;
}

LONG UsbHostReadPipe(ULONG Pipe, PVOID Data, ULONG Size) {
    if (!UsbpPage || Pipe >= USB_HOST_PIPES || !UsbpPipeSize[Pipe])
        return -1;
    USB_HOST_PAGE &page = *UsbpPage;
    ULONG which = UsbpPipeQueued[Pipe];
    OHCI_TD &td = page.PipeTds[Pipe][which];
    if (page.Pipes[Pipe].HeadP & ED_HALTED)
        return -1;
    if (UsbpCondition(td) == 0xF)
        return 0;
    UsbpRegister(HC_INTERRUPT_STATUS) = 0x7F;
    if (UsbpCondition(td) != 0)
        return -1;
    const UCHAR *buffer = page.PipeData[Pipe][which];
    ULONG length = td.Cbp ? td.Cbp - UsbpPhysical(buffer) : UsbpPipeSize[Pipe];
    if (length > Size)
        length = Size;
    RtlCopyMemory(Data, buffer, length);
    UsbpQueueRead(Pipe, which ^ 1);
    return (LONG)length;
}
