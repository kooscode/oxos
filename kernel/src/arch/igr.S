// In-game reset (spec/oxos-internal/22): KeRaiseIrqlToDpcLevel with a look at its caller.
//
// XAPI's XInputGetState(hDevice, pState), linked into every XDK title, starts
//     push ebx; push esi; xor ebx, ebx; call [KeRaiseIrqlToDpcLevel]
//     mov reg, [esp+0Ch]; <2 bytes> A3 00 00 00   (the device field at +0xA3)
// When that is the caller, its return address is redirected to HalpIgrReturn, which sees the filled-in state once
// the function has returned: no title code is patched.
    .text
    .globl _KeRaiseIrqlToDpcLevel@0
_KeRaiseIrqlToDpcLevel@0:
    movl (%esp), %eax
    cmpl $0x80000000, %eax        // kernel callers: straight on
    jae 1f
    movl (%eax), %edx
    andl $0xFFFF00FF, %edx
    cmpl $0x0C24008B, %edx        // 8B ?? 24 0C: mov reg, [esp+0Ch]
    jne 1f
    cmpl $0xA3, 6(%eax)
    jne 1f
    // Stack: our return, saved esi, saved ebx, the title's return, hDevice, pState. Move these six down by 12 bytes
    // and put HalpIgrReturn where the title's return address was copied to: XInputGetState's "ret 8" then lands in
    // HalpIgrReturn with the original return address, hDevice and pState above it.
    subl $12, %esp
    pushl %esi
    pushl %edi
    leal 0x14(%esp), %esi
    leal 0x08(%esp), %edi
    movl $6, %ecx
    rep movsl
    popl %edi
    popl %esi
    movl $HalpIgrReturn, 0x0C(%esp)
1:
    movb $2, %cl                  // DISPATCH_LEVEL
    jmp @KfRaiseIrql@4

// eax = XInputGetState's result (0 = ERROR_SUCCESS); [esp] the title's return, [esp+4] hDevice, [esp+8] pState.
HalpIgrReturn:
    testl %eax, %eax
    jnz 2f
    pushl 8(%esp)
    call _HalpIgrCheck@4
    xorl %eax, %eax
2:
    ret $8
