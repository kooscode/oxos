// Structured exception support: context capture, continue, raise and unwind entry points (spec/14).
// CONTEXT (Xbox, 0x238 bytes): ContextFlags, FloatSave[0x204], Edi Esi Ebx Edx Ecx Eax Ebp Eip SegCs EFlags Esp SegSs.

#define CX_FLAGS  0x000
#define CX_EDI    0x208
#define CX_ESI    0x20C
#define CX_EBX    0x210
#define CX_EDX    0x214
#define CX_ECX    0x218
#define CX_EAX    0x21C
#define CX_EBP    0x220
#define CX_EIP    0x224
#define CX_SEGCS  0x228
#define CX_EFLAGS 0x22C
#define CX_ESP    0x230
#define CX_SEGSS  0x234
#define CONTEXT_SIZE 0x238
#define CONTEXT_FULL 0x10007

    .text

// void cdecl KiContinueContext(CONTEXT *Context): resume execution with Context (never returns).
// A small frame {eax, ebx, eip, cs, eflags} is built just below the target stack pointer, then iret.
    .globl _KiContinueContext
_KiContinueContext:
    movl 4(%esp), %ebx
    movl CX_ESP(%ebx), %eax
    subl $20, %eax
    movl CX_EAX(%ebx), %ecx
    movl %ecx, 0(%eax)
    movl CX_EBX(%ebx), %ecx
    movl %ecx, 4(%eax)
    movl CX_EIP(%ebx), %ecx
    movl %ecx, 8(%eax)
    movl $8, 12(%eax)
    movl CX_EFLAGS(%ebx), %ecx
    movl %ecx, 16(%eax)
    movl CX_EDI(%ebx), %edi
    movl CX_ESI(%ebx), %esi
    movl CX_EBP(%ebx), %ebp
    movl CX_EDX(%ebx), %edx
    movl CX_ECX(%ebx), %ecx
    movl %eax, %esp
    popl %eax
    popl %ebx
    iretl

// Fill *ctx (in %ebx) with the caller-of-caller state: the caller's frame starts at %ebp (pushl %ebp; movl %esp,%ebp),
// so return address = 4(%ebp), its stack pointer after returning and popping `argbytes` = %ebp + 8 + argbytes.
.macro CAPTURE_CALLER argbytes
    movl $CONTEXT_FULL, CX_FLAGS(%ebx)
    movl %edi, CX_EDI(%ebx)
    movl %esi, CX_ESI(%ebx)
    movl -4(%ebp), %eax          // caller's ebx (saved by the prologue below)
    movl %eax, CX_EBX(%ebx)
    movl $0, CX_EDX(%ebx)
    movl $0, CX_ECX(%ebx)
    movl $0, CX_EAX(%ebx)
    movl 0(%ebp), %eax
    movl %eax, CX_EBP(%ebx)
    movl 4(%ebp), %eax
    movl %eax, CX_EIP(%ebx)
    movl %cs, %eax
    movl %eax, CX_SEGCS(%ebx)
    movl %ss, %eax
    movl %eax, CX_SEGSS(%ebx)
    pushfl
    popl CX_EFLAGS(%ebx)
    leal (8 + \argbytes)(%ebp), %eax
    movl %eax, CX_ESP(%ebx)
.endm

// void NTAPI RtlCaptureContext(CONTEXT *Context): the state at the caller's return point.
    .globl _RtlCaptureContext@4
_RtlCaptureContext@4:
    pushl %ebx
    movl 8(%esp), %ebx
    movl %eax, CX_EAX(%ebx)
    movl %ecx, CX_ECX(%ebx)
    movl %edx, CX_EDX(%ebx)
    movl %esi, CX_ESI(%ebx)
    movl %edi, CX_EDI(%ebx)
    movl %ebp, CX_EBP(%ebx)
    movl (%esp), %eax
    movl %eax, CX_EBX(%ebx)
    movl 4(%esp), %eax
    movl %eax, CX_EIP(%ebx)
    leal 12(%esp), %eax
    movl %eax, CX_ESP(%ebx)
    movl %cs, %eax
    movl %eax, CX_SEGCS(%ebx)
    movl %ss, %eax
    movl %eax, CX_SEGSS(%ebx)
    pushfl
    popl CX_EFLAGS(%ebx)
    movl $CONTEXT_FULL, CX_FLAGS(%ebx)
    movl CX_EAX(%ebx), %eax
    popl %ebx
    ret $4

// void NTAPI RtlRaiseException(EXCEPTION_RECORD *Record): dispatch with the caller's context; a handler that
// continues execution resumes after this call.
    .globl _RtlRaiseException@4
_RtlRaiseException@4:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    subl $CONTEXT_SIZE, %esp
    movl %esp, %ebx
    CAPTURE_CALLER 4
    pushl %ebx
    pushl 8(%ebp)
    call _KiRaiseException          // cdecl (record, context); noreturn
1:  jmp 1b

// void NTAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, EXCEPTION_RECORD *Record, PVOID ReturnValue)
    .globl _RtlUnwind@16
_RtlUnwind@16:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    subl $CONTEXT_SIZE, %esp
    movl %esp, %ebx
    CAPTURE_CALLER 16
    pushl %ebx
    pushl 20(%ebp)
    pushl 16(%ebp)
    pushl 12(%ebp)
    pushl 8(%ebp)
    call _KiUnwind                  // cdecl (frame, ip, record, value, context); noreturn
1:  jmp 1b
