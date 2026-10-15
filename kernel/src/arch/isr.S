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

// --- Hardware interrupts: push the IRQ number and dispatch in C (KiDispatchHardwareInterrupt).
.macro IRQ n
    .p2align 2
_KiIrq\n:
    pushl $\n
    jmp KiIrqCommon
.endm

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

KiIrqCommon:
    pushal
    cld
    pushl 32(%esp)                  // IRQ number pushed by the stub
    call _KiDispatchHardwareInterrupt
    addl $4, %esp
    popal
    addl $4, %esp
    iretl

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
    .long _KiIrq0, _KiIrq1, _KiIrq2, _KiIrq3, _KiIrq4, _KiIrq5, _KiIrq6, _KiIrq7
    .long _KiIrq8, _KiIrq9, _KiIrq10, _KiIrq11, _KiIrq12, _KiIrq13, _KiIrq14, _KiIrq15
