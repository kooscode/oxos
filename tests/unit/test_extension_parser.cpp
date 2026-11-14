// PE images for kernel extensions (spec/oxos_internal/34 §8, kernel/src/extensions/extension_parser.cpp), on a small
// DLL the test lays out itself: one section with a relocated pointer, the kernel imports, the descriptor export and the
// relocation block.
#include "kernel_test.h"

#include <cstring>
#include <vector>

#include <extensions/extension_parser.h>

using namespace xk; // the kernel's subsystems as storage::, extensions::, ...

namespace {

constexpr ULONG IMAGE_BASE = 0x10000000, IMAGE_SIZE = 0x2000, HEADERS = 0x400, SECTION_RVA = 0x1000;
constexpr ULONG NT = 0x80, OPTIONAL = NT + 0x18, SECTIONS = OPTIONAL + 0xE0;
constexpr ULONG POINTER_RVA = 0x1000, TARGET_RVA = 0x1010;          // a pointer to TARGET_RVA, relocated
constexpr ULONG IMPORTS_RVA = 0x1100, LOOKUP_RVA = 0x1140, SLOTS_RVA = 0x1160, DLL_NAME_RVA = 0x1180;
constexpr ULONG EXPORTS_RVA = 0x1200, RELOCATIONS_RVA = 0x1300;

void Put32(std::vector<UCHAR> &f, ULONG at, ULONG v) {
    std::memcpy(&f[at], &v, 4);
}

void Put16(std::vector<UCHAR> &f, ULONG at, USHORT v) {
    std::memcpy(&f[at], &v, 2);
}

// File offset of an RVA in the section.
ULONG Raw(ULONG rva) {
    return rva - SECTION_RVA + HEADERS;
}

void SetDirectory(std::vector<UCHAR> &f, ULONG index, ULONG rva, ULONG size) {
    Put32(f, OPTIONAL + 96 + index * 8, rva);
    Put32(f, OPTIONAL + 96 + index * 8 + 4, size);
}

// The test DLL as the linker would write it (image base 0x10000000).
std::vector<UCHAR> TestDll(const char *dll = "xboxkrnl.exe", bool byOrdinal = true) {
    std::vector<UCHAR> f(HEADERS + 0x1000, 0);
    Put16(f, 0, 0x5A4D);
    Put32(f, 0x3C, NT);
    Put32(f, NT, 0x00004550);
    Put16(f, NT + 4, 0x014C);   // i386
    Put16(f, NT + 6, 1);        // one section
    Put16(f, NT + 0x14, 0xE0);  // optional header size
    Put16(f, NT + 0x16, 0x2102); // DLL, 32-bit, executable
    Put16(f, OPTIONAL, 0x010B);
    Put32(f, OPTIONAL + 28, IMAGE_BASE);
    Put32(f, OPTIONAL + 32, 0x1000);
    Put32(f, OPTIONAL + 36, 0x200);
    Put32(f, OPTIONAL + 56, IMAGE_SIZE);
    Put32(f, OPTIONAL + 60, HEADERS);
    Put32(f, OPTIONAL + 92, 16);
    std::memcpy(&f[SECTIONS], ".data", 5);
    Put32(f, SECTIONS + 8, 0x1000);
    Put32(f, SECTIONS + 12, SECTION_RVA);
    Put32(f, SECTIONS + 16, 0x1000);
    Put32(f, SECTIONS + 20, HEADERS);
    // The data: a pointer to TARGET_RVA.
    Put32(f, Raw(POINTER_RVA), IMAGE_BASE + TARGET_RVA);
    // Imports: one descriptor for the DLL, two ordinals (190 NtCreateFile, 8 DbgPrint).
    Put32(f, Raw(IMPORTS_RVA), LOOKUP_RVA);
    Put32(f, Raw(IMPORTS_RVA) + 12, DLL_NAME_RVA);
    Put32(f, Raw(IMPORTS_RVA) + 16, SLOTS_RVA);
    ULONG flag = byOrdinal ? 0x80000000 : 0;
    for (ULONG table : {LOOKUP_RVA, SLOTS_RVA}) {
        Put32(f, Raw(table), flag | 190);
        Put32(f, Raw(table) + 4, flag | 8);
    }
    std::strcpy(reinterpret_cast<char *>(&f[Raw(DLL_NAME_RVA)]), dll);
    SetDirectory(f, 1, IMPORTS_RVA, 40);
    // Exports: XosExtension at TARGET_RVA.
    Put32(f, Raw(EXPORTS_RVA) + 0x14, 1);
    Put32(f, Raw(EXPORTS_RVA) + 0x18, 1);
    Put32(f, Raw(EXPORTS_RVA) + 0x1C, EXPORTS_RVA + 0x40);
    Put32(f, Raw(EXPORTS_RVA) + 0x20, EXPORTS_RVA + 0x50);
    Put32(f, Raw(EXPORTS_RVA) + 0x24, EXPORTS_RVA + 0x60);
    Put32(f, Raw(EXPORTS_RVA) + 0x40, TARGET_RVA);
    Put32(f, Raw(EXPORTS_RVA) + 0x50, EXPORTS_RVA + 0x70);
    std::strcpy(reinterpret_cast<char *>(&f[Raw(EXPORTS_RVA) + 0x70]), "XosExtension");
    SetDirectory(f, 0, EXPORTS_RVA, 0x80);
    // Relocations: one block for the section's page, a HIGHLOW at POINTER_RVA and a padding entry.
    Put32(f, Raw(RELOCATIONS_RVA), SECTION_RVA);
    Put32(f, Raw(RELOCATIONS_RVA) + 4, 12);
    Put16(f, Raw(RELOCATIONS_RVA) + 8, (3 << 12) | (POINTER_RVA - SECTION_RVA));
    SetDirectory(f, 5, RELOCATIONS_RVA, 12);
    return f;
}

XK_PE_ERROR Check(const std::vector<UCHAR> &f) {
    ULONG size = 0;
    return extensions::CheckImage(f.data(), (ULONG)f.size(), &size);
}

ULONG Get32(const std::vector<UCHAR> &image, ULONG at) {
    ULONG v;
    std::memcpy(&v, &image[at], 4);
    return v;
}

// The resolver of the tests: records the ordinals, gives each slot 0x1000 * ordinal; refuses ordinal Refused.
struct Resolver {
    std::vector<ULONG> Ordinals;
    ULONG Refused = 0;
};

bool Resolve(PVOID Context, ULONG Ordinal, ULONG *Slot) {
    auto r = static_cast<Resolver *>(Context);
    if (Ordinal == r->Refused)
        return false;
    r->Ordinals.push_back(Ordinal);
    *Slot = 0x1000 * Ordinal;
    return true;
}

} // namespace

