// The kernel module loader and unloader (kernel_modules.h, spec/oxos_internal/43): the modules' image mapped at XK_KERNEL_MODULES_BASE for one launch,
// through a temporary page table, and freed before the title starts. Resident: quick reboots load it too.
#include <kernel_modules/kernel_modules.h>
#include <diagnostics/diagnostics.h>
#include <io/io.h>
#include <memory/memory.h>
#include <objects/objects.h>
#include <runtime/runtime.h>
#include <titles/titles.h>

namespace xk::kernel_modules {

const XK_KERNEL_MODULES *Modules;

namespace {

constexpr char CACHE_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\cache\\oxos_kernel.cache";
constexpr ULONG POOL_TAG = 'mkOX';
constexpr ULONG KERNEL_BASE = 0x80010000;
constexpr ULONG PAGE_RW = PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;

// The image's pages and their page table while it is mapped.
PUCHAR ImagePages;
ULONG *ImageTable;

// The kernel's PE TimeDateStamp: the image must have been linked against this build.
ULONG KernelStamp() {
    auto image = (const UCHAR *)KERNEL_BASE;
    return *(const ULONG *)(image + *(const ULONG *)(image + 0x3C) + 8);
}

void ReloadCr3() {
    ULONG cr3;
    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3)::"memory");
}

// The header in the kernel image (its boot-only pages), or null when the image has none.
const XK_KERNEL_MODULES_HEADER *ImageHeader() {
    ULONG rva = *(const ULONG *)(KERNEL_BASE + XK_KERNEL_MODULES_HEADER_FIELD);
    return rva ? (const XK_KERNEL_MODULES_HEADER *)(KERNEL_BASE + rva) : nullptr;
}

bool IsHeaderValid(const XK_KERNEL_MODULES_HEADER *Header, ULONG Bytes) {
    return Header && Bytes >= sizeof(*Header) && Header->Magic == XK_KERNEL_MODULES_MAGIC && Header->Stamp == KernelStamp() &&
           Header->Base == XK_KERNEL_MODULES_BASE && Header->Size <= XK_KERNEL_MODULES_MAX_BYTES && Bytes == sizeof(*Header) + Header->Size &&
           Header->Entry < Header->Size;
}

// Copies the image into contiguous pages, maps them at XK_KERNEL_MODULES_BASE and takes the table from the entry point.
bool MapImage(const XK_KERNEL_MODULES_HEADER *Header) {
    ULONG *pde = memory::PdeAddress(XK_KERNEL_MODULES_BASE);
    ImagePages = (PUCHAR)MmAllocateContiguousMemory(Header->Size);
    ImageTable = (ULONG *)MmAllocateContiguousMemory(PAGE_SIZE);
    if (!ImagePages || !ImageTable || *pde) {
        XK_SERIAL("modules: no memory or address for the kernel modules\n");
        UnloadKernelModules();
        return false;
    }
    memcpy(ImagePages, Header + 1, Header->Size);
    memset(ImageTable, 0, PAGE_SIZE);
    for (ULONG at = 0; at < Header->Size; at += PAGE_SIZE)
        ImageTable[at >> PAGE_SHIFT] = MmGetPhysicalAddress(ImagePages + at) | PAGE_RW;
    *pde = MmGetPhysicalAddress(ImageTable) | PAGE_RW;
    ReloadCr3();
    Modules = ((const XK_KERNEL_MODULES *(*)())(XK_KERNEL_MODULES_BASE + Header->Entry))();
    return true;
}

} // namespace

// --- The cache file, written at a cold boot (boot-only: the image it copies is in the boot-only pages)

#pragma code_seg(".xkb")

void WriteKernelModulesCache() {
    const XK_KERNEL_MODULES_HEADER *header = ImageHeader();
    if (!header)
        return;
    // The extension loader's cache before the kernel modules (2026-11-17): it is gone with them.
    io::DeleteFile("\\Device\\Harddisk0\\Partition2\\OXOS\\cache\\oxos_extension_loader.cache");
    HANDLE file;
    // No cache folder (C:\OXOS\cache is made at every cold boot, before this): nothing to write.
    NTSTATUS status = io::CreateFile(CACHE_PATH, GENERIC_WRITE, 0, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &file);
    if (!NT_SUCCESS(status))
        return;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;
    at.QuadPart = 0;
    status = NtWriteFile(file, nullptr, nullptr, nullptr, &iosb, (PVOID)header, sizeof(*header) + header->Size, &at);
    NtClose(file);
}

#pragma code_seg()

// --- Loading and unloading

bool LoadKernelModules() {
    if (Modules)
        return true;
    // A cold boot: the copy in the kernel image, still in its boot-only pages.
    if (memory::BootPagesPresent()) {
        const XK_KERNEL_MODULES_HEADER *header = ImageHeader();
        if (header && IsHeaderValid(header, sizeof(*header) + header->Size))
            return MapImage(header);
        XK_SERIAL("modules: none in the kernel image\n");
        return false;
    }
    ULONG got;
    NTSTATUS status;
    auto cache = (const XK_KERNEL_MODULES_HEADER *)titles::ReadOXOSFile(CACHE_PATH, sizeof(XK_KERNEL_MODULES_HEADER) + XK_KERNEL_MODULES_MAX_BYTES, POOL_TAG,
                                                                         &got, &status);
    bool loaded = false;
    if (cache && IsHeaderValid(cache, got))
        loaded = MapImage(cache);
    else
        XK_SERIAL("modules: no cache for this kernel (%#lx): launching without the kernel modules\n", status);
    if (cache)
        ExFreePool((PVOID)cache);
    return loaded;
}

void UnloadKernelModules() {
    if (Modules) {
        *memory::PdeAddress(XK_KERNEL_MODULES_BASE) = 0;
        ReloadCr3();
    }
    Modules = nullptr;
    if (ImageTable)
        MmFreeContiguousMemory(ImageTable);
    if (ImagePages)
        MmFreeContiguousMemory(ImagePages);
    ImageTable = nullptr;
    ImagePages = nullptr;
}

} // namespace xk::kernel_modules
