// Import tracing (debug builds with TRACE_IMPORTS=1): each traced title import jumps here with eax = its index.
// The call is recorded, then the real export runs with the caller's registers and stack unchanged.
    .text
    .globl _XkTraceEntry
_XkTraceEntry:
    pushfl
    pushal                      // eax is at +28; the caller's return address at +36
    leal 36(%esp), %ecx
    pushl %ecx
    pushl %eax
    call _XkTraceRecord
    addl $8, %esp
    popal
    popfl
    jmp *_XkTraceTargets(,%eax,4)

// Return trampoline: the recorder swapped the caller's return address for this one.
    .globl _XkTraceReturn
_XkTraceReturn:
    pushl $0                    // becomes the original return address
    pushfl
    pushal                      // eax at +28, the placeholder at +36
    pushl %eax
    call _XkTraceLeave          // records eax, returns the original return address
    addl $4, %esp
    movl %eax, 36(%esp)
    popal
    popfl
    ret