TEST(PeImage, ACheckedDllIsMappedRelocatedAndBound) {
    std::vector<UCHAR> f = TestDll();
    ULONG size = 0;
    ASSERT_EQ(extensions::CheckImage(f.data(), (ULONG)f.size(), &size), XK_PE_ERROR::NONE);
    ASSERT_EQ(size, IMAGE_SIZE);
    std::vector<UCHAR> image(size, 0xCC);
    const ULONG base = 0x80345000;
    ASSERT_EQ(extensions::MapImage(f.data(), (ULONG)f.size(), image.data(), size, base), XK_PE_ERROR::NONE);
    EXPECT_EQ(Get32(image, POINTER_RVA), base + TARGET_RVA);
    EXPECT_EQ(Get32(image, OPTIONAL + 28), base);
    EXPECT_EQ(image[IMAGE_SIZE - 1], 0); // the rest of the image is zero
    Resolver r;
    ASSERT_EQ(extensions::BindImports(image.data(), size, Resolve, &r), XK_PE_ERROR::NONE);
    EXPECT_EQ(r.Ordinals, (std::vector<ULONG>{190, 8}));
    EXPECT_EQ(Get32(image, SLOTS_RVA), 0x1000u * 190);
    EXPECT_EQ(Get32(image, SLOTS_RVA + 4), 0x1000u * 8);
    EXPECT_EQ(extensions::FindExport(image.data(), size, "XosExtension"), TARGET_RVA);
    EXPECT_EQ(extensions::FindExport(image.data(), size, "xosextension"), 0u); // export names keep their case
    EXPECT_EQ(extensions::FindExport(image.data(), size, "Other"), 0u);
}

