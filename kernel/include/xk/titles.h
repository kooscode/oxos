// Title loading and launch (spec/stock/05): the state other subsystems read, the netboot title and the import tracer.
#pragma once
#include <xk/core.h>
#include <xk/objects.h>
#include <xk/types.h>

// The launch state other subsystems read (titles/loader.cpp).
XBAPI PVOID LaunchDataPage;             // the page a title passes to the next one it launches (spec/stock/05 §9)
XBAPI OBJECT_STRING XeImageFileName;    // the running title's XBE (\Device\...)
extern ULONG XeConsoleGameRegion;       // the console's game region (from the EEPROM; all regions until it is read)
extern ULONG XeDiscTitleId;             // the title pinned to the current game disc (0 = none)
extern UCHAR XeTitleDiscAuthenticated;  // the disc in the drive passed the DVD authentication

void XeStartTitleLauncher(); // the end of start-up: load and enter the first title
// Boot-only (titles/loader.cpp, spec/oxos_internal/18 §2): the cold boot's choice of what to launch, and the OXOS File
// Manager (spec/oxos_internal/31) until a browser launches an XBE that loads (false: no network card).
void XeColdBootLaunch();
bool XeServeFileManager();

// A title delivered by netboot (titles/nettitle.cpp): read before the bank's pages are reused, launched instead of the
// disc or dashboard, and its pages kept from the memory manager until then.
extern "C" void XeReadNetbootTitle();
bool XeLaunchNetbootTitle(); // boot-only: call it only while MmBootPagesPresent()
NTSTATUS XeLoadImage(PCSZ Path, BOOLEAN Dashboard); // load an XBE (titles/loader.cpp); the caller enters it
// A whole file of at most MaxSize bytes, in pool memory the caller frees, with its length in *Got. Null when it does
// not open (*Status says why: STATUS_OBJECT_NAME_NOT_FOUND when it does not exist), is empty or larger, or cannot be
// read (titles/loader.cpp; the kernel's files in C:\OXOS).
char *XeReadOxosFile(PCSZ Path, ULONG MaxSize, ULONG Tag, ULONG *Got, NTSTATUS *Status);
// The address of the kernel's export Ordinal, or 0 when it has none (titles/loader.cpp).
ULONG XeKernelExport(ULONG Ordinal);
bool MiIsNetbootTitlePage(ULONG pfn);

// The import tracer (titles/trace.cpp, TRACE_IMPORTS builds): its ring of calls, read by the bug check report.
ULONG XkTraceWrap(USHORT ordinal, ULONG target);
constexpr ULONG XK_TRACE_WORDS = 15;    // a ring entry: ordinal, caller, thread, three arguments, result, 32-byte name
constexpr ULONG XK_TRACE_RING = 0x8000; // entries in the ring
constexpr ULONG XK_TRACE_MAX_IMPORTS = 512; // imports that can be traced (a 10-byte stub each)
// TRACE_IMPORTS=2 (128 MB): the ring and the stubs at a fixed physical address near the top of RAM, kept from the
// memory manager (memory/init.cpp), clear of the second GPU instance memory at 0x7FE0000.
constexpr ULONG XK_TRACE_HIGH_PHYSICAL = 0x7C00000;
constexpr ULONG XK_TRACE_HIGH_BYTES = XK_TRACE_RING * XK_TRACE_WORDS * 4 + XK_TRACE_MAX_IMPORTS * 10;
extern "C" volatile ULONG (*XkTraceRing)[XK_TRACE_WORDS];
extern "C" volatile ULONG XkTraceCount;
extern "C" void XkTraceThreadExit(PKTHREAD Thread);
