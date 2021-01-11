/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "boot.h"
#include "i2c.h"
#include "video.h"
#include "memory_layout.h"
#include "sha1.h"
#include "cpu.h"
#include "BootIde.h"
#include "config.h"
#include "iso_fs.h"
#include "lib/time/timeManagement.h"
#include "lib/cromwell/cromString.h"
#include "lib/cromwell/cromSystem.h"
#include "string.h"
#include "Gentoox.h"

//Grub bits
unsigned long saved_drive;
#if 0
unsigned long saved_partition;
unsigned long boot_drive;

static int nRet;
static unsigned int dwKernelSize= 0, dwInitrdSize = 0;


void ExittoLinux(CONFIGENTRY *config);
void ExittoLinuxFromNet(unsigned int initrdSize, char *append);
void startLinux(void* initrdStart, unsigned long initrdSize, const char* appendLine);
void setup(void* KernelPos, void* PhysInitrdPos, unsigned long InitrdSize, const char* kernel_cmdline);
void I2CRebootSlow(void);


void BootPrintConfig(CONFIGENTRY *config) {
    int CharsProcessed=0, CharsSinceNewline=0, Length=0;
    char c;
    printk("           Bootconfig : Kernel  %s \n", config->szKernel);
    VIDEO_ATTR=0xffa8a8a8;
    if (strlen(config->szInitrd)!=0) {
        printk("           Bootconfig : Initrd  %s \n", config->szInitrd);
    }
    printk("           Bootconfig : Kernel commandline :\n");
    Length=strlen(config->szAppend);
    while (CharsProcessed<Length) {
        c = config->szAppend[CharsProcessed];
        CharsProcessed++;
        CharsSinceNewline++;
        if ((CharsSinceNewline>50 && c==' ') || CharsSinceNewline>65) {
            printk("\n");
            if (CharsSinceNewline>25) printk("%c",c);
            CharsSinceNewline = 0;
        }
        else printk("%c",c);
    }
    printk("\n");
    VIDEO_ATTR=0xffa8a8a8;
}


void memPlaceKernel(const unsigned char* kernelOrg, unsigned int kernelSize)
{
    unsigned int nSizeHeader=((*(kernelOrg + 0x01f1))+1)*512;
    memcpy((unsigned char *)KERNEL_SETUP, kernelOrg, nSizeHeader);
    memcpy((unsigned char *)KERNEL_PM_CODE,(kernelOrg+nSizeHeader),kernelSize-nSizeHeader);
}


