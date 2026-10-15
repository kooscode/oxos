// Interrupt and exception entry stubs. IDT layout (spec/02 §3.3): 0x00-0x1F CPU exceptions,
// 0x30-0x37 master PIC (IRQ0-7), 0x38-0x3F slave PIC (IRQ8-15), 64 vectors total.

    .text

// --- CPU exceptions: normalize to [vector, error code, eip, cs, eflags] and report.
.macro EXC_NOERR vec
    .p2align 2
_KiTrap\vec:
    pushl $0
    pushl $\vec
    jmp KiTrapCommon
.endm

.macro EXC_ERR vec
    .p2align 2
_KiTrap\vec:
    pushl $\vec
    jmp KiTrapCommon
.endm

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
EXC_NOERR 20
EXC_NOERR 21
EXC_NOERR 22
EXC_NOERR 23
EXC_NOERR 24
EXC_NOERR 25
EXC_NOERR 26
EXC_NOERR 27
EXC_NOERR 28
EXC_NOERR 29
EXC_NOERR 30
EXC_NOERR 31

KiTrapCommon:
    cld
    call _KiUnexpectedException     // (vector, errorCode, eip) are already on the stack
1:  cli
    hlt
    jmp 1b

// --- Hardware interrupts.
    .p2align 2
    .globl _KiClockIsr
_KiClockIsr:                        // IRQ0, vector 0x30
    pushal
    cld
    call _KiClockInterrupt
    movb $0x20, %al
    outb %al, $0x20
    popal
    iretl

.macro IRQ_SPURIOUS_MASTER irq
    .p2align 2
_KiIrq\irq:
    pushl %eax
    movb $0x20, %al
    outb %al, $0x20
    popl %eax
    iretl
.endm

.macro IRQ_SPURIOUS_SLAVE irq
    .p2align 2
_KiIrq\irq:
    pushl %eax
    movb $0x20, %al
    outb %al, $0xA0
    outb %al, $0x20
    popl %eax
    iretl
.endm

IRQ_SPURIOUS_MASTER 1
IRQ_SPURIOUS_MASTER 2
IRQ_SPURIOUS_MASTER 3
IRQ_SPURIOUS_MASTER 4
IRQ_SPURIOUS_MASTER 5
IRQ_SPURIOUS_MASTER 6
IRQ_SPURIOUS_MASTER 7
IRQ_SPURIOUS_SLAVE 8
IRQ_SPURIOUS_SLAVE 9
IRQ_SPURIOUS_SLAVE 10
IRQ_SPURIOUS_SLAVE 11
IRQ_SPURIOUS_SLAVE 12
IRQ_SPURIOUS_SLAVE 13
IRQ_SPURIOUS_SLAVE 14
IRQ_SPURIOUS_SLAVE 15

    .p2align 2
    .globl _KiIgnoreInterrupt
_KiIgnoreInterrupt:
    iretl

// --- Handler tables consumed by KiInitializeIdt().
    .section .rdata, "dr"
    .p2align 2
    .globl _KiTrapStubs
_KiTrapStubs:
    .long _KiTrap0, _KiTrap1, _KiTrap2, _KiTrap3, _KiTrap4, _KiTrap5, _KiTrap6, _KiTrap7
    .long _KiTrap8, _KiTrap9, _KiTrap10, _KiTrap11, _KiTrap12, _KiTrap13, _KiTrap14, _KiTrap15
    .long _KiTrap16, _KiTrap17, _KiTrap18, _KiTrap19, _KiTrap20, _KiTrap21, _KiTrap22, _KiTrap23
    .long _KiTrap24, _KiTrap25, _KiTrap26, _KiTrap27, _KiTrap28, _KiTrap29, _KiTrap30, _KiTrap31

    .globl _KiIrqStubs
_KiIrqStubs:
    .long _KiClockIsr, _KiIrq1, _KiIrq2, _KiIrq3, _KiIrq4, _KiIrq5, _KiIrq6, _KiIrq7
    .long _KiIrq8, _KiIrq9, _KiIrq10, _KiIrq11, _KiIrq12, _KiIrq13, _KiIrq14, _KiIrq15
