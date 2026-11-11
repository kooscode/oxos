// Open BIOS image, the top 512 bytes of each 256 KB bank (0xFFFFFE00-0xFFFFFFFF): our own reset code for when no
// MCPX boot ROM is present: debug kits with the MCPX X2 (the CPU starts in flash, 2026-11-10) and xemu without a
// boot-ROM file. On a retail console the MCPX ROM covers this range, runs the X-codes itself and reaches the 2BL
// through the overflow trick, so this code never runs there (spec/stock/01 §5, spec/oxos_internal/27 §5).
//
// It does what the boot ROM does for us: real mode -> flat 32-bit protected mode, run the X-code table at bank
// offset 0x80 (the same table the ROM would run), then jump to the 2BL at 0xFFFC1000.

#define BANK 0xFFFC0000
#define STUB 0xFFFFFE00
#define ABS(label) (STUB + (label - stub_start))

    .section .reset, "ax"
    .code16
stub_start:
    cli
    cld
    // At reset CS = 0xF000 with base 0xFFFF0000, so offsets in this segment are (address & 0xFFFF).
    lgdtl %cs:(0xFE00 + (gdt_desc - stub_start))
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    .byte 0x66, 0xEA // ljmpl $0x08, $pm32
    .long ABS(pm32)
    .word 0x08

    .code32
pm32:
    movl $0x10, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %ss
    xorl %eax, %eax
    movl %eax, %fs
    movl %eax, %gs

    // X-code interpreter (spec/oxos_internal/27 §5.3). %esi = next X-code, %edi = accumulator. Each X-code is
    // 9 bytes, {u8 op, u32 a, u32 b}, with Cromwell's op numbers. The table starts at bank offset 0x80, where the
    // MCPX ROM reads it too.
    movl $(BANK + 0x80), %esi
    xorl %edi, %edi
next:
    movzbl (%esi), %ebp
    movl 1(%esi), %ebx
    movl 5(%esi), %ecx
    addl $9, %esi // before the op runs, so jumps are relative to the next X-code
    cmpl $0x07, %ebp // 0x07 "use result": run op a (all 32 bits) with operands (b, accumulator)
    jne 1f
    movl %ebx, %ebp
    movl %ecx, %ebx
    movl %edi, %ecx
1:  cmpl $0x02, %ebp
    je op_peek
    cmpl $0x03, %ebp
    je op_poke
    cmpl $0x04, %ebp
    je op_pciout
    cmpl $0x05, %ebp
    je op_pciin
    cmpl $0x06, %ebp
    je op_andor
    cmpl $0x08, %ebp
    je op_jne
    cmpl $0x09, %ebp
    je op_jmp
    cmpl $0x11, %ebp
    je op_outb
    cmpl $0x12, %ebp
    je op_inb
    cmpl $0xEE, %ebp
    je done
    jmp next // any other op (0x10 included) does nothing

// 0x02 read: accumulator = 32-bit read of address a (no address mask, unlike the ROM).
op_peek:
    movl (%ebx), %edi
    jmp next
// 0x03 write: 32-bit write of b to address a.
op_poke:
    movl %ecx, (%ebx)
    jmp next
// 0x04 PCI write: configuration address a to 0xCF8, then b to 0xCFC (no guard on 0x80000880, unlike the ROM).
op_pciout:
    movw $0xCF8, %dx
    movl %ebx, %eax
    outl %eax, %dx
    movw $0xCFC, %dx
    movl %ecx, %eax
    outl %eax, %dx
    jmp next
// 0x05 PCI read: configuration address a to 0xCF8, then accumulator = 32-bit read of 0xCFC.
op_pciin:
    movw $0xCF8, %dx
    movl %ebx, %eax
    outl %eax, %dx
    movw $0xCFC, %dx
    inl %dx, %eax
    movl %eax, %edi
    jmp next
// 0x06 AND-OR: accumulator = (accumulator AND a) OR b.
op_andor:
    andl %ebx, %edi
    orl %ecx, %edi
    jmp next
// 0x08 jump if not equal: when accumulator != a, go on into 0x09 (move the pointer by b); else the next X-code.
op_jne:
    cmpl %ebx, %edi
    je next
// 0x09 jump: move the pointer by b (signed).
op_jmp:
    addl %ecx, %esi
    jmp next
// 0x11 I/O write: low byte of b to port a.
op_outb:
    movw %bx, %dx
    movb %cl, %al
    outb %al, %dx
    jmp next
// 0x12 I/O read: accumulator = byte read from port a, zero-extended.
op_inb:
    movw %bx, %dx
    inb %dx, %al
    movzbl %al, %edi
    jmp next

// 0xEE exit: jump to the 2BL at bank offset 0x1000 (start.S). Operand a is not used; the caches stay off.
done:
    movl $0xFFFC1000, %eax
    jmp *%eax

    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF // 0x08: flat 32-bit code
    .quad 0x00CF92000000FFFF // 0x10: flat data
gdt_desc:
    .word 0x17
    .long ABS(gdt)

    .org 0x1F0 // reset vector, 0xFFFFFFF0 (still real mode)
    .code16
    jmp stub_start
    .org 0x200