/*
CONFIGENTRY* LoadConfigNative(int drive, int partition) {
    busyLED();
    CONFIGENTRY *config;
    CONFIGENTRY *currentConfigItem;
    unsigned int nLen;
    unsigned int dwConfigSize=0;
    char *szGrub;

    szGrub = (char *) malloc(265+4);
    memset(szGrub,0,256+4);
    memset((unsigned char *)KERNEL_SETUP,0,2048);

    szGrub[0]=0xff;
    szGrub[1]=0xff;
    szGrub[2]=partition;
    szGrub[3]=0x00;

    errnum=0;
    boot_drive=0;
    saved_drive=0;
    saved_partition=0x0001ffff;
    buf_drive=-1;
    current_partition=0x0001ffff;
    current_drive=drive;
    buf_drive=-1;
    fsys_type = NUM_FSYS;
    disk_read_hook=NULL;
    disk_read_func=NULL;

    //Try for /boot/linuxboot.cfg first
    strcpy(&szGrub[4], "/boot/linuxboot.cfg");
    nRet=grub_open(szGrub);

    if(nRet!=1 || errnum) {
        //Not found - try /linuxboot.cfg
        errnum=0;
        strcpy(&szGrub[4], "/linuxboot.cfg");
        nRet=grub_open(szGrub);
    }

    dwConfigSize=filemax;
    if (nRet!=1 || errnum) {
        //File not found
        free(szGrub);
        return NULL;
    }

    nLen=grub_read((void *)KERNEL_SETUP, filemax);

    if (nLen > MAX_CONFIG_FILESIZE) nLen = MAX_CONFIG_FILESIZE;

    config = ParseConfig((char *)KERNEL_SETUP, nLen, NULL);

    for (currentConfigItem = (CONFIGENTRY*) config; currentConfigItem!=NULL ; currentConfigItem = (CONFIGENTRY*)currentConfigItem->nextConfigEntry) {
        //Set the drive ID and partition IDs for the returned config items
        currentConfigItem->bootType=BOOT_NATIVE;
        currentConfigItem->drive=drive;
        currentConfigItem->partition=partition;
    }

    grub_close();
    free(szGrub);
    return config;
}
*/
int LoadKernelNative(CONFIGENTRY *config) {
    busyLED();
    VIDEO_CURSOR_POSY=vmode.ymargin+96;
    printk("           Booting %s", config->title);
    dots();
    printk("\n\n");

    char *szGrub;
    unsigned char* tempBuf;

    szGrub = (char *) malloc(265+4);
        memset(szGrub,0,256+4);
        
    memset((unsigned char *)KERNEL_SETUP,0,2048);

    szGrub[0]=0xff;
    szGrub[1]=0xff;
    szGrub[2]=config->partition;
    szGrub[3]=0x00;

    errnum=0;
    boot_drive=0;
    saved_drive=0;
    saved_partition=0x0001ffff;
    buf_drive=-1;
    current_partition=0x0001ffff;
    current_drive=config->drive;
    buf_drive=-1;
    fsys_type = NUM_FSYS;
    disk_read_hook=NULL;
    disk_read_func=NULL;

    I2CTransmitWord(0x10, 0x0c01); // Close DVD tray
    printk("           Boot: Loading kernel '%s'", config->szKernel);
    dots();

    strncpy(&szGrub[4], config->szKernel,strlen(config->szKernel));

    nRet=grub_open(szGrub);

    if(nRet!=1) {
        //printk("           Unable to load kernel, Grub error %d\n", errnum);
        cromwellError();
        while(1);
    }
    // Use INITRD_START as temporary location for loading the Kernel
    tempBuf = (unsigned char *)INITRD_START;
    dwKernelSize=grub_read(tempBuf, MAX_KERNEL_SIZE);
    memPlaceKernel(tempBuf, dwKernelSize);
    grub_close();
    //printk(" -  %d bytes...\n", dwKernelSize);
        cromwellSuccess();

    if(strlen(config->szInitrd)!=0) {
        VIDEO_ATTR=0xffd8d8d8;
        printk("           Boot: Loading initrd '%s'", config->szInitrd);
        dots();
        printk("\t ");
        //printk("  Loading %s ", config->szInitrd);
        VIDEO_ATTR=0xffa8a8a8;
         strncpy(&szGrub[4], config->szInitrd,sizeof(config->szInitrd));
        nRet=grub_open(szGrub);

        if(filemax==0) {
            //printf("Empty file\n");
            cromwellError();
            while(1);
        }

        if( (nRet!=1) || (errnum)) {
            cromwellError();
            //printk("Unable to load initrd, Grub error %d\n", errnum);
            while(1);
        }

        //printk(" - %d bytes\n", filemax);
        cromwellSuccess();
        dwInitrdSize=grub_read((void*)INITRD_START, MAX_INITRD_SIZE);
        grub_close();
    } else {
        VIDEO_ATTR=0xffd8d8d8;
        //printk("  No initrd from config file");
        VIDEO_ATTR=0xffa8a8a8;
        dwInitrdSize=0;
    }
    free(szGrub);
    return true;
}
/*
CONFIGENTRY* LoadConfigFatX(void) {
    busyLED();
    FATXPartition *partition = NULL;
    FATXFILEINFO fileinfo;
    CONFIGENTRY *config=NULL, *currentConfigItem=NULL;

    partition = OpenFATXPartition(0,SECTOR_STORE,STORE_SIZE);

    if(partition != NULL) {

        if(LoadFATXFile(partition,"/linuxboot.cfg",&fileinfo)) {
            //Root of E has a linuxboot.cfg in
            config = (CONFIGENTRY*)malloc(sizeof(CONFIGENTRY));
            config = ParseConfig(fileinfo.buffer, fileinfo.fileSize, NULL);
            free(fileinfo.buffer);
        }
        else if(LoadFATXFile(partition,"/debian/linuxboot.cfg",&fileinfo) ) {
            //Try in /debian on E
            config = (CONFIGENTRY*)malloc(sizeof(CONFIGENTRY));
            config = ParseConfig(fileinfo.buffer, fileinfo.fileSize, "/debian");
            free(fileinfo.buffer);
            CloseFATXPartition(partition);
        }
    }
    if (config == NULL) return NULL;

    for (currentConfigItem = (CONFIGENTRY*) config; currentConfigItem!= NULL ; currentConfigItem = (CONFIGENTRY*)currentConfigItem->nextConfigEntry) {
        currentConfigItem->bootType = BOOT_FATX;
    }
    return config;
}
*/
int LoadKernelFatX(CONFIGENTRY *config) {
    busyLED();
    static FATXPartition *partition = NULL;
    static FATXFILEINFO fileinfo;
    static FATXFILEINFO infokernel;
    static FATXFILEINFO infoinitrd;
    unsigned char* tempBuf;

    memset((unsigned char *)KERNEL_SETUP,0,4096);
    memset(&fileinfo,0x00,sizeof(fileinfo));
    memset(&infokernel,0x00,sizeof(infokernel));
    memset(&infoinitrd,0x00,sizeof(infoinitrd));

    I2CTransmitWord(0x10, 0x0c01); // Close DVD tray

    VIDEO_CURSOR_POSY=vmode.ymargin+96;
    printk("           Booting %s", config->title);
    dots();
    printk("\n\n");

    partition = OpenFATXPartition(0,
            SECTOR_STORE,
            STORE_SIZE);

    if(partition == NULL) return 0;

    // Use INITRD_START as temporary location for loading the Kernel
    tempBuf = (unsigned char *)INITRD_START;
    printk("           Boot: Loading kernel '%s'", config->szKernel);
    dots();
    if(! LoadFATXFile(partition,config->szKernel,&infokernel)) {
        //printk("Error loading kernel %s\n",config->szKernel);
        //Won't free fileinfo.buffer because we don't know if malloc was called and program is stuck there anyway.
        cromwellError();
        while(1);
    } else {
        memcpy(tempBuf, fileinfo.buffer, fileinfo.fileSize);
        free(fileinfo.buffer);
        dwKernelSize = infokernel.fileSize;
        // moving the kernel to its final location
        memPlaceKernel(tempBuf, dwKernelSize);
        cromwellSuccess();
        //printk(" -  %d bytes...\n", infokernel.fileRead);
    }

    if(strlen(config->szInitrd)!=0) {
        VIDEO_ATTR=0xffd8d8d8;
        printk("           Boot: Loading initrd '%s'", config->szInitrd);
        dots();
        printk("\t ");
        wait_ms_blocking(50);

        if(! LoadFATXFile(partition,config->szInitrd,&infoinitrd)) {
            cromwellError();
            while(1);
        }
        memcpy((void*)INITRD_START, fileinfo.buffer, fileinfo.fileSize);
        free(fileinfo.buffer);
        dwInitrdSize = infoinitrd.fileSize;
        cromwellSuccess();
        //printk(" - %d %d bytes\n", dwInitrdSize,infoinitrd.fileRead);
    } else {
        printk("           Boot: Skipping initrd");
        dots();
        cromwellSuccess();
        dwInitrdSize=0;
    }
    return true;
}


