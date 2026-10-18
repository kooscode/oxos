// Kernel entry from the 2BL (spec/stock/01-boot.md §1):
//   cdecl KernelEntry(arg0, const UCHAR *keys)  -- keys = 16-byte EEPROM key + 16-byte cert key; arg0 = 'OXOS'
//   from the open image's 2BL, whose key block continues with EEPROM-key HMAC states (spec/stock/01 §5).
// The 2BL's stack lives in memory we are about to wipe, so switch to our own stack first.

#define BOOT_STACK_SIZE 0x3000

    .text
    .globl _KernelEntry
_KernelEntry:
    cli
    movl 4(%esp), %edx                          // arg0
    movl 8(%esp), %eax                          // keys
    movl $(_KiBootStack + BOOT_STACK_SIZE - 0x210), %esp   // NPX save area of the idle thread above
    xorl %ebp, %ebp
    pushl %eax
    pushl %edx
    call _KiSystemStartup                       // never returns
1:  hlt
    jmp 1b

// Double-fault and NMI run as hardware task switches (TSS selectors 0x28 / 0x30).
    .globl _KiDoubleFaultTask
_KiDoubleFaultTask:
    movb $0xDF, %al
    outb %al, $0x80
1:  cli
    hlt
    jmp 1b

    .globl _KiNmiTask
_KiNmiTask:
    movb $0xD2, %al
    outb %al, $0x80
1:  cli
    hlt
    jmp 1b

    .bss
    .p2align 4
    .globl _KiBootStack
_KiBootStack:
    .space BOOT_STACK_SIZE
