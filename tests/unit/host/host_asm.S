// Host versions of the kernel's context switch and thread start-up (kernel/src/arch/swap.S): the same logic with
// ELF symbol names, the PCR's NT_TIB through KiPcr instead of %fs, and the fake CPU's interrupt flag (HostIf)
// saved in the switch frame where the kernel saves EFLAGS (user mode cannot change the real IF).

#define KTHREAD_STACK_BASE   0x1C
#define KTHREAD_STACK_LIMIT  0x20
#define KTHREAD_KERNEL_STACK 0x24
#define NPX_FRAME_LENGTH     0x210

    .text

// Interrupt delivery: the fault handler pushes the interrupted EIP and continues here.
    .globl host_irq_trampoline
host_irq_trampoline:
    pushfl
    pushal
    cld
    call HostDeliverInterrupts
    popal
    popfl
    ret

// void FASTCALL KiSwapContext(PKTHREAD Old /*ecx*/, PKTHREAD New /*edx*/)
    .globl KiSwapContext
KiSwapContext:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl HostIf
    pushl KiPcr                                  // NT_TIB.ExceptionList
    movl %esp, KTHREAD_KERNEL_STACK(%ecx)

    movl KTHREAD_STACK_BASE(%edx), %eax          // PCR stack bounds: below the NPX save area
    subl $NPX_FRAME_LENGTH, %eax
    movl %eax, KiPcr+4
    movl KTHREAD_STACK_LIMIT(%edx), %eax
    movl %eax, KiPcr+8

    movl KTHREAD_KERNEL_STACK(%edx), %esp
    popl KiPcr
    popl HostIf
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

// First code of every new thread. Stack: SystemRoutine, StartRoutine, StartContext.
    .globl KiThreadStartup
KiThreadStartup:
    xorl %ebp, %ebp
    xorl %ecx, %ecx
    call KfLowerIrql                             // PASSIVE_LEVEL
    popl %eax
    call *%eax                                   // SystemRoutine(StartRoutine, StartContext), stdcall
    call KiThreadReturned
1:  hlt
    jmp 1b

// void KiCallOnStack(void (*fn)(), PVOID stackTop)
    .globl KiCallOnStack
KiCallOnStack:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %esp
    pushl %ebp
    call *%eax
    popl %ebp
    movl %ebp, %esp
    popl %ebp
    ret

    .section .note.GNU-stack,"",@progbits