CONFIGENTRY *LoadConfigCD(int cdromId) {
    busyLED();
    long dwConfigSize=0;
    int n;
    int configLoaded=0;
    CONFIGENTRY *config, *currentConfigItem;

    memset((unsigned char *)KERNEL_SETUP,0,4096);

    //See if we already have a CDROM in the drive
    //Try for 4 seconds - takes a while to 'spin up'.
    I2CTransmitWord(0x10, 0x0c01); // close DVD tray

    VIDEO_CURSOR_POSY=vmode.ymargin+96;
    printk("\n\n\n\n\n           Checking disc");
    dots();

    for (n=0;n<16;++n) {
        dwConfigSize = BootIso9660GetFile(cdromId,"/linuxboo.cfg", (unsigned char *)KERNEL_SETUP, 0x800);
        if (dwConfigSize>0) {
            configLoaded=1;
            break;
        }
        wait_ms_blocking(250);
    }

    if (!configLoaded) {
        cromwellWarning();
        //Needs to be changed for non-xbox drives, which don't have an eject line
        //Need to send ATA eject command.
        I2CTransmitWord(0x10, 0x0c00); // eject DVD tray
        VIDEO_ATTR=0xffeeeeff;
        printk("           Please insert a boot disc and close the DVD tray");
        dots();

        wait_ms_blocking(1000); // Wait for DVD to become responsive to inject command

        while(1) {
            // Make button 'A' close the DVD tray
            if (risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_A) == 1) {
                I2CTransmitWord(0x10, 0x0c01);
                // May as well break here too incase the drive is
                // a non-standard Xbox drive and can't report whether the
                // tray is closing or not.
                wait_ms_blocking(500);
                break;
            }

            // If the drive is closing, exit the loop.  This accounts
            // for people pushing the drive shut or even pressing the eject
            // button.
            if (DVD_TRAY_STATE == DVD_CLOSING) {
                wait_ms_blocking(500);
                break;
            }
            wait_ms_blocking(10);
        }

        busyLED();

        VIDEO_ATTR=0xffffffff;
        //printk("Loading linuxboot.cfg from CDROM... \n");
        //Try to load linuxboot.cfg - if we can't after a while, give up.
        for (n=0;n<48;++n) {
            dwConfigSize = BootIso9660GetFile(cdromId,"/linuxboo.cfg", (unsigned char *)KERNEL_SETUP, 0x800);
            if (dwConfigSize>0) {
                configLoaded=1;
                break;
            }
            wait_ms_blocking(250);
        }
    }

    //Failed to load the config file
    if (!configLoaded) {
        cromwellError();
        return NULL;
    }

    cromwellSuccess();
    printk("           Booting CD");
    dots();
    printk("\n\n");
        
    // LinuxBoot.cfg File Loaded
    config = ParseConfig((char *)KERNEL_SETUP, dwConfigSize, NULL);
    //Populate the configs with the drive ID
    for (currentConfigItem = (CONFIGENTRY*) config; currentConfigItem!=NULL; currentConfigItem = (CONFIGENTRY*)currentConfigItem->nextConfigEntry) {
        //Set the drive ID and partition IDs for the returned config items
        currentConfigItem->drive=cdromId;
        currentConfigItem->bootType=BOOT_CDROM;
    }

    strcpy(config->title,"CD/DVD");
    return config;
}

