// Open BIOS image: the 2BL's C part, running from RAM at 0x400000 (spec/stock/01 §5). It does what the reference 2BL
// does for the kernel, as observed at the kernel's entry point:
//   - the kernel image, decompressed to physical 0x10000 (it runs at 0x80010000);
//   - a page directory at 0xF000: 0-256 MB identity-mapped and again at 0x80000000 with 4 MB pages, entry 0x300
//     mapping the directory itself, the NV2A registers (0xFD000000, 16 MB) uncached, the top 4 MB (flash);
//   - CR4 = PSE | OSFXSR | OSXMMEXCPT, CR3 = 0xF000, CR0 = PG | WP | NE | ET | PE;
//   - KernelEntry(arg0, keys) called with cdecl. arg0 = 'OXOS' marks our key block (spec/stock/01 §1): no EEPROM or
//     certificate key, but the EEPROM HMAC states taken from XboxDev/cromwell at build time.
#include <xk/inflate.h>

using xk_inflate::Inflate;
using xk_inflate::Inflater;
typedef unsigned char UCHAR;
typedef unsigned long ULONG;

// Written by tools/mkbios.py at bank offset 0x6000.
struct XK_BOOT_PAYLOAD {
    ULONG Magic;                      // 'OXKP'
    ULONG KernelOffset, KernelPacked; // raw deflate stream, bank-relative
    ULONG KernelSize;                 // the flat kernel image
    ULONG KeysOffset, KeysSize;       // the key block handed to the kernel
    ULONG SplashOffset, SplashSize;   // the boot splash copy in this bank (0, 0 if none; read by the kernel)
};

constexpr ULONG BANK = 0xFFFC0000, PAYLOAD = BANK + 0x6000, PAYLOAD_MAGIC = 0x504B584F, BOOT_MAGIC = 0x534F584F;
constexpr ULONG KERNEL_PHYS = 0x10000, PAGE_DIRECTORY = 0xF000;

static UCHAR BlKeys[1024];

extern "C" void *memcpy(void *d, const void *s, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        ((UCHAR *)d)[i] = ((const UCHAR *)s)[i];
    return d;
}

extern "C" void *memset(void *d, int c, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        ((UCHAR *)d)[i] = (UCHAR)c;
    return d;
}

static void BlPost(UCHAR code) { __asm__ volatile("outb %0, $0x80" ::"a"(code)); }

[[noreturn]] static void BlFail(UCHAR code) {
    BlPost(code);
    for (;;)
        __asm__ volatile("cli; hlt");
}

extern "C" [[noreturn]] void BlMain() {
    BlPost(0x20);
    auto payload = (const volatile XK_BOOT_PAYLOAD *)PAYLOAD;
    if (payload->Magic != PAYLOAD_MAGIC || payload->KeysSize > sizeof(BlKeys))
        BlFail(0xE0);

    Inflater s = {(const UCHAR *)(BANK + payload->KernelOffset), payload->KernelPacked, 0, 0, 0,
                  (UCHAR *)KERNEL_PHYS, payload->KernelSize, 0, false};
    if (!Inflate(s) || s.OutPos != payload->KernelSize)
        BlFail(0xE1);
    BlPost(0x21);
    memcpy(BlKeys, (const void *)(BANK + payload->KeysOffset), payload->KeysSize);

    auto pd = (volatile ULONG *)PAGE_DIRECTORY;
    for (ULONG i = 0; i < 1024; i++)
        pd[i] = 0;
    for (ULONG i = 0; i < 0x40; i++) {
        pd[i] = (i << 22) | 0xE3;         // present, writable, accessed, dirty, 4 MB
        pd[0x200 + i] = (i << 22) | 0xE3; // the same at 0x80000000
    }
    pd[0x300] = PAGE_DIRECTORY | 0x63;
    for (ULONG i = 0; i < 4; i++)
        pd[0x3F4 + i] = (0xFD000000 + (i << 22)) | 0xFB; // + write-through, cache-disabled
    pd[0x3FF] = 0xFFC00000 | 0xE3;

    auto image = (const UCHAR *)KERNEL_PHYS;
    ULONG nt = *(const ULONG *)(image + 0x3C);
    ULONG entry = *(const ULONG *)(image + nt + 0x34) + *(const ULONG *)(image + nt + 0x28); // ImageBase + entry RVA
    BlPost(0x22);
    __asm__ volatile("fninit\n\t"
                     "movl %%cr4, %%eax\n\t"
                     "orl $0x610, %%eax\n\t"
                     "movl %%eax, %%cr4\n\t"
                     "movl %0, %%eax\n\t"
                     "movl %%eax, %%cr3\n\t"
                     "movl $0x80010031, %%eax\n\t"
                     "movl %%eax, %%cr0\n\t"
                     "pushl %1\n\t"
                     "pushl %2\n\t"
                     "call *%3\n\t"
                     :
                     : "i"(PAGE_DIRECTORY), "r"(BlKeys), "i"(BOOT_MAGIC), "r"(entry)
                     : "eax", "memory");
    BlFail(0xE2);
}
