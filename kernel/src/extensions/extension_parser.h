// PE images for kernel extensions (spec/oxos_internal/34 §8): checking, mapping, relocating, binding imports and finding an export. It makes no kernel calls,
// so the host unit tests run it unchanged (tests/unit/test_extension_parser.cpp).
#pragma once
#include <runtime/types.h>

// Why an image is refused (none: it is fine).
enum class XK_PE_ERROR {
    NONE,
    NOT_PE,          // no MZ / PE headers, or headers outside the file
    NOT_I386_DLL,    // another machine, not a DLL, or not a 32-bit image
    NO_RELOCATIONS,  // relocations stripped: the image cannot be moved
    HAS_TLS,         // a TLS directory (not supported)
    BAD_SECTION,     // a section outside the file or the image
    BAD_RELOCATION,  // a relocation block or entry the loader does not handle
    BAD_IMPORT,      // an import from another DLL than oxoskrnl.exe, by name, or an ordinal the resolver refuses
};

// Resolves one import: Ordinal of oxoskrnl.exe for the import address table entry at *Slot (an address in Image). Returns false to refuse it.
typedef bool (*XK_PE_IMPORT_RESOLVER)(PVOID Context, ULONG Ordinal, ULONG *Slot);

namespace xk::extensions {

// Checks that File (Size bytes) is an i386 PE DLL with relocations and no TLS, whose headers and sections lie inside the file; *ImageSize receives its
// SizeOfImage.
XK_PE_ERROR CheckImage(const UCHAR *File, ULONG Size, ULONG *ImageSize);

// Lays a checked File out in Image (ImageSize bytes, as CheckImage returned): headers and sections at their RVAs, the rest zero, then relocated to run at Base
// (Image's address in the kernel; another one in the host tests).
XK_PE_ERROR MapImage(const UCHAR *File, ULONG Size, UCHAR *Image, ULONG ImageSize, ULONG Base);


// Binds a mapped image's imports: every one must be an ordinal of oxoskrnl.exe (any case); Resolve fills each slot.
XK_PE_ERROR BindImports(UCHAR *Image, ULONG ImageSize, XK_PE_IMPORT_RESOLVER Resolve, PVOID Context);

// The RVA of a mapped image's export named Name, or 0 when it has none.
ULONG FindExport(const UCHAR *Image, ULONG ImageSize, const char *Name);

} // namespace xk::extensions