int LoadKernelCdrom(CONFIGENTRY *config) {
    busyLED();
    unsigned char* tempBuf;

    // Use INITRD_START as temporary location for loading the Kernel
    tempBuf = (unsigned char *)INITRD_START;
    printk("           Boot: Loading kernel '%s'", config->szKernel);
    dots();
    dwKernelSize=BootIso9660GetFile(config->drive,config->szKernel, tempBuf, MAX_KERNEL_SIZE);

    if( dwKernelSize < 0 ) {
        cromwellError();
        while(1);
    } else {
        memPlaceKernel(tempBuf, dwKernelSize);
        cromwellSuccess();
    }

    if(strlen(config->szInitrd)!=0) {
        VIDEO_ATTR=0xffd8d8d8;
        printk("           Boot: Loading initrd '%s'", config->szInitrd);
        dots();
        printk("\t ");
        VIDEO_ATTR=0xffa8a8a8;

        dwInitrdSize=BootIso9660GetFile(config->drive, config->szInitrd, (void*)INITRD_START, MAX_INITRD_SIZE);
        if( dwInitrdSize < 0 ) {
            cromwellError();
            while(1);
        }
        cromwellSuccess();
    } else {
        cromwellError();
        dwInitrdSize=0;
        while(1);
    }
    return true;
}
#endif

#if 0
void ExittoLinux(CONFIGENTRY *config) {
    busyLED();
    VIDEO_ATTR=0xff8888a8;
    //BootPrintConfig(config);
    //printk("     Kernel:  %s\n", (char *)(0x00090200+(*((unsigned short *)0x9020e)) ));
    //printk("\n");
    {
        char *sz="\2Starting titlehere\2";
        VIDEO_CURSOR_POSX=((vmode.width-BootVideoGetStringTotalWidth(sz))/2)*4;
        VIDEO_CURSOR_POSY=vmode.height-64;
        VIDEO_ATTR=0xff00ff00;
        printk("\2Starting %s\2", config->title);
    }
    setLED("rrrr");
    startLinux((void*)INITRD_START, dwInitrdSize, config->szAppend);
}

