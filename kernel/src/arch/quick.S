// Quick reboot (spec/stock/02 §1): restart the kernel in place. Runs on a small stack in the persisted section,
// quiesces DMA hardware, restores initialised .data from KernelEntry's RAM copy (or the flash copy) described by our DOS-header fields
// (0x28 bytes to zero, 0x2C bytes to copy, 0x30 flash source, 0x34 destination), zeroes BSS, then re-enters
// KernelEntry with the persisted boot keys.
    .text
    .globl _KiQuickReboot
_KiQuickReboot:
    cli
    movl $(_KiQuickRebootStack + 0x800), %esp
    call _KiQuickRebootQuiesce
    cld
    movl _KiDataInitSource, %esi  // the copy KernelEntry made, else the flash copy
    testl %esi, %esi
    jnz 1f
    movl $0xFFC000FB, 0xC0300FFC  // flash copy: map PDE 0x3FF again (MmInit cleared it), uncached
    movl %cr3, %eax
    movl %eax, %cr3
    movl 0x80010030, %esi
1:
    movl 0x80010034, %edi
    movl 0x8001002C, %ecx
    rep movsb
    movl 0x80010028, %ecx
    xorl %eax, %eax
    rep stosb
    movb $1, _KiQuickRebootFlag
    incl _KiQuickRebootCount
    pushl $_KiPersistedBootKeys
    pushl $0
    pushl $0                      // return address slot (KernelEntry never returns)
    jmp _KernelEntry

    .section .persist,"aw"
    .p2align 4
_KiQuickRebootStack:
    .space 0x800
