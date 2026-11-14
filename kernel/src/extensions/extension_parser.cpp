// PE images for kernel extensions (spec/oxos_internal/34 §8). Offsets are the PE/COFF format's (Microsoft's "PE
// Format" specification): the file header after the "PE\0\0" signature, the 32-bit optional header after it, its
// data directories, then the section table.
#include <xk/extension_parser.h>

namespace {

constexpr USHORT DOS_MAGIC = 0x5A4D;          // "MZ"
constexpr ULONG NT_SIGNATURE = 0x00004550;    // "PE\0\0"
constexpr USHORT MACHINE_I386 = 0x014C;
constexpr USHORT OPTIONAL_MAGIC_PE32 = 0x010B;
constexpr USHORT FILE_RELOCS_STRIPPED = 0x0001, FILE_DLL = 0x2000;
constexpr ULONG DIRECTORY_EXPORT = 0, DIRECTORY_IMPORT = 1, DIRECTORY_RELOCATION = 5, DIRECTORY_TLS = 9;
constexpr ULONG SECTION_HEADER_SIZE = 40, IMPORT_DESCRIPTOR_SIZE = 20;
constexpr USHORT RELOCATION_ABSOLUTE = 0, RELOCATION_HIGHLOW = 3;
constexpr ULONG IMPORT_BY_ORDINAL = 0x80000000;

ULONG Read32(const UCHAR *P) {
    return *reinterpret_cast<const ULONG *>(P);
}

USHORT Read16(const UCHAR *P) {
    return *reinterpret_cast<const USHORT *>(P);
}

// Offset + Length lies within Size, without overflowing.
bool Inside(ULONG Offset, ULONG Length, ULONG Size) {
    return Offset <= Size && Length <= Size - Offset;
}

// The headers of an image (in the file or mapped: the headers are the same in both).
struct HEADERS {
    const UCHAR *Nt;       // the "PE\0\0" signature
    const UCHAR *Optional; // the optional header
    ULONG SectionCount;
    const UCHAR *Sections; // the section table
    ULONG DirectoryCount;
};

// Finds the headers in Data (Size bytes); false when they are not all inside it.
bool FindHeaders(const UCHAR *Data, ULONG Size, HEADERS *H) {
    if (Size < 0x40 || Read16(Data) != DOS_MAGIC)
        return false;
    ULONG nt = Read32(Data + 0x3C);
    if (!Inside(nt, 0x18, Size) || Read32(Data + nt) != NT_SIGNATURE)
        return false;
    USHORT optionalSize = Read16(Data + nt + 0x14);
    if (!Inside(nt + 0x18, optionalSize, Size) || optionalSize < 96)
        return false;
    H->Nt = Data + nt;
    H->Optional = Data + nt + 0x18;
    H->SectionCount = Read16(Data + nt + 6);
    H->DirectoryCount = Read32(H->Optional + 92);
    if (H->DirectoryCount > (optionalSize - 96u) / 8)
        H->DirectoryCount = (optionalSize - 96u) / 8;
    ULONG sections = nt + 0x18 + optionalSize;
    if (!Inside(sections, H->SectionCount * SECTION_HEADER_SIZE, Size))
        return false;
    H->Sections = Data + sections;
    return true;
}

// Data directory Index: its RVA and size (both 0 when the image has none).
void Directory(const HEADERS &H, ULONG Index, ULONG *Rva, ULONG *Length) {
    *Rva = 0;
    *Length = 0;
    if (Index >= H.DirectoryCount)
        return;
    *Rva = Read32(H.Optional + 96 + Index * 8);
    *Length = Read32(H.Optional + 96 + Index * 8 + 4);
}

// A zero-terminated string at Rva of the mapped image, compared with Name ignoring the case of letters.
bool NameIs(const UCHAR *Image, ULONG ImageSize, ULONG Rva, const char *Name, bool AnyCase) {
    for (ULONG i = 0;; i++) {
        if (!Inside(Rva, i + 1, ImageSize))
            return false;
        char c = static_cast<char>(Image[Rva + i]);
        char n = Name[i];
        if (AnyCase) {
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
            n = n >= 'A' && n <= 'Z' ? static_cast<char>(n + 32) : n;
        }
        if (c != n)
            return false;
        if (!c)
            return true;
    }
}

} // namespace

XK_PE_ERROR XkPeCheck(const UCHAR *File, ULONG Size, ULONG *ImageSize) {
    HEADERS h;
    if (!FindHeaders(File, Size, &h))
        return XK_PE_ERROR::NOT_PE;
    USHORT characteristics = Read16(h.Nt + 0x16);
    if (Read16(h.Nt + 4) != MACHINE_I386 || !(characteristics & FILE_DLL) || Read16(h.Optional) != OPTIONAL_MAGIC_PE32)
        return XK_PE_ERROR::NOT_I386_DLL;
    ULONG rva, length;
    Directory(h, DIRECTORY_RELOCATION, &rva, &length);
    if ((characteristics & FILE_RELOCS_STRIPPED) || !rva || !length)
        return XK_PE_ERROR::NO_RELOCATIONS;
    Directory(h, DIRECTORY_TLS, &rva, &length);
    if (rva)
        return XK_PE_ERROR::HAS_TLS;
    ULONG imageSize = Read32(h.Optional + 56), headerSize = Read32(h.Optional + 60);
    if (headerSize > Size || headerSize > imageSize)
        return XK_PE_ERROR::NOT_PE;
    for (ULONG i = 0; i < h.SectionCount; i++) {
        const UCHAR *s = h.Sections + i * SECTION_HEADER_SIZE;
        ULONG virtualSize = Read32(s + 8), address = Read32(s + 12), rawSize = Read32(s + 16), raw = Read32(s + 20);
        ULONG span = virtualSize > rawSize ? virtualSize : rawSize;
        if (!Inside(address, span, imageSize) || (rawSize && !Inside(raw, rawSize, Size)))
            return XK_PE_ERROR::BAD_SECTION;
    }
    *ImageSize = imageSize;
    return XK_PE_ERROR::NONE;
}

