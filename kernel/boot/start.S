// Open BIOS image: 2BL entry at bank offset 0x1000 (0xFFFC1000), reached in flat 32-bit protected mode from the
// X-codes' overflow trick (MCPX ROM present) or from our reset code (no ROM). The GDT may still point into the
// hidden boot ROM, so ours is loaded first. Then caches/MTRRs as the reference 2BL leaves them, a stack below
// 4 MB, and the C part copied to RAM at 0x400000 (where the reference 2BL lives and the kernel wipes it).

    .section .entry, "ax"
    .code32
    .globl _start
_start:
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

    // The overflow trick left `mov eax, 0xFFFC1000; jmp eax` in RAM at address 0: clear it, so physical page 0
    // starts zeroed as a Microsoft BIOS leaves it (titles' USB library keeps its state there).
    movl %eax, 0
    movl %eax, 4

    // Caches off while the MTRRs change.
    movl %cr0, %eax
    orl $0x60000000, %eax
    movl %eax, %cr0
    wbinvd
    xorl %eax, %eax
    xorl %edx, %edx
    movl $0x2FF, %ecx
    wrmsr
    movl $0x200, %ecx // clear the 8 variable ranges
2:  wrmsr
    incl %ecx
    cmpl $0x210, %ecx
    jne 2b
    // As the reference 2BL leaves them: RAM 0-64 MB write-back, the top 512 KB of flash write-protect cached,
    // everything else uncached (default type).
    movl $0x200, %ecx
    movl $0x00000006, %eax
    xorl %edx, %edx
    wrmsr
    incl %ecx
    movl $0xFC000800, %eax
    movl $0x0000000F, %edx
    wrmsr
    incl %ecx
    movl $0xFFF80005, %eax
    xorl %edx, %edx
    wrmsr
    incl %ecx
    movl $0xFFF80800, %eax
    movl $0x0000000F, %edx
    wrmsr
    movl $0x2FF, %ecx
    movl $0x800, %eax
    xorl %edx, %edx
    wrmsr
    // The page attribute table as the retail 2BLs set it: PA0 WB, PA1 WC, PA2 UC-, PA3 UC (twice), so a PTE's PWT
    // bit selects write-combining (spec/stock/21 §1).
    movl $0x277, %ecx
    movl $0x00070106, %eax
    movl $0x00070106, %edx
    wrmsr
    movl %cr0, %eax
    andl $0x9FFFFFFF, %eax
    movl %eax, %cr0

    // 64 MB consoles: the GPU's memory size (PFB 0x100200 bit 8, as Cromwell for 64 MB) and the host bridge's top of
    // memory for 64 MB, before anything is stored in RAM. The X-codes (XboxDev/cromwell's) always leave the 128 MB
    // set-up; RAM that is not there does not keep a value. PFB 0x100204 stays the X-codes' 0x11448000: the
    // 0x11338000 of spec/stock/22 §6.3 stopped a 1.0 console's kernel in the memory manager (2026-10-22).
    movl $0x2468ACE0, 0x04000FF0
    movl $0x13579BDF, 0x00000FF0
    cmpl $0x2468ACE0, 0x04000FF0
    je 4f                                   // 128 MB: keep it
    andl $0xFFFFFEFF, 0xFD100200            // PFB: bit 8 clear (64 MB)
    movl $0x80000084, %eax                  // host bridge register 0x84: top of memory 64 MB - 1
    movw $0xCF8, %dx
    outl %eax, %dx
    movl $0x03FFFFFF, %eax
    movw $0xCFC, %dx
    outl %eax, %dx
4:
    movl $0x003FFFF0, %esp
    // Copy the C part (.text/.rodata/.data) to RAM and clear its .bss.
    movl $__ram_load, %esi
    movl $__ram_start, %edi
    movl $__ram_copy_dwords, %ecx
    rep movsl
    movl $__bss_start, %edi
    movl $__bss_dwords, %ecx
    xorl %eax, %eax
    rep stosl
    call BlMain
3:  hlt
    jmp 3b

    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF // 0x08: flat 32-bit code
    .quad 0x00CF92000000FFFF // 0x10: flat data
gdt_desc:
    .word 0x17
    .long gdt
