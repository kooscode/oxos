// Title patches from a file (spec/oxos_internal/25 §3): the parser. It makes no kernel calls, so the host unit tests run it unchanged
// (tests/unit/test_title_patcher_ini.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

// The parser's output, for one title: records one after another, each a header and its data, padded to 4 bytes. A name record (Kind 1) holds a section's `name
// =` text in Length bytes. A patch record (Kind 0) holds the bytes that must be present (OriginalLength: the line's CURRENT_BYTES), then the bytes written at
// Address when they are (Length: NEW_BYTES). They must be at Address, or, for a blind patch, at Check: the section's `match =` line.
struct INI_PATCH_RECORD {
    uint32_t Address;
    uint8_t Length;
    uint8_t OriginalLength;
    uint16_t Kind;
    uint32_t Check; // 0: the current bytes are at Address; else where the section's match bytes are
    // uint8_t Original[OriginalLength], uint8_t Data[Length], then padding to a multiple of 4
};
constexpr uint16_t INI_PATCH_BYTES = 0, INI_PATCH_NAME = 1;
constexpr uint32_t INI_PATCH_MAX_BYTES = 255, INI_PATCH_MAX_NAME = 80; // bytes on each side of a patch; a name

struct INI_PATCH_COUNTS {
    uint32_t Sections; // sections that matched the title
    uint32_t Patches;  // patch records written
    uint32_t Errors;   // lines that could not be read (in any section) or did not fit in the output
};

namespace oxos::ini_file {

// Reads the patch file text and writes the records of every section that names TitleId, without a version or with Version. Returns the number of bytes of
// records written.
uint32_t ParseTitlePatches(const char *Text, uint32_t Length, uint32_t TitleId, uint32_t Version, uint8_t *Out, uint32_t OutSize, INI_PATCH_COUNTS *Counts);

inline uint32_t PatchRecordSize(const INI_PATCH_RECORD *r) {
    return (sizeof(INI_PATCH_RECORD) + r->OriginalLength + r->Length + 3) & ~3u;
}

inline const uint8_t *PatchOriginal(const INI_PATCH_RECORD *r) {
    return (const uint8_t *)(r + 1);
}

inline const uint8_t *PatchData(const INI_PATCH_RECORD *r) {
    return (const uint8_t *)(r + 1) + r->OriginalLength;
}

// The bytes a patch touches at Address: it writes Length bytes there, and compares OriginalLength bytes there too unless they are checked at Check.
inline uint32_t PatchSpan(const INI_PATCH_RECORD *r) {
    return !r->Check && r->OriginalLength > r->Length ? r->OriginalLength : r->Length;
}

} // namespace oxos::ini_file