XK_PE_ERROR XkPeMap(const UCHAR *File, ULONG Size, UCHAR *Image, ULONG ImageSize, ULONG Base) {
    HEADERS h;
    if (!FindHeaders(File, Size, &h))
        return XK_PE_ERROR::NOT_PE;
    for (ULONG i = 0; i < ImageSize; i++)
        Image[i] = 0;
    ULONG headerSize = Read32(h.Optional + 60);
    for (ULONG i = 0; i < headerSize; i++)
        Image[i] = File[i];
    for (ULONG i = 0; i < h.SectionCount; i++) {
        const UCHAR *s = h.Sections + i * SECTION_HEADER_SIZE;
        ULONG virtualSize = Read32(s + 8), address = Read32(s + 12), rawSize = Read32(s + 16), raw = Read32(s + 20);
        ULONG copy = rawSize < virtualSize || !virtualSize ? rawSize : virtualSize;
        for (ULONG b = 0; b < copy; b++)
            Image[address + b] = File[raw + b];
    }
    // Relocations: blocks of {page RVA, block size, entries}; each HIGHLOW entry is a 32-bit address to move.
    ULONG delta = Base - Read32(h.Optional + 28);
    ULONG rva, length;
    Directory(h, DIRECTORY_RELOCATION, &rva, &length);
    if (!Inside(rva, length, ImageSize))
        return XK_PE_ERROR::BAD_RELOCATION;
    for (ULONG at = rva; at + 8 <= rva + length;) {
        ULONG page = Read32(Image + at), blockSize = Read32(Image + at + 4);
        if (blockSize < 8 || !Inside(at, blockSize, rva + length))
            return XK_PE_ERROR::BAD_RELOCATION;
        for (ULONG e = at + 8; e + 2 <= at + blockSize; e += 2) {
            USHORT entry = Read16(Image + e);
            USHORT type = entry >> 12;
            ULONG target = page + (entry & 0xFFF);
            if (type == RELOCATION_ABSOLUTE)
                continue;
            if (type != RELOCATION_HIGHLOW || !Inside(target, 4, ImageSize))
                return XK_PE_ERROR::BAD_RELOCATION;
            *reinterpret_cast<ULONG *>(Image + target) += delta;
        }
        at += blockSize;
    }
    // The image's headers now say where it runs.
    *reinterpret_cast<ULONG *>(Image + (h.Optional - File) + 28) = Base;
    return XK_PE_ERROR::NONE;
}

XK_PE_ERROR XkPeBindImports(UCHAR *Image, ULONG ImageSize, XK_PE_IMPORT_RESOLVER Resolve, PVOID Context) {
    HEADERS h;
    if (!FindHeaders(Image, ImageSize, &h))
        return XK_PE_ERROR::NOT_PE;
    ULONG rva, length;
    Directory(h, DIRECTORY_IMPORT, &rva, &length);
    if (!rva)
        return XK_PE_ERROR::NONE;
    for (ULONG at = rva;; at += IMPORT_DESCRIPTOR_SIZE) {
        if (!Inside(at, IMPORT_DESCRIPTOR_SIZE, ImageSize))
            return XK_PE_ERROR::BAD_IMPORT;
        ULONG names = Read32(Image + at), name = Read32(Image + at + 12), slots = Read32(Image + at + 16);
        if (!name && !slots)
            return XK_PE_ERROR::NONE;
        if (!NameIs(Image, ImageSize, name, "xboxkrnl.exe", true))
            return XK_PE_ERROR::BAD_IMPORT;
        ULONG lookup = names ? names : slots;
        for (ULONG i = 0;; i++) {
            if (!Inside(lookup + i * 4, 4, ImageSize) || !Inside(slots + i * 4, 4, ImageSize))
                return XK_PE_ERROR::BAD_IMPORT;
            ULONG entry = Read32(Image + lookup + i * 4);
            if (!entry)
                break;
            auto slot = reinterpret_cast<ULONG *>(Image + slots + i * 4);
            if (!(entry & IMPORT_BY_ORDINAL) || !Resolve(Context, entry & 0xFFFF, slot))
                return XK_PE_ERROR::BAD_IMPORT;
        }
    }
}

ULONG XkPeFindExport(const UCHAR *Image, ULONG ImageSize, const char *Name) {
    HEADERS h;
    if (!FindHeaders(Image, ImageSize, &h))
        return 0;
    ULONG rva, length;
    Directory(h, DIRECTORY_EXPORT, &rva, &length);
    if (!rva || !Inside(rva, 0x28, ImageSize))
        return 0;
    ULONG functionCount = Read32(Image + rva + 0x14), nameCount = Read32(Image + rva + 0x18);
    ULONG functions = Read32(Image + rva + 0x1C), names = Read32(Image + rva + 0x20);
    ULONG ordinals = Read32(Image + rva + 0x24);
    if (!Inside(names, nameCount * 4, ImageSize) || !Inside(ordinals, nameCount * 2, ImageSize) ||
        !Inside(functions, functionCount * 4, ImageSize))
        return 0;
    for (ULONG i = 0; i < nameCount; i++) {
        if (!NameIs(Image, ImageSize, Read32(Image + names + i * 4), Name, false))
            continue;
        USHORT index = Read16(Image + ordinals + i * 2);
        if (index >= functionCount)
            return 0;
        ULONG found = Read32(Image + functions + index * 4);
        return Inside(found, 1, ImageSize) ? found : 0;
    }
    return 0;
}
