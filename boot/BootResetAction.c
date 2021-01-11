/*
 * Sequences the necessary post-reset actions from as soon as we are able to run C
 */

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************
 */

#include "boot.h"
#include "BootEEPROM.h"
#include "FatFSAccessor.h"
#include "i2c.h"
#include "cpu.h"
#include "config.h"
#include "video.h"
#include "memory_layout.h"
#include "cromwell.h"
#include "IconMenu.h"
#include "MenuActions.h"
#include "MenuInits.h"
#include "menu/misc/ConfirmDialog.h"
#include "string.h"
#include "lib/time/timeManagement.h"
#include "lib/cromwell/cromSystem.h"
#include "lib/cromwell/CallbackTimer.h"
#include "include/BootLogger.h"

JPEG jpegBackdrop;

int nTempCursorMbrX, nTempCursorMbrY;

extern volatile int nInteruptable;

volatile CURRENT_VIDEO_MODE_DETAILS vmode;

void ClearScreen (void)
{
    BootVideoClearScreen(&jpegBackdrop, 0, vmode.height-1);
}

void printMainMenuHeader(void)
{
    VIDEO_CURSOR_POSX = vmode.xmargin + 160;
    VIDEO_CURSOR_POSY = vmode.ymargin + 40;

    //green
    VIDEO_ATTR=0xffffffff; 
    printk("\2"PROG_NAME" v" VERSION "\n\n\2");

    // RED
    VIDEO_ATTR=0xffff0000; // ARGB
    printk("       RAM: %dMB\n", xbox_ram);

    // // Print EEPROM info
    // BootEepromPrintInfo();

    printk("       WIDTH %d  \n", vmode.width);
    printk("       HEIGHT %d  \n", vmode.height);
 

    // VIDEO_CURSOR_POSX=(vmode.xmargin)*4;
    // // capture title area
    // VIDEO_ATTR=0xffc8c8c8;
    // printk("           Encoder: ");
    // VIDEO_ATTR=0xffc8c800;
    // printk("%s  ", VideoEncoderName());
    // VIDEO_ATTR=0xffc8c8c8;
    // printk("Cable: ");
    // VIDEO_ATTR=0xffc8c800;
    // printk("%s  ", AvCableName());

    // int n = 0;
    // int nx = 0;
    // if (I2CGetTemperature(&n, &nx))
    // {
    //     VIDEO_ATTR=0xffc8c8c8;
    //     printk("CPU Temp: ");
    //     VIDEO_ATTR=0xffc8c800;
    //     printk("%doC  ", n);
    //     VIDEO_ATTR=0xffc8c8c8;
    //     printk("M/b Temp: ");
    //     VIDEO_ATTR=0xffc8c800;
    //     printk("%doC  ", nx);
    // }

    // printk("\n");
}

//////////////////////////////////////////////////////////////////////
//
//  BootResetAction()

