/*
 * FlashUi.c
 *
 *  Created on: Dec 12, 2016
 *      Author: cromwelldev
 */

#include "FlashUi.h"
#include "boot.h"
#include "FlashDriver.h"
#include "memory_layout.h"
#include "video.h"
// #include "include/lpcmod_v1.h"
#include "md5.h"
#include "i2c.h"
#include "cromwell.h"
#include "string.h"
#include "stdio.h"
#include "lib/LPCMod/BootLPCMod.h"
#include "lib/cromwell/cromSystem.h"
#include "Gentoox.h"
#include "FlashMenuActions.h"
#include "menu/misc/ConfirmDialog.h"
#include "menu/misc/ProgressBar.h"
// #include "xblast/HardwareIdentifier.h"
// #include "xblast/settings/xblastSettingsChangeTracker.h"
#ifdef DEV_FEATURES
#include "MenuActions.h"
#endif

static void BootFlashUserInterface(FlashOp ee, unsigned int dwPos, unsigned int dwExtent);
static void setBiosJob(const unsigned char *data, unsigned int size, bool askConfirm);
static bool FlashPrintResult(void);
static void blockExecuteFlashJob(void);

static bool mustRestart = false;
static unsigned char previousPercent = 0;

//Selects which function should be called for flashing.
void FlashFileFromBuffer(unsigned char *fileBuf, unsigned int fileSize, bool askConfirm)
{
    BootLogger(DEBUG_FLASH_UI, DBG_LVL_INFO, "New image to flash. size=%u", fileSize);

    if(Flash_getProgress().currentFlashOp == FlashOp_Idle)
    {
        setBiosJob(fileBuf, fileSize, askConfirm);
    }

    blockExecuteFlashJob();
}


 // callback to show progress
static void BootFlashUserInterface(FlashOp ee, unsigned int dwPos, unsigned int dwExtent)
{
    if(ee==FlashOp_EraseInProgress)
    {
        DisplayProgressBar(dwPos,dwExtent,0xffffff00);
    }
    else if(ee==FlashOp_WriteInProgress)
    {
        DisplayProgressBar(dwPos,dwExtent,0xff00ff00);
    }
    else if(ee==FlashOp_VerifyInProgress)
    {
         DisplayProgressBar(dwPos,dwExtent,0xffff00ff);
    }
    else if(ee==FlashOp_ReadInProgress)
    {
         DisplayProgressBar(dwPos,dwExtent,0xffff00ff);
    }
}

void BootShowFlashDevice(void)
{
    const OBJECT_FLASH* of = NULL;

    FlashProgress progress;
    progress = Flash_getProgress();

    if(progress.currentFlashOp != FlashOp_Idle)
    {
        VIDEO_ATTR=0xffc8c8c8;
        printk("\n           Flash is unavailable.");
    }

    progress = Flash_ReadDeviceInfo(&of);

    if(progress.currentFlashOp == FlashOp_Error)
    {
        VIDEO_ATTR=0xffc8c8c8;
        printk("\n           No valid Flash device Detected!!!");
        return;
    }


    VIDEO_ATTR=0xffc8c8c8;
    printk("\n           Manufacturer ID : ");
    VIDEO_ATTR=0xffc8c800;
    printk("%02X", of->flashType.m_bManufacturerId);
    VIDEO_ATTR=0xffc8c8c8;
    printk("\n           Device ID : ");
    VIDEO_ATTR=0xffc8c800;
    printk("%02X", of->flashType.m_bDeviceId);
    VIDEO_ATTR=0xffc8c8c8;
    printk("\n           Name : ");
    VIDEO_ATTR=0xffc8c800;
    printk("%s", of->flashType.m_szFlashDescription);
    VIDEO_ATTR=0xffc8c8c8;
    printk("\n           Total size : ");
    VIDEO_ATTR=0xffc8c800;
    printk("%u KB", of->flashType.m_dwLengthInBytes / 1024);

    return;
}