TEST(PeImage, TheKernelsNameMatchesInAnyCase) {
    std::vector<UCHAR> f = TestDll("XBOXKRNL.EXE");
    std::vector<UCHAR> image(IMAGE_SIZE);
    ASSERT_EQ(extensions::MapImage(f.data(), (ULONG)f.size(), image.data(), IMAGE_SIZE, IMAGE_BASE), XK_PE_ERROR::NONE);
    Resolver r;
    EXPECT_EQ(extensions::BindImports(image.data(), IMAGE_SIZE, Resolve, &r), XK_PE_ERROR::NONE);
}

TEST(PeImage, ImagesTheLoaderCannotUseAreRefused) {
    std::vector<UCHAR> f = TestDll();
    f[0] = 'X';
    EXPECT_EQ(Check(f), XK_PE_ERROR::NOT_PE);
    f = TestDll();
    Put16(f, NT + 4, 0x8664);
    EXPECT_EQ(Check(f), XK_PE_ERROR::NOT_I386_DLL);
    f = TestDll();
    Put16(f, NT + 0x16, 0x0102); // not a DLL
    EXPECT_EQ(Check(f), XK_PE_ERROR::NOT_I386_DLL);
    f = TestDll();
    SetDirectory(f, 5, 0, 0);
    EXPECT_EQ(Check(f), XK_PE_ERROR::NO_RELOCATIONS);
    f = TestDll();
    SetDirectory(f, 9, 0x1400, 0x18);
    EXPECT_EQ(Check(f), XK_PE_ERROR::HAS_TLS);
    f = TestDll();
    Put32(f, SECTIONS + 16, 0x2000); // raw data past the end of the file
    EXPECT_EQ(Check(f), XK_PE_ERROR::BAD_SECTION);
    f = TestDll();
    f.resize(0x100); // the section table is cut off
    EXPECT_EQ(Check(f), XK_PE_ERROR::NOT_PE);
}

TEST(PeImage, ImportsMustBeKernelOrdinals) {
    std::vector<UCHAR> image(IMAGE_SIZE);
    for (auto f : {TestDll("kernel32.dll"), TestDll("xboxkrnl.exe", false)}) {
        ASSERT_EQ(extensions::MapImage(f.data(), (ULONG)f.size(), image.data(), IMAGE_SIZE, IMAGE_BASE),
                  XK_PE_ERROR::NONE);
        Resolver r;
        EXPECT_EQ(extensions::BindImports(image.data(), IMAGE_SIZE, Resolve, &r), XK_PE_ERROR::BAD_IMPORT);
    }
    std::vector<UCHAR> f = TestDll();
    ASSERT_EQ(extensions::MapImage(f.data(), (ULONG)f.size(), image.data(), IMAGE_SIZE, IMAGE_BASE), XK_PE_ERROR::NONE);
    Resolver r;
    r.Refused = 8; // an ordinal the kernel does not have
    EXPECT_EQ(extensions::BindImports(image.data(), IMAGE_SIZE, Resolve, &r), XK_PE_ERROR::BAD_IMPORT);
}

TEST(PeImage, OnlyHighLowRelocationsAreApplied) {
    std::vector<UCHAR> f = TestDll();
    Put16(f, Raw(RELOCATIONS_RVA) + 8, (10 << 12) | 0); // a 64-bit relocation
    std::vector<UCHAR> image(IMAGE_SIZE);
    EXPECT_EQ(extensions::MapImage(f.data(), (ULONG)f.size(), image.data(), IMAGE_SIZE, IMAGE_BASE + 0x1000),
              XK_PE_ERROR::BAD_RELOCATION);
    f = TestDll();
    Put32(f, Raw(RELOCATIONS_RVA) + 4, 0x2000); // a block running past the directory
    EXPECT_EQ(extensions::MapImage(f.data(), (ULONG)f.size(), image.data(), IMAGE_SIZE, IMAGE_BASE + 0x1000),
              XK_PE_ERROR::BAD_RELOCATION);
}
