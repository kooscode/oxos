// Import tracing (TRACE_IMPORTS=1): a ring of the title's most recent kernel calls, read by the host tools.
#include <xk/ke.h>
#include <xk/rtl.h>
#include <xk/ob.h>
#include <xk/io.h>
#include <xk/serial.h>

constexpr ULONG XK_TRACE_MAX_IMPORTS = 512, XK_TRACE_RING = 0x8000;


extern "C" {
ULONG XkTraceTargets[XK_TRACE_MAX_IMPORTS];
USHORT XkTraceOrdinals[XK_TRACE_MAX_IMPORTS];
// Ring entries: ordinal, caller, thread, first three stack arguments, return value, then 32 bytes of name.
constexpr ULONG XK_TRACE_WORDS = 15;
volatile ULONG (*XkTraceRing)[XK_TRACE_WORDS]; // allocated at the top of RAM (not in the image)
// Persisted: with TRACE_IMPORTS=2 the ring above 64 MB keeps the calls from before a quick reboot too.
XK_PERSIST volatile ULONG XkTraceCount;
void XkTraceEntry();
void XkTraceReturn();

// Calls in flight per thread (title APCs can nest calls), to find the original return address again. Titles keep
// dozens of threads blocked in a wait call, each holding a slot until its thread exits.
struct XK_TRACE_SLOT {
    ULONG Thread, Depth, Return[8], Entry[8];
};
static XK_TRACE_SLOT XkTraceSlots[128];

void XkTraceThreadExit(PKTHREAD Thread) {
    ULONG flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags)); // a title's interrupt routine may be recording
    for (auto &s : XkTraceSlots)
        if (s.Thread == (ULONG)Thread)
            s.Thread = s.Depth = 0;
    __asm__ volatile("pushl %0; popfl" ::"r"(flags) : "cc");
}

static XK_TRACE_SLOT *XkTraceSlot(ULONG thread) {
    for (auto &s : XkTraceSlots)
        if (s.Thread == thread)
            return &s;
    for (auto &s : XkTraceSlots)
        if (!s.Thread || !s.Depth) {
            s.Thread = thread;
            s.Depth = 0;
            return &s;
        }
    return nullptr;
}

// The object name of an OBJECT_ATTRIBUTES (or an OBJECT_STRING when `string`), for the file and link APIs.
static void XkTraceName(volatile ULONG *e, ULONG p, bool string) {
    auto out = (volatile char *)(e + 7);
    for (int i = 0; i < 32; i++)
        out[i] = 0;
    if (p < 0x10000)
        return;
    auto str = string ? (POBJECT_STRING)p : ((POBJECT_ATTRIBUTES)p)->ObjectName;
    if ((ULONG)str < 0x10000 || (ULONG)str->Buffer < 0x10000)
        return;
    ULONG n = str->Length, skip = n > 31 ? n - 31 : 0; // keep the end of long paths
    for (ULONG i = 0; i + skip < n && i < 31; i++)
        out[i] = str->Buffer[skip + i];
}

#if XK_DEBUG_SERIAL
// On a console the ring cannot be read (no QMP): calls that return an error status (0xC0xxxxxx) are also printed on
// the serial port as they return - "t fail ordinal N -> status from ADDR args A B C [name]" - with runs of the same
// call from the same place counted instead of repeated. tools/modxo logs show them; kernel/exports.tsv names N.
static ULONG XkFailOrdinal, XkFailFrom, XkFailResult, XkFailRepeats;
static void XkTraceReportFailure(volatile ULONG *e, ULONG result) {
    ULONG ordinal = e[0], from = e[1];
    if (ordinal == XkFailOrdinal && from == XkFailFrom && result == XkFailResult) {
        XkFailRepeats++;
        return;
    }
    if (XkFailRepeats)
        XK_SERIAL("t   (x%lu more)\n", XkFailRepeats);
    XkFailOrdinal = ordinal, XkFailFrom = from, XkFailResult = result, XkFailRepeats = 0;
    char name[32] = {};
    // The calls XkTraceRecord stores a name for: file opens keep 23 characters (then their share and options).
    int n = ordinal == 190 || ordinal == 202 ? 23 : ordinal == 210 || ordinal == 195 || ordinal == 67 || ordinal == 69 ? 31 : 0;
    for (int i = 0; i < n; i++)
        name[i] = ((volatile char *)(e + 7))[i];
    if (ordinal == 219) // NtReadFile: handle, length, byte offset
        XK_SERIAL("t fail NtReadFile(%#lx, %#lx bytes at %#lx'%08lx) -> %#lx from %#lx\n", e[3], e[11], e[13], e[12],
                  result, from);
    else
        XK_SERIAL("t fail ordinal %lu -> %#lx from %#lx args %#lx %#lx %#lx %s\n", ordinal, result, from, e[3], e[4],
                  e[5], name);
}

