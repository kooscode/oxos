// Runs the kernel extension loader (spec/oxos_internal/34 §8.1). The loader (extension_ini.cpp, extension_loader.cpp
// and extension_parser.cpp in this folder) is not part of the resident kernel: tools/link_extension_loader.py links it
// at XK_EXTENSION_LOADER_BASE against this kernel's addresses and tools/mkbios.py appends it to the kernel image, in
// the boot-only pages. A cold boot writes it to the cache file; every launch reads it from there into temporary pages,
// maps them at XK_EXTENSION_LOADER_BASE, runs it, and frees them before the title loads.
#include <xk/extensions.h>
#include <xk/io.h>
#include <xk/memory.h>
#include <xk/objects.h>
#include <xk/runtime.h>
#include <xk/serial.h>
#include <xk/titles.h>

namespace xk::extensions {

namespace {

constexpr char CACHE_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions\\oxos_extension_loader.cache";
constexpr char LIST_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_extensions.ini";
constexpr ULONG POOL_TAG = 'lvOX';
constexpr ULONG KERNEL_BASE = 0x80010000;
constexpr ULONG PAGE_RW = PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;

// The kernel's PE TimeDateStamp: the loader must have been linked against this build.
ULONG KernelStamp() {
    auto image = (const UCHAR *)KERNEL_BASE;
    return *(const ULONG *)(image + *(const ULONG *)(image + 0x3C) + 8);
}

void ReloadCr3() {
    ULONG cr3;
    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3)::"memory");
}

} // namespace

// --- The cache file, written at a cold boot (boot-only: the loader it copies is in the boot-only pages)

#pragma code_seg(".xkb")

void WriteLoaderCache() {
    ULONG rva = *(const ULONG *)(KERNEL_BASE + XK_EXTENSION_LOADER_HEADER_FIELD);
    if (!rva)
        return;
    auto header = (XK_EXTENSION_LOADER_HEADER *)(KERNEL_BASE + rva);
    OBJECT_STRING name;
    RtlInitAnsiString(&name, CACHE_PATH);
    OBJECT_ATTRIBUTES attributes = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    // No extensions folder: no extensions, and nothing to write.
    NTSTATUS status = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &iosb, nullptr, 0, 0,
                                   FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(status))
        return;
    LARGE_INTEGER at;
    at.QuadPart = 0;
    status = NtWriteFile(file, nullptr, nullptr, nullptr, &iosb, header, sizeof(*header) + header->Size, &at);
    NtClose(file);
    XK_SERIAL("extensions: loader cache written (%lu bytes, %#lx)\n", sizeof(*header) + header->Size, status);
}

#pragma code_seg()

// --- Running the loader

void LoadExtensions() {
    if (SafeMode) {
        XK_SERIAL("extensions: safe mode (B at boot): none loaded\n");
        return;
    }
    if (!KiQuickRebootFlag && memory::BootPagesPresent())
        WriteLoaderCache();
    // Without the list there is nothing to load (and the cache is not read), unless the last launch's state pages
    // are still kept: the loader then frees them (§6).
    OBJECT_STRING listName;
    RtlInitAnsiString(&listName, LIST_PATH);
    OBJECT_ATTRIBUTES list = {nullptr, &listName, OBJ_CASE_INSENSITIVE};
    FILE_NETWORK_OPEN_INFORMATION info;
    if (!StateIndexPage && !NT_SUCCESS(NtQueryFullAttributesFile(&list, &info)))
        return;
    ULONG got;
    NTSTATUS status;
    ULONG largest = sizeof(XK_EXTENSION_LOADER_HEADER) + XK_EXTENSION_LOADER_MAX_BYTES;
    auto cache = (PUCHAR)titles::ReadOxosFile(CACHE_PATH, largest, POOL_TAG, &got, &status);
    auto header = (const XK_EXTENSION_LOADER_HEADER *)cache;
    if (!cache || got < sizeof(*header) || header->Magic != XK_EXTENSION_LOADER_MAGIC ||
        header->Stamp != KernelStamp() || header->Base != XK_EXTENSION_LOADER_BASE ||
        header->Size > XK_EXTENSION_LOADER_MAX_BYTES ||
        got != sizeof(*header) + header->Size || header->Entry >= header->Size) {
        XK_SERIAL("extensions: no loader cache for this kernel (%#lx): none loaded\n", status);
        if (cache)
            ExFreePool(cache);
        return;
    }
    ULONG size = header->Size, entry = header->Entry;
    auto pages = (PUCHAR)MmAllocateContiguousMemory(size);
    auto table = (ULONG *)MmAllocateContiguousMemory(PAGE_SIZE);
    ULONG *pde = memory::PdeAddress(XK_EXTENSION_LOADER_BASE);
    if (pages && table && !*pde) {
        RtlCopyMemory(pages, header + 1, size);
        RtlZeroMemory(table, PAGE_SIZE);
        for (ULONG at = 0; at < size; at += PAGE_SIZE)
            table[at >> PAGE_SHIFT] = MmGetPhysicalAddress(pages + at) | PAGE_RW;
        *pde = MmGetPhysicalAddress(table) | PAGE_RW;
        ReloadCr3();
        ((void (*)())(XK_EXTENSION_LOADER_BASE + entry))();
        *pde = 0;
        ReloadCr3();
    } else {
        XK_SERIAL("extensions: no memory or address for the loader: none loaded\n");
    }
    ExFreePool(cache);
    if (table)
        MmFreeContiguousMemory(table);
    if (pages)
        MmFreeContiguousMemory(pages);
}

} // namespace xk::extensions
