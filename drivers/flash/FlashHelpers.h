/*
 * Helpers.h
 *
 *  Created on: Jul 3, 2017
 *      Author: cromwelldev
 */

#ifndef HELPERS_H_
#define HELPERS_H_

#include <stdbool.h>

typedef struct {
    unsigned char m_bManufacturerId;
    unsigned char m_bDeviceId;
     char m_szFlashDescription[32];
    unsigned int m_dwLengthInBytes;
    unsigned char m_support4KBErase;
} KNOWN_FLASH_TYPE;

typedef struct {
    volatile unsigned char * volatile m_pbMemoryMappedStartAddress; // fill on entry
    KNOWN_FLASH_TYPE flashType;
    char m_szAdditionalErrorInfo[256];
    bool m_fIsBelievedCapableOfWriteAndErase;
} OBJECT_FLASH;

OBJECT_FLASH flashDevice;

typedef enum
{
    FlashOp_Idle,
    FlashOp_PendingOp,
    FlashOp_EraseInProgress,
    FlashOp_WriteInProgress,
    FlashOp_VerifyInProgress,
    FlashOp_ReadInProgress,
    FlashOp_Completed,
    FlashOp_Error
} FlashOp;

typedef enum
{
    FlashErrorcodes_NoError = 0U,
    FlashErrorcodes_UserAbort,
    FlashErrorcodes_FailedErase,
    FlashErrorcodes_FailedProgram,
    FlashErrorcodes_FailedVerify,
    FlashErrorcodes_UnknownFlash,
    FlashErrorcodes_WriteProtect,
    FlashErrorcodes_FileSizeError,
    FlashErrorcodes_InvalidUpdateFile,
    FlashErrorcodes_MD5Mismatch,
    FlashErrorcodes_FlashContentError,
    FlashErrorcodes_DowngradeWarning,
    FlashErrorcodes_UndefinedError
} FlashErrorcodes;


#endif /* HELPERS_H_ */
