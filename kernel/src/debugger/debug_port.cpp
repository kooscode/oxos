// The kernel's debug port (debug_port.h, spec/oxos_internal/51 §2): attaching a debugger, registering a debug transport, and what a debugger needs to know
// about threads and loaded images. OXOS's own export oxos_debug_port (oxos_sdk.h §9) hands the table to extensions (oxos_gdb.xos). Resident: the hooks call
// the debugger while titles run.
#include <debugger/debug_port.h>
#include <arch/arch.h>
#include <memory/memory.h>
#include <extensions/extension_state.h>
#include <kernel_modules/kernel_modules.h>

namespace xk::debugger {

// The debugger gets the kernel's trap frame as it is.
static_assert(sizeof(OXOS_TRAP_FRAME) == sizeof(core::KTRAP_FRAME_XK));
static_assert(__builtin_offsetof(OXOS_TRAP_FRAME, Eip) == __builtin_offsetof(core::KTRAP_FRAME_XK, Eip));

constexpr NTSTATUS STATUS_DEVICE_BUSY_ = (NTSTATUS)0x80000011; // another debugger or transport is there

const OXOS_DEBUGGER *volatile AttachedDebugger;              // the attached debugger, or null
static bool KernelDebugger;                                 // AttachedDebugger is a GDB=1 kernel's own (it stays)
static const OXOS_DEBUG_TRANSPORT *RegisteredTransport;     // the transport an extension registered, or null

// --- Attaching

static NTSTATUS AttachDebugger(const OXOS_DEBUGGER *Debugger) {
    if (AttachedDebugger)
        return STATUS_DEVICE_BUSY_;
    AttachedDebugger = Debugger;
    return STATUS_SUCCESS;
}

static void DetachDebugger(const OXOS_DEBUGGER *Debugger) {
    if (AttachedDebugger == Debugger && !KernelDebugger)
        AttachedDebugger = nullptr;
}

static NTSTATUS RegisterTransport(const OXOS_DEBUG_TRANSPORT *Transport) {
    if (RegisteredTransport)
        return STATUS_DEVICE_BUSY_;
    RegisteredTransport = Transport;
    return STATUS_SUCCESS;
}

static void UnregisterTransport(const OXOS_DEBUG_TRANSPORT *Transport) {
    if (RegisteredTransport == Transport)
        RegisteredTransport = nullptr;
}

static const OXOS_DEBUG_TRANSPORT *GetTransport() {
    return RegisteredTransport;
}

void AttachKernelDebugger(const OXOS_DEBUGGER *Debugger) {
    AttachedDebugger = Debugger;
    KernelDebugger = true;
}

void ForgetExtensionDebugger() {
    if (!KernelDebugger)
        AttachedDebugger = nullptr;
    RegisteredTransport = nullptr;
}

// --- Threads: the stopped thread is KiPcr's current one; another's registers are in its switch frame (arch/context_switch.S: ExceptionList, EFLAGS, edi,
// esi, ebx, ebp, return address, from KernelStack up).

static bool IsReadable(const void *Start, ULONG Length) {
    return MmIsAddressValid((PVOID)Start) && MmIsAddressValid((PVOID)((ULONG)Start + Length - 1));
}

static PVOID GetCurrentThread() {
    return KiPcr.PrcbData.CurrentThread;
}

static ULONG ListThreads(PVOID *Threads, ULONG Max) {
    ULONG count = 0;
    if (!KiPcr.PrcbData.CurrentThread || !Max)
        return 0; // before the scheduler: only the boot
    Threads[count++] = &core::IdleThreadObject;
    PLIST_ENTRY head = &core::SystemProcess.ThreadListHead;
    for (PLIST_ENTRY e = head->Flink; e && e != head && count < Max; e = e->Flink) {
        if (!IsReadable(e, sizeof(*e)))
            break;
        PKTHREAD t = CONTAINING_RECORD(e, KTHREAD, ThreadListEntry);
        if (t != &core::IdleThreadObject && IsReadable(t, sizeof(KTHREAD)))
            Threads[count++] = t;
    }
    return count;
}

static BOOLEAN ReadThreadRegisters(PVOID Thread, ULONG Registers[16]) {
    auto t = (PKTHREAD)Thread;
    if (!IsReadable(t, sizeof(KTHREAD)))
        return FALSE;
    auto stack = (const ULONG *)t->KernelStack;
    if (!IsReadable(stack, 7 * sizeof(ULONG)))
        return FALSE;
    // eax, ecx and edx are not saved by a switch: 0.
    for (int i = 0; i < 16; i++)
        Registers[i] = i >= 11 ? 0x10 : 0; // ss ds es fs gs: the kernel's one data segment
    Registers[3] = stack[4];
    Registers[4] = (ULONG)(stack + 7);
    Registers[5] = stack[5];
    Registers[6] = stack[3];
    Registers[7] = stack[2];
    Registers[8] = stack[6];
    Registers[9] = stack[1];
    Registers[10] = 8; // cs
    return TRUE;
}

static BOOLEAN DescribeThread(PVOID Thread, ULONG *State, LONG *Priority) {
    auto t = (PKTHREAD)Thread;
    if (!IsReadable(t, sizeof(KTHREAD)))
        return FALSE;
    *State = t->State;
    *Priority = t->Priority;
    return TRUE;
}

static PVOID GetFloatOwnerThread() {
    return KiPcr.PrcbData.NpxThread;
}

static PVOID GetFloatArea(PVOID Thread) {
    return (UCHAR *)((PKTHREAD)Thread)->StackBase - NPX_FRAME_LENGTH;
}

// --- Images: the kernel modules while mapped, then the extensions (the debugger knows the kernel and the title itself)

static ULONG ListImages(OXOS_DEBUG_IMAGE *Images, ULONG Max) {
    ULONG count = 0;
    if (kernel_modules::Modules && count < Max)
        Images[count++] = {"kernel_modules", (PVOID)XK_KERNEL_MODULES_BASE, OXOS_DEBUG_IMAGE_KERNEL_MODULES};
    const XK_EXTENSIONS_STATE *loaded = extensions::LoadedExtensions;
    for (ULONG i = 0; loaded && i < loaded->Count && count < Max; i++)
        Images[count++] = {loaded->Extensions[i].Context.SectionName, loaded->Extensions[i].Context.ImageBase, OXOS_DEBUG_IMAGE_EXTENSION};
    return count;
}

static const OXOS_DEBUG_PORT DEBUG_PORT = {sizeof(OXOS_DEBUG_PORT), AttachDebugger, DetachDebugger, RegisterTransport, UnregisterTransport, GetTransport,
                                           MmIsAddressValid, GetCurrentThread, ListThreads, ReadThreadRegisters, DescribeThread, GetFloatOwnerThread,
                                           GetFloatArea, ListImages};

const OXOS_DEBUG_PORT *GetDebugPort() {
    return &DEBUG_PORT;
}

// --- After a bug check

void WaitAfterBugCheck() {
    for (;;) {
        const OXOS_DEBUGGER *debugger = AttachedDebugger;
        if (debugger)
            debugger->WaitAfterBugCheck(); // the debugger serves its host; the Modxo's power button still works through it
        else
            halt();
    }
}

} // namespace xk::debugger
