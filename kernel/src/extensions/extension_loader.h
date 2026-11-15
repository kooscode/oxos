// The kernel extension loader's image (spec/oxos_internal/34 §8.1): linked on its own at XK_EXTENSION_LOADER_BASE against this kernel's addresses
// (tools/link_extension_loader.py) and appended to the kernel image by tools/mkbios.py, in the boot-only pages, behind a header. A cold boot writes it to
// C:\OXOS\cache\oxos_extension_loader.cache (extension_runtime_loader.cpp); every launch runs it from that file.
#pragma once
#include <runtime/types.h>

constexpr ULONG XK_EXTENSION_LOADER_BASE = 0xB0000000;   // its link address, mapped only while it runs
constexpr ULONG XK_EXTENSION_LOADER_MAX_BYTES = 0x10000; // its largest image
constexpr ULONG XK_EXTENSION_LOADER_MAGIC = 0x4C564F58;  // "XOVL"
constexpr ULONG XK_EXTENSION_LOADER_HEADER_FIELD = 0x38; // in the kernel's DOS header: the header's RVA (0: none)

// In front of the loader's image, in the kernel image and in the cache file.
struct XK_EXTENSION_LOADER_HEADER {
    ULONG Magic;
    ULONG Stamp; // the kernel's PE TimeDateStamp it was linked against
    ULONG Base;  // XK_EXTENSION_LOADER_BASE
    ULONG Size;  // bytes of the image after this header (its SizeOfImage)
    ULONG Entry; // the entry point's RVA: XkExtensionLoaderEntry
};
XK_SIZE(XK_EXTENSION_LOADER_HEADER, 20);

// The loader's entry point (src/extensions/extension_loader.cpp).
extern "C" void XkExtensionLoaderEntry();
namespace xk::extensions {
// Cold boot, boot-only: writes the loader from the kernel image to the cache file, when the extensions folder exists.
void WriteLoaderCache();
} // namespace xk::extensions
