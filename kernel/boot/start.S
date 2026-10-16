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
    movl %cr0, %eax
    andl $0x9FFFFFFF, %eax
    movl %eax, %cr0

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