static bool FlashPrintResult(void)
{
    bool isError = false;
    bool isCritical = false;
    const char* string;

    BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
    VIDEO_ATTR=0xffef37;

#ifndef DEV_FEATURES
    if(mustRestart == true)
    {
        BootLogger(DEBUG_FLASH_UI, DBG_LVL_INFO, "Flash update sequence restart system");
        // Set LED to oxox.
        inputLED();
        Flash_freeFlashFSM();
        I2CRebootSlow();
        while(1);
    }
#endif

    FlashErrorcodes res = Flash_getProgress().flashErrorCode;

    if(res == FlashErrorcodes_NoError)
    {
        printk("\n           \2Flashing success...\n\2\n\n\n");
        VIDEO_ATTR=0xffffff;
        printk("           ");
        cromwellSuccess();
        printk ("\n           Flashing successful!!!");
    }
    else
    {
        printk("\n           \2Flashing failed...\n\2\n\n\n");
        VIDEO_ATTR=0xffffff;
        switch (res)
        {
        case FlashErrorcodes_UserAbort:
            string = "Flashing aborted.";
            break;
        case FlashErrorcodes_MD5Mismatch:
            string = "MD5 mismatch.";
            break;
        case FlashErrorcodes_InvalidUpdateFile:
            string = "Invalid XBlast OS update file.";
            break;
        case FlashErrorcodes_UnknownFlash:
            string = "Unknown flash device.\n           Write-Protect is enabled?";
            break;
        case FlashErrorcodes_WriteProtect:
            string = "Cannot write to device.";
            break;
        case FlashErrorcodes_FileSizeError:
            string = "File size error.";
            break;
        case FlashErrorcodes_FailedErase:
            string = "Erasing failed, please reflash.";
            isError = true;
            isCritical = true;
            break;
        case FlashErrorcodes_FailedProgram:
            string = "Programming failed, please reflash.";
            isError = true;
            isCritical = true;
            break;
        case FlashErrorcodes_FlashContentError:
            string = "Active flash bank does not contain XBlast OS image.\n           Not saving.";
            isError = true;
            break;
        case FlashErrorcodes_UndefinedError:
        default:
            string = "Unknown error! Congrats, you're not supposed to be here.";
            isError = true;
            break;
        }

        printk("           ");
        if(isError == true)
        {
            cromwellError();
        }
        else
        {
            cromwellWarning();
        }
        printk ("\n           %s", string);
    }

    FlashFooter();

    return isCritical;
}

bool SaveXBlastOSSettings(void)
{
    bool resultSuccess = false;

    if(memcmp(&LPCmodSettings, &LPCmodSettingsOrigFromFlash, sizeof(_LPCmodSettings)) == 0)
    {
        BootLogger(DEBUG_FLASH_UI, DBG_LVL_INFO, "No setting changed since last boot. Skipping save to flash.");
        return true;
    }

    if(isXBlastOnTSOP())
    {
        BootLogger(DEBUG_FLASH_UI, DBG_LVL_INFO, "XBlast detected but running from TSOP. Can't save to LPC flash.");
        return true;
    }

    if(isXBlastCompatible() == false && isXBE())
    {
        BootLogger(DEBUG_FLASH_UI, DBG_LVL_INFO, "No XBlast HW detected.Came from XBE. Assume no flash to same to.");
        return true;
    }

    switchOSBank(FlashBank_OSBank);

    FlashProgress flashProgress = Flash_SaveXBlastOSSettings();

    while(cromwellLoop())
    {
        flashProgress = Flash_getProgress();

        if(flashProgress.currentFlashOp == FlashOp_Idle)
        {
            break;
        }
        resultSuccess = executeFlashDriverUI();
        Flash_executeFlashFSM();
    }

    return resultSuccess;
}

