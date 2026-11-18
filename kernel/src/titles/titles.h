// Title loading and launch (spec/stock/05): the state other subsystems read, the netboot title and the import tracer.
#pragma once
#include <core/core.h>
#include <objects/objects.h>
#include <runtime/types.h>

// The launch state other subsystems read (titles/title_loader.cpp).
XBAPI PVOID LaunchDataPage;             // the page a title passes to the next one it launches (spec/stock/05 §9)
XBAPI OBJECT_STRING XeImageFileName;    // the running title's XBE (\Device\...)

namespace xk::titles {

// An XBE section header (spec/stock/05 §2), as the title loader and the title patcher module (kernel/modules/title_patcher.cpp) read it.
struct XBE_SECTION_HEADER {
    ULONG Flags, VirtualAddress, VirtualSize, PointerToRawData, SizeOfRawData, SectionName;
    ULONG SectionReferenceCount;
    USHORT *HeadSharedPageReferenceCount, *TailSharedPageReferenceCount;
    UCHAR SectionDigest[20];
};
static_assert(sizeof(XBE_SECTION_HEADER) == 0x38);

extern ULONG ConsoleGameRegion;      // the console's game region (from the EEPROM; all regions until it is read)
extern ULONG DiscTitleId;            // the title pinned to the current game disc (0 = none)
extern UCHAR TitleDiscAuthenticated; // the disc in the drive passed the DVD authentication

void StartTitleLauncher(); // the end of start-up: load and enter the first title
// Boot-only (titles/title_loader.cpp, spec/oxos_internal/18 §2): the cold boot's choice of what to launch, and the OXOS File Manager (spec/oxos_internal/31)
// until a browser launches an XBE that loads (false: no network card).
void LaunchAtColdBoot();
bool ServeFileManager();

// A title delivered by netboot (titles/title_netboot.cpp): read before the bank's pages are reused, launched instead of the disc or dashboard, and its pages
// kept from the memory manager until then.
void ReadNetbootTitle();
bool LaunchNetbootTitle();                        // boot-only: call it only while memory::BootPagesPresent()
NTSTATUS LoadImage(PCSZ Path, BOOLEAN Dashboard, char *Target = nullptr); // load an XBE (titles/title_loader.cpp); the caller enters it
// A whole file of at most MaxSize bytes, in pool memory the caller frees, with its length in *Got. Null when it does not open (*Status says why:
// STATUS_OBJECT_NAME_NOT_FOUND when it does not exist), is empty or larger, or cannot be read (titles/title_loader.cpp; the kernel's files in C:\OXOS).
char *ReadOXOSFile(PCSZ Path, ULONG MaxSize, ULONG Tag, ULONG *Got, NTSTATUS *Status);
// The address of the kernel's export Ordinal, or 0 when it has none (titles/title_loader.cpp).
ULONG KernelExportAddress(ULONG Ordinal);
bool IsNetbootTitlePage(ULONG pfn);

} // namespace xk::titles

// The import tracer (titles/title_tracer.cpp, TRACE_IMPORTS builds): its ring of calls, read by the bug check report.
ULONG XkTraceWrap(USHORT ordinal, ULONG target);
constexpr ULONG XK_TRACE_WORDS = 15;    // a ring entry: ordinal, caller, thread, three arguments, result, 32-byte name
constexpr ULONG XK_TRACE_RING = 0x8000; // entries in the ring
constexpr ULONG XK_TRACE_MAX_IMPORTS = 512; // imports that can be traced (a 10-byte stub each)
// TRACE_IMPORTS=2 (128 MB): the ring and the stubs at a fixed physical address near the top of RAM, kept from the memory manager (memory/init.cpp), clear of
// the second GPU instance memory at 0x7FE0000.
constexpr ULONG XK_TRACE_HIGH_PHYSICAL = 0x7C00000;
constexpr ULONG XK_TRACE_HIGH_BYTES = XK_TRACE_RING * XK_TRACE_WORDS * 4 + XK_TRACE_MAX_IMPORTS * 10;
extern "C" volatile ULONG (*XkTraceRing)[XK_TRACE_WORDS];
extern "C" volatile ULONG XkTraceCount;
extern "C" void XkTraceThreadExit(PKTHREAD Thread);
