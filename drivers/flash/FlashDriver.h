/*
 * FlashDriver.h
 *
 *  Created on: Dec 5, 2016
 *      Author: cromwelldev
 */

#ifndef FLASHDRIVER_H_
#define FLASHDRIVER_H_

#include "FlashHelpers.h"
#include "BiosIdentifier.h"
#include "xblast/settings/xblastSettingsDefs.h"
#include "lpcmod_v1.h"

typedef enum
{
    FlashTask_NoTask,
    FlashTask_WriteBios,
    FlashTask_ReadBios,
    FlashTask_WriteSettings,
    FlashTask_ReadSettings
} FlashTask;

typedef struct
{
    FlashOp currentFlashOp;
    FlashTask currentFlashTask;
    unsigned char progressInPercent;
    FlashErrorcodes flashErrorCode;
} FlashProgress;

// Call at system init
void Flash_Init(void);

// Call perpetually
void Flash_executeFlashFSM(void);

// Call once operation is over
void Flash_freeFlashFSM(void);

void Flash_forceUserAbort(void);

FlashProgress Flash_getProgress(void);

// Read flash device info. Returns immediately
FlashProgress Flash_ReadDeviceInfo(const OBJECT_FLASH* *const output);

// Initiates a read operation. Call regularly after to get operation state
FlashProgress Flash_ReadBIOSBank(FlashBank bank);
// Call once read is over
unsigned int getBiosBuffer(const unsigned char* *const output);

// Write calls
FlashProgress Flash_XBlastOSBankFlash(const unsigned char* inBuf, unsigned int size, unsigned int offset, bool overrideChecks);
FlashProgress Flash_XBlastUserBankFlash(const unsigned char* inBuf, unsigned int size, unsigned int offset, FlashBank bank);
FlashProgress Flash_SimpleBIOSBankFlash(const unsigned char* inBuf, unsigned int size, unsigned int offset);

// To initiate read. Won't stall the system.
FlashProgress Flash_ReadXBlastOSSettingsRequest(void);
// To call once getting data from flash is done.
bool Flash_LoadXBlastOSSettings(_LPCmodSettings* input);

bool bootReadXBlastOSSettings(void);

FlashProgress Flash_SaveXBlastOSSettings(void);

struct BiosIdentifier getBiosIdentifierFromFlash(void);

#ifdef DEV_FEATURES
unsigned int getBiosBufferSize(void);
unsigned int getStartingOffset(void);
unsigned int getCurrentAddr(void);
unsigned int getEraseSequenceMethod(void);
bool getFirstEraseTry(void);
#endif

#endif /* FLASHDRIVER_H_ */
