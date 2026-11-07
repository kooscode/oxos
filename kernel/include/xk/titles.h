// Title loading and launch (spec/stock/05): the state other subsystems read, the netboot title and the import tracer.
#pragma once
#include <xk/core.h>
#include <xk/types.h>

// The launch state other subsystems read (titles/loader.cpp).
XBAPI PVOID LaunchDataPage;             // the page a title passes to the next one it launches (spec/stock/05 §9)
extern ULONG XeConsoleGameRegion;       // the console's game region (from the EEPROM; all regions until it is read)
extern ULONG XeDiscTitleId;             // the title pinned to the current game disc (0 = none)
extern UCHAR XeTitleDiscAuthenticated;  // the disc in the drive passed the DVD authentication

void XeStartTitleLauncher(); // the end of start-up: load and enter the first title

// A title delivered by netboot (titles/nettitle.cpp): read before the bank's pages are reused, launched instead of the
// disc or dashboard, and its pages kept from the memory manager until then.
extern "C" void XeReadNetbootTitle();
bool XeLaunchNetbootTitle();
NTSTATUS XeLoadImage(PCSZ Path, BOOLEAN Dashboard); // load an XBE (titles/loader.cpp); the caller enters it
bool MiIsNetbootTitlePage(ULONG pfn);

// The import tracer (titles/trace.cpp, TRACE_IMPORTS builds): its ring of calls, read by the bug check report.
ULONG XkTraceWrap(USHORT ordinal, ULONG target);
constexpr ULONG XK_TRACE_WORDS = 15;    // a ring entry: ordinal, caller, thread, three arguments, result, 32-byte name
constexpr ULONG XK_TRACE_RING = 0x8000; // entries in the ring
extern "C" volatile ULONG (*XkTraceRing)[XK_TRACE_WORDS];
extern "C" volatile ULONG XkTraceCount;
extern "C" void XkTraceThreadExit(PKTHREAD Thread);
