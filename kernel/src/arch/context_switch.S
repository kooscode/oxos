// Context switch, thread start-up and stack switching (spec/stock/07 "Scheduling").
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

// ULONG KiCallTitleRoutine(PVOID Routine, ULONG Count, const ULONG *Arguments)  -- cdecl (arch.h CallTitleRoutine)
// Calls a routine a title may have supplied with Count 32-bit arguments, pushed last to first, and returns its eax. esp is restored from ebp afterwards: a
// routine built cdecl where the kernel expects stdcall returns without removing its arguments, and the caller's stack stays intact (spec/oxos_internal/47).
    .globl _KiCallTitleRoutine
_KiCallTitleRoutine:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi
    movl 12(%ebp), %ecx
    movl 16(%ebp), %esi
1:  testl %ecx, %ecx
    jz 2f
    pushl -4(%esi,%ecx,4)
    decl %ecx
    jmp 1b
2:  call *8(%ebp)
    leal -4(%ebp), %esp                          // whatever the routine left on the stack
    popl %esi
    popl %ebp
    ret

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
