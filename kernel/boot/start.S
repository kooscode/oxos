// Open BIOS image: 2BL entry at bank offset 0x1000 (0xFFFC1000), reached in flat 32-bit protected mode from the
// X-codes' overflow trick (retail MCPX ROM) or from our reset code (no ROM: MCPX X2, xemu). The GDT may still point
// into the hidden boot ROM, so ours is loaded first. Then caches/MTRRs as the reference 2BL leaves them, a stack below
// 4 MB, and the C part copied to RAM at 0x400000 (where the reference 2BL lives and the kernel wipes it).

    .section .entry, "ax"
    .code32
    .globl _start
_start:
    // Our own GDT (the table below), then reload every segment register from it: CS by the far jump, DS, ES, SS with
    // the flat data selector, FS and GS with the null one.
    cli
    lgdt gdt_desc
    ljmp $0x08, $1f
1:  movl $0x10, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %ss
    xorl %eax, %eax
    movl %eax, %fs
    movl %eax, %gs

    // The X-codes left the overflow trick's `mov eax, 0xFFFC1000; jmp eax` in RAM at address 0 (also when our reset
    // code ran them): clear it, so physical page 0 starts zeroed as a Microsoft BIOS leaves it (titles' USB library
    // keeps its state there).
    movl %eax, 0
    movl %eax, 4
#if XK_XCODE_ENTRY_SNAPSHOT
    // Debug (spec/oxos_internal/27 §4): the X-code settings that get lost on some cold boots, read before anything
    // else runs, kept in MMX registers until the memory is sized, then stored at physical 0x2340 for the kernel's log.
    movl 0xFD0010B0, %eax
    movd %eax, %mm0
    movl 0xFD0010BC, %eax
    movd %eax, %mm1
    movl 0xFD0010E8, %eax
    movd %eax, %mm2
    movl 0xFD680500, %eax
    movd %eax, %mm3
    movl 0xFD68050C, %eax
    movd %eax, %mm4
    movl 0xFD100210, %eax
    movd %eax, %mm5
    movl 0xFD100200, %eax
    movd %eax, %mm6
    movl 0xFD100204, %eax
    movd %eax, %mm7
    xorl %eax, %eax
#endif
#if XK_MCPX_DUMP
    // MCPX ROM dumper (spec/oxos_internal/12_mcpx_dump.md): entered through the TEA (MCPX 1.1) or RC4 (1.0) attack
    // instead of the overflow trick, the boot ROM is still mapped over the top 512 bytes: copy them to RAM 0x2000 and
    // mark the copy ('MCPX' at 0x2200) for the netboot code, which sends it once the link is up.
    movl $0xFFFFFE00, %esi
    movl $0x2000, %edi
    movl $128, %ecx
    cld
    rep movsl
    movl $0x5850434D, 0x2200
    xorl %eax, %eax
#endif

    // Caches off while the MTRRs change (CR0.CD and NW set, caches flushed), MTRRs off (default type register 0x2FF
    // = 0), then the 8 variable ranges (MSRs 0x200-0x20F, base and mask each) cleared.
    movl %cr0, %eax
    orl $0x60000000, %eax
    movl %eax, %cr0
    wbinvd
    xorl %eax, %eax
    xorl %edx, %edx
    movl $0x2FF, %ecx
    wrmsr
    movl $0x200, %ecx
2:  wrmsr
    incl %ecx
    cmpl $0x210, %ecx
    jne 2b
    // As the reference 2BL leaves them (spec/stock/01 §4): RAM 0-64 MB write-back, the top 512 KB of flash
    // write-protect cached, everything else uncached (default type). On a 128 MB console the kernel widens range 0
    // to 128 MB (memory/init.cpp).
    movl $0x200, %ecx                       // range 0 base: 0, type 6 (write-back)
    movl $0x00000006, %eax
    xorl %edx, %edx
    wrmsr
    incl %ecx                               // range 0 mask: 64 MB, valid
    movl $0xFC000800, %eax
    movl $0x0000000F, %edx
    wrmsr
    incl %ecx                               // range 1 base: 0xFFF80000, type 5 (write-protect)
    movl $0xFFF80005, %eax
    xorl %edx, %edx
    wrmsr
    incl %ecx                               // range 1 mask: 512 KB, valid
    movl $0xFFF80800, %eax
    movl $0x0000000F, %edx
    wrmsr
    movl $0x2FF, %ecx                       // default type: MTRRs on, everything else uncached
    movl $0x800, %eax
    xorl %edx, %edx
    wrmsr
    // The page attribute table as the retail 2BLs set it: PA0 WB, PA1 WC, PA2 UC-, PA3 UC (twice), so a PTE's PWT
    // bit selects write-combining (spec/stock/21 §1).
    movl $0x277, %ecx
    movl $0x00070106, %eax
    movl $0x00070106, %edx
    wrmsr
    // DRAM refresh (spec/oxos_internal/20): every reference X-code table enables it (PFB 0x100210 = 0x80000000) and
    // then reprograms the memory clock, and nothing in a reference boot chain checks it again. On a warm 1.4 console it
    // was found off here in about one cold boot in five, and the unrefreshed cells lost bits within seconds. Before
    // anything is stored in RAM: if bit 31 is clear, set it as the X-codes do, and leave a record for the kernel's log
    // (physical 0x23D8: 'RFS0', then the value found).
    movl $0, 0x23D8
    movl 0xFD100210, %eax
    testl $0x80000000, %eax
    jnz 14f
    movl %eax, 0x23DC
    movl $0x80000000, 0xFD100210
    movl $0x30534652, 0x23D8