// Successful reads, the first 30 of each handle: "t read HANDLE LENGTH at OFFSET -> BYTES" (offset -1: the current
// position).
static ULONG XkTraceReadHandles[64], XkTraceReadCounts[64];
static void XkTraceReportRead(volatile ULONG *e) {
    ULONG i = 0;
    while (i < 64 && XkTraceReadHandles[i] && XkTraceReadHandles[i] != e[3])
        i++;
    if (i == 64)
        return;
    XkTraceReadHandles[i] = e[3];
    if (XkTraceReadCounts[i]++ >= 30)
        return;
    auto iosb = (PIO_STATUS_BLOCK)e[9];
    ULONG got = (ULONG)iosb >= 0x10000 ? (ULONG)iosb->Information : 0xFFFFFFFF;
    XK_SERIAL("t read %#lx %#lx at %#lx'%08lx -> %#lx bytes, status %#lx (event %#lx apc %#lx)\n", e[3], e[11],
              e[13], e[12], got, e[6], e[4], e[5]);
    auto data = (const UCHAR *)e[10];
    if (got != 0xFFFFFFFF && got && (ULONG)data >= 0x10000)
        XK_SERIAL("t   data %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", data[0],
                  data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11],
                  data[12], data[13], data[14], data[15]);
}

// Successful file opens, so a failure on a handle can be matched to its file: "t open HANDLE name".
static void XkTraceReportOpen(volatile ULONG *e) {
    char name[24] = {};
    for (int i = 0; i < 23; i++)
        name[i] = ((volatile char *)(e + 7))[i];
    auto handle = (PHANDLE)e[3];
    if ((ULONG)handle >= 0x10000)
        XK_SERIAL("t open %#lx %s\n", (ULONG)*handle, name);
}
#endif

ULONG __cdecl XkTraceLeave(ULONG result) {
    ULONG flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    XK_TRACE_SLOT *s = XkTraceSlot((ULONG)KeGetCurrentThread());
    ULONG ret = 0;
    [[maybe_unused]] volatile ULONG *failed = nullptr;
    if (s && s->Depth) {
        s->Depth--;
        ret = s->Return[s->Depth];
        volatile ULONG *e = XkTraceRing[s->Entry[s->Depth] % XK_TRACE_RING];
        e[6] = result;
        if ((result & 0xFF000000) == 0xC0000000 || (!result && (e[0] == 190 || e[0] == 202)) ||
            (e[0] == 219 && (result & 0xFF000000) != 0xC0000000) ||
            (result & 0xFF000000) == 0x80000000 || result == 0x102 || result == 0x103) // warnings, timeouts, pending
            failed = e;
    }
    __asm__ volatile("pushl %0; popfl" ::"r"(flags) : "cc");
#if XK_DEBUG_SERIAL
    if (failed && failed[0] == 219 && (result & 0xFF000000) != 0xC0000000)
        XkTraceReportRead(failed);
    else if (failed && result)
        XkTraceReportFailure(failed, result);
    else if (failed)
        XkTraceReportOpen(failed);
#endif
    return ret;
}

