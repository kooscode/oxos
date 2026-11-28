// The title patcher module (spec/oxos_internal/25 §3, 43): reads C:\OXOS\oxos_title_patches.ini once per title load and turns this title's sections into
// patch records (shared/ini_file/title_patcher_ini.cpp), checked against the title's sections. The kernel keeps the records for the title and writes them
// into each section whenever it is read from the image (titles/title_loader.cpp, ApplyPatches: also later, through XeLoadSection, so it stays resident).
#include "modules.h"
#include <diagnostics/diagnostics.h>
#include <ini_file/title_patcher_ini.h>
#include <memory/memory.h>
#include <runtime/c_library.h>
#include <titles/titles.h>

namespace xk::modules {

namespace {

constexpr char PATCH_FILE[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_title_patches.ini";
constexpr ULONG MAX_PATCH_FILE = 0x100000;
constexpr ULONG FILE_POOL_TAG = 'tPeX';
constexpr ULONG RECORD_POOL_TAG = 'rPeX';

// The patch, and the match bytes it is checked against, inside one section (so both are loaded when the patch is applied).
bool IsInsideOneSection(const INI_PATCH_RECORD *Record, const titles::XBE_SECTION_HEADER *Sections, ULONG Count) {
    for (ULONG i = 0; i < Count; i++) {
        ULONG va = Sections[i].VirtualAddress, size = Sections[i].VirtualSize;
        if (Record->Address >= va && Record->Address - va + oxos::ini_file::PatchSpan(Record) <= size &&
            (!Record->Check || (Record->Check >= va && Record->Check - va + Record->OriginalLength <= size)))
            return true;
    }
    return false;
}

} // namespace

void BuildTitlePatches(ULONG TitleId, ULONG Version, const titles::XBE_SECTION_HEADER *Sections, ULONG Count, PUCHAR *Records, ULONG *Size) {
    *Records = nullptr;
    *Size = 0;
    ULONG got;
    NTSTATUS status;
    char *text = titles::ReadOXOSFile(PATCH_FILE, MAX_PATCH_FILE, FILE_POOL_TAG, &got, &status);
    if (!text) {
        if (status != STATUS_OBJECT_NAME_NOT_FOUND && status != STATUS_OBJECT_PATH_NOT_FOUND)
            XK_SERIAL("patches: oxos_title_patches.ini not read: %#lx\n", status);
        return; // no file: no patches
    }
    // A record is never more than twice its line ("patch=1:00" makes 12 bytes, "name=" 8).
    ULONG room = 2 * got + 16;
    auto out = (PUCHAR)ExAllocatePoolWithTag(room, RECORD_POOL_TAG);
    INI_PATCH_COUNTS counts = {};
    ULONG used = out ? oxos::ini_file::ParseTitlePatches(text, got, TitleId, Version, out, room, &counts) : 0;
    ExFreePool(text);
    if (counts.Sections || counts.Errors) {
        XK_SERIAL("patches: title %08lx version %lu: %u sections, %u patches, %u lines not understood\n", TitleId, Version, counts.Sections, counts.Patches,
                  counts.Errors);
    }
    if (!used) {
        if (out)
            ExFreePool(out);
        return;
    }
    for (ULONG at = 0; at < used;) {
        auto r = (INI_PATCH_RECORD *)(out + at);
        at += oxos::ini_file::PatchRecordSize(r);
        if (r->Kind == INI_PATCH_NAME) {
            CHAR line[INI_PATCH_MAX_NAME + 1];
            memcpy(line, r + 1, r->Length);
            line[r->Length] = 0;
            XK_SERIAL("patches: %s\n", line);
            continue;
        }
        if (!IsInsideOneSection(r, Sections, Count)) {
            XK_SERIAL("patches: %#lx (%lu bytes) is not inside one section of the title%s: skipped\n", r->Address, oxos::ini_file::PatchSpan(r),
                      r->Check ? " with its match bytes" : "");
            r->Kind = 0xFFFF;
        }
    }
    *Records = out;
    *Size = used;
}

} // namespace xk::modules