extern void BootResetAction ( void )
{
    unsigned char EjectButtonPressed=0;

    BootLogger(DEBUG_ALWAYS_SHOW, DBG_LVL_INFO, "OXOS is starting.");

    memcpy(&cromwell_config, (void*)(CODE_LOC_START + 0x20), sizeof(cromwell_config));
    memcpy(&cromwell_retryload, (void*)(CODE_LOC_START + 0x20 + sizeof(cromwell_config)), sizeof(cromwell_retryload));
    memcpy(&cromwell_2blversion, (void*)(CODE_LOC_START + 0x20 + sizeof(cromwell_config) + sizeof(cromwell_retryload)), sizeof(cromwell_2blversion));
    memcpy(&cromwell_2blsize, (void*)(CODE_LOC_START + 0x20 + sizeof(cromwell_config) + sizeof(cromwell_retryload) + sizeof(cromwell_2blversion)), sizeof(cromwell_2blsize));

    VIDEO_CURSOR_POSX=40;
    VIDEO_CURSOR_POSY=140;
        
    VIDEO_AV_MODE = 0xff;
    nInteruptable = 0;

    // prep our BIOS console print state
    VIDEO_ATTR = 0xffffffff;

    // init malloc() and free() structures
    MemoryManagementInitialization((void *)MEMORYMANAGERSTART, MEMORYMANAGERSIZE);
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_DEBUG, "Init soft MMU.");

    BootDetectMemorySize();

    BootInterruptsWriteIdt();

    // initialize the PCI devices
    BootPciPeripheralInitialization();
    
    I2CTransmitWord(0x10, 0x1901); // no reset on eject

    //detect if started with EJECT
    if(I2CTransmitByteGetReturn(0x10, 0x03) & 0x01)
    {
        EjectButtonPressed = 1;
        I2CTransmitByteGetReturn(0x10, 0x11); // dummy Query IRQ
        I2CWriteBytetoRegister(0x10, 0x03,0x00); // Clear Tray Register
        I2CTransmitWord(0x10, 0x0c01); // close DVD tray
    }

    /* We allow interrupts */
    BootPciInterruptEnable();
    nInteruptable = 1;

    callbackTimer_init();
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_DEBUG, "CallbackTimer init done.");
    BootStartUSB();
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "USB init done.");


    // Reset the AGP bus and start with good condition
    BootAGPBUSInitialization();

    I2CTransmitByteGetReturn(0x10, 0x11);// dummy Query IRQ
    I2CTransmitWord(0x10, 0x1a01); // Enable PIC interrupts. Cannot be deactivated once set.
    wait_us_blocking(760000);

    // We disable The CPU Cache
    cache_disable();
    // We Update the Microcode of the CPU
    display_cpuid_update_microcode();
    // We Enable The CPU Cache
    cache_enable();
    //setup_ioapic();

    // Decrypt EEPROM
    BootEepromReadEntireEEPROM();
    memcpy(&origEeprom, &eeprom, sizeof(EEPROMDATA));
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "Initial EEprom read.");
        
    // unknown I2C data sent after eeprom read
    I2CTransmitWord(0x10, 0x1a01); // unknown, done immediately after reading out eeprom data
    I2CTransmitWord(0x10, 0x1b04); // unknown

    // set Ethernet MAC address from EEPROM
    {
        volatile unsigned char * pb=(unsigned char *)0xfef000a8;  // Ethernet MMIO base + MAC register offset (<--thanks to Anders Gustafsson)
        int n;
        for(n=5;n>=0;n--) { *pb++=    eeprom.MACAddress[n]; } // send it in backwards, its reversed by the driver
    }

    // clear the Video Ram
    memset((void *)FB_START,0x00,FB_SIZE);

    // Load and Init the Background image
    BootVgaInitializationKernelNG((CURRENT_VIDEO_MODE_DETAILS *)&vmode);
    jpegBackdrop.pData =NULL;
    jpegBackdrop.pBackdrop = NULL; //Static memory alloc now.

    //load jpeg from image
    BootVideoInitJPEGBackdropBuffer(&jpegBackdrop);
  
    // paint main menu
    videosavepage = malloc(FB_SIZE);
    ClearScreen();
    printMainMenuHeader();

    printk("\n\n\n\n");

    // // Wait for DVD?
    // BootIdeWaitNotBusy(0x1f0);
    // wait_ms(100);


    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "Starting IDE init.");
    BootIdeInit();
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "IDE init done.");
    
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "Starting FatFS init.");
    FatFS_init();
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "FatFS init done.");
    
    // BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "Starting VirtualRoot init.");
    // VirtualRootInit();
    // BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "VirtualRoot init done.");
    // BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "Starting DebugLogger init.");
    // debugLoggerInit();
    // BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "DebugLogger init done.");

    // printk("\n\n\n\n");

    // nTempCursorMbrX=VIDEO_CURSOR_POSX;
    // nTempCursorMbrY=VIDEO_CURSOR_POSY;

    // Init Icon Menu
    IconMenuInit();
    
    // Main loop..
    while(IconMenu())
    {
        ClearScreen();
        printMainMenuHeader();
    }

    //Good practice.
    free(videosavepage);

    //Should never come back here.
    while(1);
}
