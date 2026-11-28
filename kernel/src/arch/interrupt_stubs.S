// Interrupt and exception entry stubs. IDT layout (spec/stock/02 §3.3): 0x00-0x1F CPU exceptions, 0x30-0x37 master PIC (IRQ0-7), 0x38-0x3F slave PIC (IRQ8-15),
// 64 vectors total.

    .text

// Unwind information for gdb (DEBUG=1 builds): the two common entry paths describe the interrupted code's registers as a signal frame, so a backtrace from
// a handler goes on into the code the trap or interrupt stopped.
#if XK_DEBUG_INFO
#define CFI(...) __VA_ARGS__
    .cfi_sections .debug_frame
#else
#define CFI(...)
#endif

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
    .p2align 2
_KiTrap7:                           // #NM: lazy FPU switch (spec/stock/07), resumable
    pushal
    cld
    call _KiNpxNotAvailable
    popal
    iretl
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
    .globl _KiTrap45
EXC_NOERR 45                        // int 0x2D: debug service (spec/stock/14)

// Trap frame (KTRAP_FRAME_XK): edi esi ebp esp ebx edx ecx eax | vector error | eip cs eflags. KiDispatchTrap returns when execution resumes with the (possibly
// edited) frame; a context with a different stack pointer is resumed through KiContinueContext instead.
KiTrapCommon:
    CFI(.cfi_startproc simple; .cfi_signal_frame; .cfi_def_cfa %esp, 20; .cfi_offset %eip, -12)
    pushal
    CFI(.cfi_def_cfa_offset 52; .cfi_offset %eax, -24; .cfi_offset %ecx, -28; .cfi_offset %edx, -32; .cfi_offset %ebx, -36)
    CFI(.cfi_offset %ebp, -44; .cfi_offset %esi, -48; .cfi_offset %edi, -52)
    cld
    pushl %esp
    CFI(.cfi_def_cfa_offset 56)
    call _KiDispatchTrap
    addl $4, %esp
    CFI(.cfi_def_cfa_offset 52)
    popal
    CFI(.cfi_def_cfa_offset 20)
    addl $8, %esp
    CFI(.cfi_def_cfa_offset 12)
    iretl
    CFI(.cfi_endproc)

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
    CFI(.cfi_startproc simple; .cfi_signal_frame; .cfi_def_cfa %esp, 16; .cfi_offset %eip, -12)
    pushal
    CFI(.cfi_def_cfa_offset 48; .cfi_offset %eax, -20; .cfi_offset %ecx, -24; .cfi_offset %edx, -28; .cfi_offset %ebx, -32)
    CFI(.cfi_offset %ebp, -40; .cfi_offset %esi, -44; .cfi_offset %edi, -48)
    cld
    pushl 32(%esp)                  // IRQ number pushed by the stub
    CFI(.cfi_def_cfa_offset 52)
    call _KiDispatchHardwareInterrupt
    addl $4, %esp
    CFI(.cfi_def_cfa_offset 48)
    popal
    CFI(.cfi_def_cfa_offset 16)
    addl $4, %esp
    CFI(.cfi_def_cfa_offset 12)
    iretl
    CFI(.cfi_endproc)

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
