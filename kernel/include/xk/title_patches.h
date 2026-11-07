// Title patches from a file (spec/oxos_internal/25 §3): the parser. It makes no kernel calls, so the host unit tests
// run it unchanged (tests/unit/test_patches.cpp).
#pragma once
#include <xk/types.h>

// The parser's output, for one title: records one after another, each a header and its data, padded to 4 bytes.
// A name record (Kind 1) holds a section's `name =` text in Length bytes. A patch record (Kind 0) holds the bytes
// that must be present (OriginalLength: the line's CURRENT_BYTES), then the bytes written at Address when they are
// (Length: NEW_BYTES). They must be at Address, or, for a blind patch, at Check: the section's `match =` line.
struct XE_PATCH_RECORD {
    ULONG Address;
    UCHAR Length;
    UCHAR OriginalLength;
    USHORT Kind;
    ULONG Check; // 0: the current bytes are at Address; else where the section's match bytes are
    // UCHAR Original[OriginalLength], UCHAR Data[Length], then padding to a multiple of 4
};
constexpr USHORT XE_PATCH_BYTES = 0, XE_PATCH_NAME = 1;
constexpr ULONG XE_PATCH_MAX_BYTES = 255, XE_PATCH_MAX_NAME = 80; // bytes on each side of a patch; a name

struct XE_PATCH_COUNTS {
    ULONG Sections; // sections that matched the title
    ULONG Patches;  // patch records written
    ULONG Errors;   // lines that could not be read (in any section) or did not fit in the output
};

// Reads the patch file text and writes the records of every section that names TitleId, without a version or with
// Version. Returns the number of bytes of records written.
ULONG XePatchParse(const char *Text, ULONG Length, ULONG TitleId, ULONG Version, PUCHAR Out, ULONG OutSize,
                   XE_PATCH_COUNTS *Counts);

inline ULONG XePatchRecordSize(const XE_PATCH_RECORD *r) {
    return (sizeof(XE_PATCH_RECORD) + r->OriginalLength + r->Length + 3) & ~3u;
}

inline const UCHAR *XePatchOriginal(const XE_PATCH_RECORD *r) {
    return (const UCHAR *)(r + 1);
}

inline const UCHAR *XePatchData(const XE_PATCH_RECORD *r) {
    return (const UCHAR *)(r + 1) + r->OriginalLength;
}

// The bytes a patch touches at Address: it writes Length bytes there, and compares OriginalLength bytes there too
// unless they are checked at Check.
inline ULONG XePatchSpan(const XE_PATCH_RECORD *r) {
    return !r->Check && r->OriginalLength > r->Length ? r->OriginalLength : r->Length;
}