void __cdecl XkTraceRecord(ULONG index, PULONG frame) {
    switch (XkTraceOrdinals[index]) { // too frequent to be useful
    case 103: case 104: case 119: case 125: case 126: case 128: case 129: case 160: case 161:
    case 221: case 234: case 238: case 277: case 278: case 294: case 295: // Halo 2 spins on 238 (NtYieldExecution)
    // and takes a mutant (221 release, 234 wait) around every step
        return;
    }
    if (!XkTraceRing)
        return;
    ULONG flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    ULONG entry = XkTraceCount;
    volatile ULONG *e = XkTraceRing[entry % XK_TRACE_RING];
    e[0] = XkTraceOrdinals[index];
    e[1] = frame[0];
    e[2] = (ULONG)KeGetCurrentThread();
    e[3] = frame[1];
    e[4] = frame[2];
    e[5] = frame[3];
    e[6] = 0xDEADDEAD; // no return seen yet
    switch (e[0]) {
    case 190: case 202:           // NtCreateFile, NtOpenFile (object attributes are the 3rd argument)
        XkTraceName(e, frame[3], false);
        ((volatile UCHAR *)&e[12])[3] = 0; // name: 23 characters; then share access and create/open options
        e[13] = e[0] == 202 ? frame[5] : frame[7];
        e[14] = e[0] == 202 ? frame[6] : frame[9];
        break;
    case 210: case 195:           // NtQueryFullAttributesFile, NtDeleteFile (1st)
        XkTraceName(e, frame[1], false);
        break;
    case 67: case 69:             // IoCreateSymbolicLink, IoDeleteSymbolicLink
        XkTraceName(e, frame[1], true);
        break;
    case 219: case 236: {         // NtReadFile/NtWriteFile: ApcContext, IoStatusBlock, Buffer, Length, *ByteOffset,
        e[7] = 0x10000000 | e[0]; // and the first dword of the buffer (written data, or what a read overwrites)
        e[8] = frame[4];
        e[9] = frame[5];
        e[10] = frame[6];
        e[11] = frame[7];
        auto offset = (PULONG)frame[8];
        e[12] = offset ? offset[0] : 0xFFFFFFFF;
        e[13] = offset ? offset[1] : 0xFFFFFFFF;
        e[14] = frame[6] ? *(PULONG)frame[6] : 0;
        break;
    }
    case 226: {                   // NtSetInformationFile: class, length, the first 4 dwords of the information
        e[7] = 0x10000000 | e[0];
        e[8] = frame[5];
        e[9] = frame[4];
        auto info = (PULONG)frame[3];
        for (int i = 0; i < 4; i++)
            e[10 + i] = info[i];
        e[14] = 0;
        break;
    }
    default:
        e[7] = 0;
    }
    XkTraceCount = XkTraceCount + 1;
    if (XK_TRACE_SLOT *s = XkTraceSlot(e[2]); s && s->Depth < 8) { // catch the return
        s->Return[s->Depth] = frame[0];
        s->Entry[s->Depth] = entry;
        s->Depth++;
        frame[0] = (ULONG)XkTraceReturn;
    }
    __asm__ volatile("pushl %0; popfl" ::"r"(flags) : "cc");
}
}

// Returns the address to put in the thunk: a 10-byte stub (mov eax, index; jmp XkTraceEntry) for functions.
ULONG XkTraceWrap(USHORT ordinal, ULONG target) {
    static PUCHAR stubs;
    static ULONG used;
    if (!stubs) {
#if XK_TRACE_IMPORTS == 2
        // TRACE_IMPORTS=2 needs 128 MB (xemu mem_limit = '128'): ring and stubs live above the 64 MB the kernel
        // manages, so the title sees exactly the memory of a release build.
        ULONG ringBytes = XK_TRACE_RING * XK_TRACE_WORDS * 4;
        auto high = (PUCHAR)MmMapIoSpace(0x4000000, ringBytes + XK_TRACE_MAX_IMPORTS * 10, PAGE_READWRITE);
        XkTraceRing = (volatile ULONG(*)[XK_TRACE_WORDS])high;
        stubs = high ? high + ringBytes : nullptr;
#else
        stubs = (PUCHAR)ExAllocatePoolWithTag(XK_TRACE_MAX_IMPORTS * 10, 'crTX');
        XkTraceRing = (volatile ULONG(*)[XK_TRACE_WORDS])MmAllocateContiguousMemory(XK_TRACE_RING * XK_TRACE_WORDS * 4);
        // The count survives a quick reboot but this ring does not: clear it, so older entries read as empty.
        if (XkTraceRing)
            RtlZeroMemory((PVOID)XkTraceRing, XK_TRACE_RING * XK_TRACE_WORDS * 4);
#endif
    }
    if (!stubs || used == XK_TRACE_MAX_IMPORTS)
        return target;
    // Only code is wrapped: data exports (KeTickCount, LaunchDataPage, ...) live outside the first (code) section.
    auto image = (PUCHAR)0x80010000;
    PUCHAR nt = image + *(ULONG *)(image + 0x3C);
    PUCHAR section = nt + 24 + *(USHORT *)(nt + 20);
    ULONG textStart = 0x80010000 + *(ULONG *)(section + 12), textEnd = textStart + *(ULONG *)(section + 8);
    if (target < textStart || target >= textEnd)
        return target;
    ULONG i = used++;
    XkTraceTargets[i] = target;
    XkTraceOrdinals[i] = ordinal;
    PUCHAR s = stubs + i * 10;
    s[0] = 0xB8;
    *(ULONG *)(s + 1) = i;
    s[5] = 0xE9;
    *(LONG *)(s + 6) = (LONG)((ULONG)XkTraceEntry - (ULONG)(s + 10));
    return (ULONG)s;
}
