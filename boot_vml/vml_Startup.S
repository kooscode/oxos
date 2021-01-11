#include "../include/memory_layout.h"

.code32

.section .vmlinuz_start, "ax"

.org 0x00
    
.org 0x1f1
    .byte 0x0A // *((BYTE *)0x901f1))+1)*512; .. Loader Takes us to 0x1600

.org 0x1600
       
    xor    %eax, %eax
    xor    %edx, %edx          
    cld
    
    mov     $CODE_LOC_START, %edi        // Destiation
    mov     $0x00100100, %esi    // Source
    mov     $0x400000, %ecx        // 4MB
    rep movsb
    wbinvd
    cld    
    ljmp     $0x10, $CODE_LOC_START

.org 0x1700    // Here, we append the thing we want to load

