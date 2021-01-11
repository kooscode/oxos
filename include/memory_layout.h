#ifndef memory_layout_h
#define memory_layout_h

/* a retail Xbox has 64 MB of RAM */
#define RAMSIZE (64 * 1024*1024)
/* parameters for the kernel have to be here */
#define KERNEL_SETUP   0x90000
/* command line must not be overwritten, place it in unused setup area */
#define CMD_LINE_LOC (KERNEL_SETUP+0x0800)
/* place GDT at 0xA0000 */
#define GDT_LOC 0xA0000
/* place IDT at 0xB0000 */
#define IDT_LOC 0xB0000
/* the protected mode part of the kernel has to reside at 1 MB in RAM */
#define KERNEL_PM_CODE     0x00100000
/* 8 MB ought to be enough kernel */
#define KERNEL_PM_CODE_END 0x00900000

#define INITRD_START       KERNEL_PM_CODE_END
#define MAX_INITRD_END     0x01A00000

#define MEMORYMANAGERSTART MAX_INITRD_END
#define MEMORYMANAGEREND   0x02FFFFFF

#define CODE_LOC_START 0x03800000 /* Decompressed cromwell binary get puts at that offset in RAM */

#define STACK_TOP CODE_LOC_START /* Stack is going down on x86 */

#define MAX_KERNEL_SIZE    (KERNEL_PM_CODE_END - KERNEL_PM_CODE)
#define MEMORYMANAGERSIZE  (MEMORYMANAGEREND - MEMORYMANAGERSTART)
#define MAX_INITRD_SIZE    (MAX_INITRD_END - INITRD_START)

/* the size of the framebuffer (defaults to 4 MB) */
#define FB_SIZE 0x00400000
/* the start of the framebuffer */
#define FB_START (0xf0000000 | (RAMSIZE - FB_SIZE))

/* let's reserve 4 MB at the top for the framebuffer */
#define RAMSIZE_USE (RAMSIZE - FB_SIZE)

//#define LPCFlashadress 0xFFF00000
#define LPCFlashadress 0xFF000000u

//Location and max size of extracted 2BL from BFM BIOS
#define MIN_2BL 0x400000
#define MAX_2BL 0x405FFF
#define THE_2BL_SIZE 0x6000

//Kernel location range and size definition for BFM BIOS
#define MIN_SHADOW_ROM 0x0000000
#define MAX_SHADOW_ROM 0x3000000
#define SHADOW_ROM_SIZE 0x100000

#endif /* #ifndef memory_layout_h */