static void setBiosJob(const unsigned char *data, unsigned int size, bool askConfirm)
{
    FlashProgress flashProgress;
    flashProgress.flashErrorCode = FlashErrorcodes_UserAbort;
    flashProgress.currentFlashOp = FlashOp_Error;
    char * stringTemp;

    if(isXBlastOnLPC())
    {
        if (currentFlashBank == FlashBank_OSBank)
        {
            if (askConfirm == false || ConfirmDialog("Confirm update XBlast OS?", 1) == false)
            {
                flashProgress = Flash_XBlastOSBankFlash(data, size, 0, false);

                if(flashProgress.flashErrorCode == FlashErrorcodes_DowngradeWarning)
                {
                    if(ConfirmDialog("Downgrade XBlast OS version?", 1) == false)
                    {
                        Flash_freeFlashFSM();
                        flashProgress = Flash_XBlastOSBankFlash(data, size, 0, true);
                    }
                    else
                    {
                        Flash_forceUserAbort();
                    }
                }

                if(flashProgress.currentFlashOp == FlashOp_PendingOp)
                {
                    mustRestart = true;
                }
            }
            else
            {
                Flash_forceUserAbort();
            }
        }
        else
        {
            if (currentFlashBank == FlashBank_512Bank)
            {
                stringTemp = "Confirm flash bank0(512KB)?";
            }
            else if (currentFlashBank == FlashBank_256Bank)
            {
                stringTemp = "Confirm flash bank1(256KB)?";
            }
            else if (currentFlashBank == FlashBank_SplitTSOP0Bank)
            {
                stringTemp = "Confirm TSOP bank0(512KB)?";
            }
            else if (currentFlashBank == FlashBank_SplitTSOP1Bank)
            {
                stringTemp = "Confirm TSOP bank1(512KB)?";
            }
            else if (currentFlashBank == FlashBank_FullTSOPBank)
            {
                stringTemp = "Confirm TSOP (whole)?";
            }
            //The case below shouldn't happen.
            else
            {
                stringTemp = "Confirm flash bank?";
            }

            if (askConfirm == false || ConfirmDialog(stringTemp, 1) == false)
            {
                flashProgress = Flash_XBlastUserBankFlash(data, size, 0, currentFlashBank);
            }
            else
            {
                Flash_forceUserAbort();
            }
        }
    }
    else  //If no XBlast mod
    {
        if (askConfirm  == false || ConfirmDialog("Confirm flash active bank?", 1) == false)
        {
            mustRestart = true;
            flashProgress = Flash_SimpleBIOSBankFlash(data, size, 0);
        }
        else
        {
            Flash_forceUserAbort();
        }
    }
}

static void blockExecuteFlashJob(void)
{
    FlashProgress flashProgress = Flash_getProgress();

    while(cromwellLoop())
    {
        flashProgress = Flash_getProgress();

        if(flashProgress.currentFlashOp == FlashOp_Idle)
        {
            break;
        }

        executeFlashDriverUI();
        Flash_executeFlashFSM();
    }

    switchOSBank(FlashBank_OSBank);
}

