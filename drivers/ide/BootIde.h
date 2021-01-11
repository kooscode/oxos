/* Cromwell IDE driver code - GNU GPL */

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef __BOOTIDE_H__
#define __BOOTIDE_H__
#include "boot.h"
#include "include/BootLogger.h"

tsHarddiskInfo tsaHarddiskInfo[2];  // static struct stores data about attached drives

typedef enum {
    IDE_CMD_NOOP = 0,
    IDE_CMD_RECALIBRATE = 0x10,
    IDE_CMD_READ_MULTI_RETRY = 0x20,
    IDE_CMD_READ_MULTI = IDE_CMD_READ_MULTI_RETRY,
    IDE_CMD_READ_MULTI_NORETRY = 0x21,
    
    IDE_CMD_READ_EXT = 0x24, /* 48-bit LBA */
    IDE_CMD_READ_DMA_EXT = 0x25,
    IDE_CMD_READ_MULTIPLE_EXT = 0x29,
    IDE_CMD_WRITE_MULTI_RETRY = 0x30,
    IDE_CMD_WRITE_EXT = 0x34
    ,
    IDE_CMD_WRITE_DMA_EXT = 0x35,
    IDE_CMD_WRITE_MULTIPLE_EXT = 0x39,
        
    IDE_CMD_DRIVE_DIAG = 0x90,
    IDE_CMD_SET_PARAMS = 0x91,
    IDE_CMD_STANDBY_IMMEDIATE = 0x94, /* 2 byte command- also send
                                         IDE_CMD_STANDBY_IMMEDIATE2 */

    IDE_CMD_SMART = 0xB0,
    IDE_CMD_WRITE_MULTIPLE = 0xC5,
    IDE_CMD_READ_MULTIPLE = 0xC4,
    IDE_CMD_SET_MULTIPLE_MODE = 0xC6,
    IDE_CMD_READ_DMA = 0xC8,
    IDE_CMD_WRITE_DMA = 0xCA,
    IDE_CMD_STANDBY_IMMEDIATE2 = 0xE0,
    IDE_CMD_CACHE_FLUSH = 0xE7,
    IDE_CMD_CACHE_FLUSH_EXT = 0xEA,
    
    //Get info commands
    IDE_CMD_IDENTIFY = 0xEC,
    IDE_CMD_PACKET_IDENTIFY = 0xA1,
        
    ATAPI_SOFT_RESET = 0x08,
    
    IDE_CMD_SET_FEATURES = 0xEF,

    IDE_CMD_ATAPI_PACKET = 0xA0,

    //IDE security commands
    IDE_CMD_SECURITY_SET_PASSWORD = 0xF1,
    IDE_CMD_SECURITY_UNLOCK = 0xF2,
    IDE_CMD_SECURITY_DISABLE = 0xF6    
} ide_command_t;

typedef struct {                 //PRD table entry. 8 bytes in length
    unsigned int address;
    unsigned short byteCount;
    unsigned short reserved : 15;
    unsigned short endoftable : 1;
}__attribute__((packed))PRD;    //No filling. SHouldn't matter since its elements makes it already aligned.

typedef struct PRDT{
    PRD __attribute__((aligned(4)))PRD1;      //All aligned on a Dword boundary
    PRD __attribute__((aligned(4)))PRD2;
    PRD __attribute__((aligned(4)))PRD3;
    PRD __attribute__((aligned(4)))PRD4;
}__attribute__((packed, aligned(64 * 1024))) PRDTable;  //Aligned on a 64K boundary.
                                                        //So no crossing unless struct is > 64K in size.


//Methods
int BootIdeInit(void);
int BootIdeReadSector(int nDriveIndex, void * pbBuffer, unsigned int block, int byte_offset, int n_bytes) ;
int BootIdeReadData(unsigned uIoBase, void * buf, size_t size);
int DriveSecurityChange(unsigned uIoBase, int driveId, ide_command_t ide_cmd, unsigned char *password);
int CalculateDrivePassword(int driveId, unsigned char *key, unsigned char *eepromPtr);
bool driveMasterPasswordUnlock(unsigned uIoBase, int driveId, const char *master_password);
int BootIdeWriteSector(int nDriveIndex, const void * pbBuffer, unsigned int block, unsigned char retry);
int BootIdeWriteMultiple(int nDriveIndex, void * pbBuffer, unsigned int startLBA, unsigned short len, unsigned char retry);

int BootIdeFlushCache(int nDriveIndex);
unsigned long long BootIdeGetSectorCount(int nDriveIndex);
int BootIdeGetSectorSize(int nDriveIndex);
int BootIdeDeviceConnected(int nDriveIndex);
int BootIdeDeviceIsATAPI(int nDriveIndex);
int BootIdeDeviceIsLocked(int nDriveIndex);

bool driveToggleSMARTFeature(int nDriveIndex, unsigned short smart_cmd);
int driveSMARTRETURNSTATUS(int nDriveIndex);

int HDD_SECURITY_SendATACommand(int nIndexDrive, ide_command_t ATACommand, char * password, bool masterPassword);

#endif
