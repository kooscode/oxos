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
    VIDEO_ARGB=0xffffffff; 
    printk("\2"PROG_NAME" v" VERSION "\n\n\2");

    // RED
    VIDEO_ARGB=0xffff0000; // ARGB
    printk("       RAM: %dMB\n", xbox_ram);

    // // Print EEPROM info
    // BootEepromPrintInfo();

    printk("       WIDTH %d  \n", vmode.width);
    printk("       HEIGHT %d  \n", vmode.height);
 
    // capture title area
    VIDEO_ARGB=0xffc8c8c8;
    printk("       Encoder: ");
    VIDEO_ARGB=0xffc8c800;
    printk("%s  ", VideoEncoderName());
    VIDEO_ARGB=0xffc8c8c8;
    printk("Cable: ");
    VIDEO_ARGB=0xffc8c800;
    printk("%s  \n", AvCableName());

    // int n = 0;
    // int nx = 0;
    // if (I2CGetTemperature(&n, &nx))
    // {
    //     VIDEO_ARGB=0xffc8c8c8;
    //     printk("CPU Temp: ");
    //     VIDEO_ARGB=0xffc8c800;
    //     printk("%doC  ", n);
    //     VIDEO_ARGB=0xffc8c8c8;
    //     printk("M/b Temp: ");
    //     VIDEO_ARGB=0xffc8c800;
    //     printk("%doC  ", nx);
    // }

    // printk("\n");
}

//////////////////////////////////////////////////////////////////////
//
// BootResetAction()
// 2BL finished and loaded the ROM into RAM and starts here!
extern void BootResetAction ( void )
{
    unsigned char EjectButtonPressed=0;

    // Set LED to orange.
    setLED("oooo");

    // init malloc() and free() structures
    MemoryManagementInitialization((void *)MEMORYMANAGERSTART, MEMORYMANAGERSIZE);

    BootDetectMemorySize();

    BootInterruptsWriteIdt();

    // initialize the PCI devices
    BootPciPeripheralInitialization();
    
    // no reset on eject
    I2CTransmitWord(0x10, 0x1901); 

    //detect if started with EJECT
    if(I2CTransmitByteGetReturn(0x10, 0x03) & 0x01)
    {
        EjectButtonPressed = 1;
        // dummy Query IRQ
        I2CTransmitByteGetReturn(0x10, 0x11); 
        // Clear Tray Register
        I2CWriteBytetoRegister(0x10, 0x03,0x00);
        // close DVD tray
        I2CTransmitWord(0x10, 0x0c01); 
    }

    // We allow interrupts
    BootPciInterruptEnable();
    nInteruptable = 1;

    callbackTimer_init();
    
    // init USB
    BootStartUSB();

    // Reset the AGP bus and start with good condition
    BootAGPBUSInitialization();

    // dummy Query IRQ
    I2CTransmitByteGetReturn(0x10, 0x11);
    // Enable PIC interrupts. Cannot be deactivated once set.
    I2CTransmitWord(0x10, 0x1a01); 

    //Delay?
    wait_us_blocking(760000);

    // We disable The CPU Cache
    cache_disable();
    // We Update the Microcode of the CPU
    display_cpuid_update_microcode();
    // We Enable The CPU Cache
    cache_enable();

    // We are notusing IO Advance Programmable Interrupts?
    //setup_ioapic();

    // Decrypt EEPROM
    BootEepromReadEntireEEPROM();
    memcpy(&origEeprom, &eeprom, sizeof(EEPROMDATA));
        
    // unknown I2C data sent after eeprom read
    I2CTransmitWord(0x10, 0x1a01); // unknown, done immediately after reading out eeprom data
    I2CTransmitWord(0x10, 0x1b04); // unknown

    // set Ethernet MAC address from EEPROM
    {
        volatile unsigned char * pb=(unsigned char *)0xfef000a8;  // Ethernet MMIO base + MAC register offset (<--thanks to Anders Gustafsson)
        int n;
        for(n=5;n>=0;n--) { *pb++= eeprom.MACAddress[n]; } // send it in backwards, its reversed by the driver
    }

    // clear the Video Ram (blank screen)
    memset((void *)FB_START,0x00,FB_SIZE);

    // Load and Init the Background image
    BootVgaInitializationKernelNG((CURRENT_VIDEO_MODE_DETAILS *)&vmode);
    jpegBackdrop.pData =NULL;
    jpegBackdrop.pBackdrop = NULL; 
  
    // paint main menu
    ClearScreen();
    printMainMenuHeader();

    printk("       Init IDE\n");
    BootIdeWaitNotBusy(0x1f0);
    wait_ms(100);
    BootIdeInit();

    printk("       Init FatX\n");
    FatFS_init();
    BootLogger(DEBUG_BOOT_LOG, DBG_LVL_INFO, "FatFS init done.");

    //splash wait
    wait_ms(5000);

    //load jpeg from rom image
    BootVideoInitJPEGBackdropBuffer(&jpegBackdrop);
    
    // Main loop..
    ClearScreen();
    printMainMenuHeader();
    TEXTMENU* mainmenu = TextMenuInit();
    while(cromwellLoop())
    {
       TextMenu(mainmenu, NULL);
    }

    // Set LED flashing red.
    setLED("rxrx");

    //Should never come back here.
    while(1);
}