void ExittoLinuxFromNet(unsigned int initrdSize, char* append) {
    busyLED();
    VIDEO_ATTR=0xff8888a8;
    //BootPrintConfig(config);
    //printk("     Kernel:  %s\n", (char *)(0x00090200+(*((unsigned short *)0x9020e)) ));
    //printk("\n");
    {
        char *sz="\2Starting Linux\2";
        VIDEO_CURSOR_POSX=((vmode.width-BootVideoGetStringTotalWidth(sz))/2)*4;
        VIDEO_CURSOR_POSY=vmode.height-64;
        VIDEO_ATTR=0xff00ff00;
        printk("\2Starting Linux\2");
    }
    setLED("rrrr");
    startLinux((void*)INITRD_START, initrdSize, append);
}


void startLinux(void* initrdStart, unsigned long initrdSize, const char* appendLine)
{
    int nAta=0;
    // turn off USB
    BootStopUSB();
    setup( (void *)KERNEL_SETUP, initrdStart, initrdSize, appendLine);
        
    if(tsaHarddiskInfo[0].m_bCableConductors == 80) {
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&2) nAta=1;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&4) nAta=2;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&8) nAta=3;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&16) nAta=4;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&32) nAta=5;
    } else {
        // force the HDD into a good mode 0x40 ==UDMA | 2 == UDMA2
        nAta=2; // best transfer mode without 80-pin cable
    }
    // nAta=1;
    BootIdeSetTransferMode(0, 0x40 | nAta);
    BootIdeSetTransferMode(1, 0x40 | nAta);

    goodLED();

    // Set framebuffer address to final location (for vesafb driver)
    (*(unsigned int*)0xFD600800) = (0xf0000000 | ((xbox_ram*0x100000) - FB_SIZE));

    // disable interrupts
    asm volatile ("cli\n");

    // clear idt area
    memset((void*)IDT_LOC,0x0,1024*8);

    __asm __volatile__ (
    "wbinvd\n"

    // Flush the TLB
    "xor %eax, %eax \n"
    "mov %eax, %cr3 \n"

    // Load IDT table (0xB0000 = IDT_LOC)
    "lidt     0xB0000\n"

    // DR6/DR7: Clear the debug registers
    "xor %eax, %eax \n"
    "mov %eax, %dr6 \n"
    "mov %eax, %dr7 \n"
    "mov %eax, %dr0 \n"
    "mov %eax, %dr1 \n"
    "mov %eax, %dr2 \n"
    "mov %eax, %dr3 \n"

    // Kill the LDT, if any
    "xor    %eax, %eax \n"
    "lldt %ax \n"
        
    // Reload CS as 0010 from the new GDT using a far jump
    ".byte 0xEA       \n"   // jmp far 0010:reload_cs
    ".long reload_cs_exit  \n"
    ".word 0x0010  \n"

    ".align 16  \n"
    "reload_cs_exit: \n"

    // CS is now a valid entry in the GDT.  Set SS, DS, and ES to valid
    // descriptors, but clear FS and GS as they are not necessary.

    // Set SS, DS, and ES to a data32 segment with maximum limit.
    "movw $0x0018, %ax \n"
    "mov %eax, %ss \n"
    "mov %eax, %ds \n"
    "mov %eax, %es \n"

    // Clear FS and GS
    "xor %eax, %eax \n"
    "mov %eax, %fs \n"
    "mov %eax, %gs \n"

    // Set the stack pointer to give us a valid stack
    "movl $0x03BFFFFC, %esp \n"

    "xor     %ebx, %ebx \n"
    "xor     %eax, %eax \n"
    "xor     %ecx, %ecx \n"
    "xor     %edx, %edx \n"
    "xor     %edi, %edi \n"
    "movl     $0x90000, %esi\n"       // kernel setup area
    "ljmp     $0x10, $0x100000\n"     // Jump to Kernel protected mode entry
    );

    // We are not longer here, we are already in the Linux loader, we never come back here

    // See you again in Linux then
    while(1);
}
#endif