bool executeFlashDriverUI(void)
{
    bool resultSuccess = true;
    FlashProgress flashProgress = Flash_getProgress();

    switch(flashProgress.currentFlashTask)
    {
    case FlashTask_WriteBios:
        if(flashProgress.currentFlashOp == FlashOp_PendingOp)
        {
            BootLogger(DEBUG_FLASH_UI, DBG_LVL_DEBUG, "Flash update sequence pending op");
            VIDEO_ATTR=0xffef37;

            if(mustRestart)
            {
                printk("\n           \2%s\n\2\n", (isXBlastOnLPC())?"Updating XBlast OS...":"Updating flash bank...");
                VIDEO_ATTR=0xffffff;
                printk("\n\n\n           WARNING!\n");
                printk("           Do not turn off your console during this process!\n");
                printk("           Your console should automatically reboot when this\n");
                printk("           is done.  However, if it does not, please manually\n");
                printk("           do so by pressing the power button once the LED has\n");
                printk("           turned flashing amber (oxox)\n");
            }
            else
            {
                printk("\n           \2Updating BIOS bank...\n\2\n");
                VIDEO_ATTR=0xffffff;
                printk("\n\n\n           WARNING!\n");
                printk("           Do not turn off your console during this process!\n");
            }
        }
        else if(flashProgress.currentFlashOp == FlashOp_Completed)
        {
            BootLogger(DEBUG_FLASH_UI, DBG_LVL_DEBUG, "Flash update sequence completed");
            FlashPrintResult();
            Flash_freeFlashFSM();
        }
        else if(flashProgress.currentFlashOp == FlashOp_Error)
        {
            BootLogger(DEBUG_FLASH_UI, DBG_LVL_ERROR, "!!Flash update sequence error!! errorCode=%u", flashProgress.flashErrorCode);
            FlashPrintResult();
            Flash_freeFlashFSM();
        }
        else
        {
            if(previousPercent != flashProgress.progressInPercent)
            {
                previousPercent = flashProgress.progressInPercent;
                BootFlashUserInterface(flashProgress.currentFlashOp, flashProgress.progressInPercent, 100);
            }
        }
        break;
    case FlashTask_WriteSettings:
    {
        static char previousPercent = -1;
        static FlashOp previousFlashOp = FlashOp_PendingOp;

        if(previousPercent == -1)
        {
            BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            printk("\n\n           Saving Settings to flash.");
            printk("\n           Do not manually power off your Xbox.");
            printk("\n\n           ");
            previousPercent = 0;
        }
        if(previousPercent != flashProgress.progressInPercent)
        {
            previousPercent = flashProgress.progressInPercent;
            printk(".");
        }

        if(previousFlashOp != flashProgress.currentFlashOp)
        {
            previousFlashOp = flashProgress.currentFlashOp;
            switch(flashProgress.currentFlashOp)
            {
            case FlashOp_EraseInProgress:
                printk("\n\n           Erasing\n           ");
                break;
            case FlashOp_WriteInProgress:
                printk("\n\n           Writing\n           ");
                break;
            case FlashOp_VerifyInProgress:
                printk("\n\n           Verifying\n           ");
                break;
            default:
                break;
            }
        }

        if(flashProgress.currentFlashOp == FlashOp_Completed)
        {
#ifdef DEV_FEATURES
            printk("\n\n\n          BiosBufferSize : %u", getBiosBufferSize());
            printk("\n          StartingOffset : %u", getStartingOffset());
            printk("\n          CurrentAddr : %u", getCurrentAddr());
            printk("\n          EraseSequenceMethod : %u", getEraseSequenceMethod());
            printk("\n          FirstEraseTry : %u", getFirstEraseTry());
            UIFooter();
#endif
            Flash_freeFlashFSM();
        }
        else if(flashProgress.currentFlashOp == FlashOp_Error)
        {
#ifdef DEV_FEATURES
            printk("\n\n          BiosBufferSize : %u", getBiosBufferSize());
            printk("\n          StartingOffset : %u", getStartingOffset());
            printk("\n          CurrentAddr : %u", getCurrentAddr());
            printk("\n          EraseSequenceMethod : %u", getEraseSequenceMethod());
            printk("\n          FirstEraseTry : %u", getFirstEraseTry());
            UIFooter();
#endif
            printk("\n\n\n\n\n           \2Save Settings to flash failed...\n\2\n");
            VIDEO_ATTR=0xffffff;
            resultSuccess = FlashPrintResult() == false;

            if(resultSuccess)
            {
                if(ConfirmDialog("Settings not saved.\n\2Continue anyway?", 1))
                {
                    resultSuccess = false; //Do not continue
                }
            }
            Flash_freeFlashFSM();
        }
    }
        break;
    default:
        break;
    }

    return resultSuccess;
}
