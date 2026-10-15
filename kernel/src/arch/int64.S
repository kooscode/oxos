// MSVC compiler helpers for 64-bit arithmetic on i386 (called by clang for the i386-pc-win32 target).
// Division helpers take (dividend, divisor) on the stack and pop 16 bytes; shifts take edx:eax and cl.
    .text

.macro DIVHELPER name, impl, want_rem
    .globl \name
\name:
    subl $8, %esp
    pushl %esp              // &rem
    pushl 28(%esp)          // divisor hi
    pushl 28(%esp)          // divisor lo
    pushl 28(%esp)          // dividend hi
    pushl 28(%esp)          // dividend lo
    call \impl
    addl $20, %esp
.if \want_rem
    movl (%esp), %eax
    movl 4(%esp), %edx
.endif
    addl $8, %esp
    ret $16
.endm

    DIVHELPER __aulldiv, _XkUDivMod64, 0
    DIVHELPER __aullrem, _XkUDivMod64, 1
    DIVHELPER __alldiv, _XkSDivMod64, 0
    DIVHELPER __allrem, _XkSDivMod64, 1

// Quotient in edx:eax, remainder in ebx:ecx.
.macro DVRMHELPER name, impl
    .globl \name
\name:
    subl $8, %esp
    pushl %esp
    pushl 28(%esp)
    pushl 28(%esp)
    pushl 28(%esp)
    pushl 28(%esp)
    call \impl
    addl $20, %esp
    movl (%esp), %ecx
    movl 4(%esp), %ebx
    addl $8, %esp
    ret $16
.endm

    DVRMHELPER __aulldvrm, _XkUDivMod64
    DVRMHELPER __alldvrm, _XkSDivMod64

    .globl __allmul
__allmul:
    movl 8(%esp), %eax      // a hi
    movl 16(%esp), %ecx     // b hi
    orl %eax, %ecx
    movl 12(%esp), %ecx     // b lo
    jnz 1f
    movl 4(%esp), %eax      // a lo
    mull %ecx
    ret $16
1:  pushl %ebx
    mull %ecx               // a hi * b lo
    movl %eax, %ebx
    movl 8(%esp), %eax      // a lo
    mull 20(%esp)           // a lo * b hi
    addl %eax, %ebx
    movl 8(%esp), %eax      // a lo
    mull %ecx               // a lo * b lo
    addl %ebx, %edx
    popl %ebx
    ret $16

    .globl __allshl
__allshl:
    cmpb $64, %cl
    jae 2f
    cmpb $32, %cl
    jae 1f
    shldl %cl, %eax, %edx
    shll %cl, %eax
    ret
1:  movl %eax, %edx
    xorl %eax, %eax
    andb $31, %cl
    shll %cl, %edx
    ret
2:  xorl %eax, %eax
    xorl %edx, %edx
    ret

    .globl __allshr
__allshr:
    cmpb $64, %cl
    jae 2f
    cmpb $32, %cl
    jae 1f
    shrdl %cl, %edx, %eax
    sarl %cl, %edx
    ret
1:  movl %edx, %eax
    sarl $31, %edx
    andb $31, %cl
    sarl %cl, %eax
    ret
2:  sarl $31, %edx
    movl %edx, %eax
    ret

    .globl __aullshr
__aullshr:
    cmpb $64, %cl
    jae 2f
    cmpb $32, %cl
    jae 1f
    shrdl %cl, %edx, %eax
    shrl %cl, %edx
    ret
1:  movl %edx, %eax
    xorl %edx, %edx
    andb $31, %cl
    shrl %cl, %eax
    ret
2:  xorl %eax, %eax
    xorl %edx, %edx
    ret
