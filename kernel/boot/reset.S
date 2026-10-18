// Open BIOS image, the top 512 bytes of each 256 KB bank (0xFFFFFE00-0xFFFFFFFF): our own reset code for when no
// MCPX boot ROM is present (xemu without a boot-ROM file). On a console the MCPX ROM hides this range, runs the
// X-codes itself and reaches the 2BL through the overflow trick, so this code never runs there (spec/stock/01 §5).
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

    // X-code interpreter. %esi = next X-code, %edi = accumulator; each X-code is {u8 op, u32 a, u32 b}.
    movl $(BANK + 0x80), %esi
    xorl %edi, %edi
next:
    movzbl (%esi), %ebp
    movl 1(%esi), %ebx
    movl 5(%esi), %ecx
    addl $9, %esi
    cmpl $0x07, %ebp // "use result": run opcode a with operands (b, accumulator)
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
    jmp next // unknown opcodes are skipped

op_peek:
    movl (%ebx), %edi
    jmp next
op_poke:
    movl %ecx, (%ebx)
    jmp next
op_pciout:
    movw $0xCF8, %dx
    movl %ebx, %eax
    outl %eax, %dx
    movw $0xCFC, %dx
    movl %ecx, %eax
    outl %eax, %dx
    jmp next
op_pciin:
    movw $0xCF8, %dx
    movl %ebx, %eax
    outl %eax, %dx
    movw $0xCFC, %dx
    inl %dx, %eax
    movl %eax, %edi
    jmp next
op_andor:
    andl %ebx, %edi
    orl %ecx, %edi
    jmp next
op_jne:
    cmpl %ebx, %edi
    je next
op_jmp:
    addl %ecx, %esi
    jmp next
op_outb:
    movw %bx, %dx
    movb %cl, %al
    outb %al, %dx
    jmp next
op_inb:
    movw %bx, %dx
    inb %dx, %al
    movzbl %al, %edi
    jmp next

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