14:
    // The DRAM set-up as the retail X-codes finish it (spec/stock/22 §6.3), still with the caches off so that every
    // access reaches the memory, and before anything is stored in RAM (0x100204 sets the DRAM geometry, so it cannot
    // change under live data). The X-codes (XboxDev/cromwell's) leave the larger chips' set-up (PFB 0x100200
    // 0x03070103, 0x100204 0x11448000, host 0x84 128 MB). For each of 8 byte lanes (four at 0x00555548 + 16n, four
    // at 0x04555548 + 16n) write the address and the address + 32 MB: a lane where the second write leaves the first
    // alone has the larger chips (ebx); one that does not keep a value is faulty (ebp, not used further).
    cmpl $0x11448000, 0xFD100204            // X-codes that sized the memory themselves: keep their set-up
    jne 13f
    xorl %ebx, %ebx
    xorl %ebp, %ebp
    movl $0x00555548, %esi
    movl $1, %ecx
5:  movl $0xAAAAAAAA, (%esi)
    movl $0x5A5A5A5A, 0x02000000(%esi)
    cmpl $0x5A5A5A5A, 0x02000000(%esi)
    jne 7f
    cmpl $0xAAAAAAAA, (%esi)
    jne 6f
    orl %ecx, %ebx                          // no alias at +32 MB: the larger chips
6:  movl $0x55555555, (%esi)
    cmpl $0x55555555, (%esi)
    je 8f
7:  orl %ecx, %ebp
8:  addl $0x10, %esi
    shll $1, %ecx
    cmpl $0x10, %ecx
    jne 9f
    movl $0x04555548, %esi
9:  cmpl $0x100, %ecx
    jne 5b
    // No lane large (eight smaller chips, 64 MB): 0x100200 0x03070103 and 0x100204 0x11338000, top of memory 64 MB.
    // Every lane large (128 MB): 0x03070103, the X-codes' 0x11448000, 128 MB. Otherwise, the retail default and the
    // result on a retail 64 MB console (lanes 0-3 large, 4-7 absent): 0x03070003, 0x11448000, 64 MB.
    movl $0x03FFFFFF, %edi
    testl %ebx, %ebx
    jnz 10f
    movl $0x03070103, 0xFD100200
    movl $0x11338000, 0xFD100204
    jmp 11f
10: cmpl $0xFF, %ebx
    jne 12f
    movl $0x03070103, 0xFD100200
    movl $0x07FFFFFF, %edi
    jmp 11f
12: movl $0x03070003, 0xFD100200
11: movl $0x80000084, %eax                  // host bridge register 0x84: top of memory
    movw $0xCF8, %dx
    outl %eax, %dx
    movl %edi, %eax
    movw $0xCFC, %dx
    outl %eax, %dx
13:

    // Caches on (CR0.CD and NW cleared): from here the MTRRs above decide what is cached.
    movl %cr0, %eax
    andl $0x9FFFFFFF, %eax
    movl %eax, %cr0

#if XK_XCODE_ENTRY_SNAPSHOT
    // The entry snapshot taken at _start, now that RAM is sized and safe to write.
    movl $0x53304358, 0x2340                // 'XC0S'
    movd %mm0, 0x2344
    movd %mm1, 0x2348
    movd %mm2, 0x234C
    movd %mm3, 0x2350
    movd %mm4, 0x2354
    movd %mm5, 0x2358
    movd %mm6, 0x235C
    movd %mm7, 0x2360
    emms
#endif
4:
    // A stack just below the C part at 4 MB (it grows down, away from it).
    movl $0x003FFFF0, %esp
    // Copy the C part (.text/.rodata/.data) to RAM and clear its .bss (addresses from bl.ld).
    movl $__ram_load, %esi
    movl $__ram_start, %edi
    movl $__ram_copy_dwords, %ecx
    rep movsl
    movl $__bss_start, %edi
    movl $__bss_dwords, %ecx
    xorl %eax, %eax
    rep stosl
    // The rest of the 2BL in C++ (bl.cpp): it starts the kernel and does not return; if it did, halt.
    call BlMain
3:  hlt
    jmp 3b

    // The GDT: null, flat code and flat data over the whole 4 GB, the same layout as reset.S's.
    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF // 0x08: flat 32-bit code
    .quad 0x00CF92000000FFFF // 0x10: flat data
gdt_desc:
    .word 0x17
    .long gdt
