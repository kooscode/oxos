// Context switch, thread start-up and stack switching (spec/07 "Scheduling").
//
// Switch frame, from KernelStack upwards (all threads, including new ones built by
// KiInitializeContextThread):  ExceptionList, EFLAGS, edi, esi, ebx, ebp, return address.

#define KTHREAD_STACK_BASE   0x1C
#define KTHREAD_STACK_LIMIT  0x20
#define KTHREAD_KERNEL_STACK 0x24
#define NPX_FRAME_LENGTH     0x210

    .text

// void FASTCALL KiSwapContext(PKTHREAD Old /*ecx*/, PKTHREAD New /*edx*/)
// Called at DISPATCH_LEVEL. Bookkeeping (states, PRCB, CR0.TS) is done by the caller.
    .globl @KiSwapContext@8
@KiSwapContext@8:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushfl
    pushl %fs:0                                  // NT_TIB.ExceptionList
    movl %esp, KTHREAD_KERNEL_STACK(%ecx)

    movl KTHREAD_STACK_BASE(%edx), %eax          // PCR stack bounds: below the NPX save area
    subl $NPX_FRAME_LENGTH, %eax
    movl %eax, %fs:4
    movl KTHREAD_STACK_LIMIT(%edx), %eax
    movl %eax, %fs:8

    movl KTHREAD_KERNEL_STACK(%edx), %esp
    popl %fs:0
    popfl
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

// First code of every new thread. Stack: SystemRoutine, StartRoutine, StartContext.
    .globl _KiThreadStartup
_KiThreadStartup:
    xorl %ebp, %ebp
    xorl %ecx, %ecx
    call @KfLowerIrql@4                          // PASSIVE_LEVEL
    popl %eax
    call *%eax                                   // SystemRoutine(StartRoutine, StartContext), stdcall
    call _KiThreadReturned                       // system routines must terminate the thread
1:  hlt
    jmp 1b

// void KiCallOnStack(void (*fn)(), PVOID stackTop)  -- used to run DPCs on the PRCB DPC stack.
    .globl _KiCallOnStack
_KiCallOnStack:
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
