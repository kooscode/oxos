/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "boot.h"
#include "BootHddKey.h"
#include "BootEEPROM.h"
#include "i2c.h"
#include "rc4.h"
#include "cromwell.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "include/BootLogger.h"
#include "RecoveryImages.h"
#include "EEPROMStrings.h"

void BootEepromReadEntireEEPROM()
{
    int i;
    unsigned char *pb=(unsigned char *)&eeprom;
    for(i = 0; i < sizeof(EEPROMDATA); i++)
    {
        *pb++ = I2CTransmitByteGetReturn(0x54, i);
    }
}

void BootEepromReloadEEPROM(EEPROMDATA * realeeprom)
{
    memcpy(realeeprom, &origEeprom, sizeof(EEPROMDATA));
}

void BootEepromCompareAndWriteEEPROM(EEPROMDATA * realeeprom)
{
    int i;
    unsigned char *pb = (unsigned char *)&eeprom;
    unsigned char *pc = (unsigned char *)realeeprom;
    for(i = 0; i < sizeof(EEPROMDATA); i++)
    {
        if(pb[i] != pc[i])                //Compare byte by byte.
        {
            WriteToSMBus(0x54,i,1,pb[i]);          //Physical EEPROM's content is different from what's held in memory.
        }
    }
}

void BootEepromPrintInfo()
{
    VIDEO_ARGB=0xffc8c8c8;
    printk("\n           MAC : ");
    VIDEO_ARGB=0xffc8c800;
    printk("%02X%02X%02X%02X%02X%02X  ",
        eeprom.MACAddress[0], eeprom.MACAddress[1], eeprom.MACAddress[2],
        eeprom.MACAddress[3], eeprom.MACAddress[4], eeprom.MACAddress[5]
    );

    VIDEO_ARGB=0xffc8c8c8;
    printk("Vid: ");
    VIDEO_ARGB=0xffc8c800;

    switch(*((EEPROM_VideoStandard *)&eeprom.VideoStandard))
    {
        case EEPROM_VideoStandardInvalid:
            printk(Gameregiontext[EEPROM_XBERegionInvalid]);
            break;
        case EEPROM_VideoStandardNTSC_M:
            printk(Gameregiontext[EEPROM_XBERegionNorthAmerica]);
            break;
        case EEPROM_VideoStandardNTSC_J:
            printk(Gameregiontext[EEPROM_XBERegionJapan]);
            break;
        case EEPROM_VideoStandardPAL_I:
            printk(Gameregiontext[EEPROM_XBERegionEuropeAustralia]);
            break;
        default:
            printk("%08X", (int)*((EEPROM_VideoStandard *)&eeprom.VideoStandard));
            break;
    }

    VIDEO_ARGB=0xffc8c8c8;
    printk("  Serial: ");
    VIDEO_ARGB=0xffc8c800;
    
    {
        char sz[13];
        memcpy(sz, &eeprom.SerialNumber[0], 12);
        sz[12]='\0';
        printk(" %s", sz);
    }

    printk("\n");
    VIDEO_ARGB=0xffc8c8c8;
}

void BootEepromWriteEntireEEPROM(void)
{
    int i;
    unsigned char *pb=(unsigned char *)&eeprom;

    for(i = 0; i < sizeof(EEPROMDATA); i++)
    {
        WriteToSMBus(0x54,i,1,pb[i]);
    }
}

/* The EepromCRC algorithm was obtained from the XKUtils 0.2 source released by 
 * TeamAssembly under the GNU GPL.  
 * Specifically, from XKCRC.cpp 
 *
 * Rewritten to ANSI C by David Pye (dmp@davidmpye.dyndns.org)
 *
 * Thanks! */
void EepromCRC(unsigned char *crc, unsigned char *data, long dataLen)
{
    unsigned char CRC_Data[0x5b + 4];
    int pos=0;
    memset(crc,0x00,4);

    memset(CRC_Data,0x00, dataLen+4);
    //Circle shift input data one byte right
    memcpy(CRC_Data + 0x01 , data, dataLen-1);
    memcpy(CRC_Data, data + dataLen-1, 0x01);

    for (pos=0; pos<4; ++pos)
    {
        unsigned short CRCPosVal = 0xFFFF;
        unsigned long l;
        for (l=pos; l<dataLen; l+=4)
        {
            CRCPosVal -= *(unsigned short*)(&CRC_Data[l]);
        }
        CRCPosVal &= 0xFF00;
        crc[pos] = (unsigned char) (CRCPosVal >> 8);
    }
}

void EepromSetVideoStandard(EEPROM_VideoStandard standard)
{
    //Changing this setting requires that Checksum2
    //be recalculated.

    memcpy(eeprom.VideoStandard, &standard, 0x04);
    EepromCRC(eeprom.Checksum2,eeprom.SerialNumber,0x28);
}

void EepromSetVideoFormat(EEPROM_VidScreenFormat format)
{
    eeprom.VideoFlags[2] &= ~(EEPROM_VidScreenFullScreen | EEPROM_VidScreenWidescreen | EEPROM_VidScreenLetterbox);
    eeprom.VideoFlags[2] |= format;
    EepromCRC(eeprom.Checksum3,eeprom.TimeZoneBias,0x5b);
}

int getGameRegionValue(EEPROMDATA * eepromPtr)
{
    int result = -1;
    unsigned char baEepromDataLocalCopy[0x30];
    int version = 0;
    unsigned int gameRegion=0;

    version = decryptEEPROMData((unsigned char *)eepromPtr, baEepromDataLocalCopy);

    if(version > EEPROM_EncryptV1_6)
    {
        result = EEPROM_XBERegionInvalid;
    }
    else
    {
        memcpy(&gameRegion,&baEepromDataLocalCopy[28+16],4);
        if(gameRegion == EEPROM_XBERegionJapan)
        {
            result = EEPROM_XBERegionJapan;
        }
        else if(gameRegion == EEPROM_XBERegionEuropeAustralia)
        {
            result = EEPROM_XBERegionEuropeAustralia;
        }
        else if(gameRegion == EEPROM_XBERegionNorthAmerica)
        {
            result = EEPROM_XBERegionNorthAmerica;
        }
        else
        {
            result = EEPROM_XBERegionInvalid;
        }
    }
    return result;
}

int setGameRegionValue(unsigned char value)
{
    int result = -1;
    unsigned char baKeyHash[20];
    unsigned char baDataHashConfirm[20];
    unsigned char baEepromDataLocalCopy[0x30];
    struct rc4_key RC4_key;
    int version = 0;
    int counter;
    unsigned int gameRegion = value;

    version = decryptEEPROMData((unsigned char *)&eeprom, baEepromDataLocalCopy);

    if (version > EEPROM_EncryptV1_6)
    {
        return (-1);    //error, let's not do something stupid here. Leave with dignity.
    }

    //else we know the version
    memcpy(&baEepromDataLocalCopy[28+16],&gameRegion,4);
    
    encryptEEPROMData(baEepromDataLocalCopy, &eeprom, version);

    //Everything went well, return new gameRegion.
    return gameRegion;

}

EEPROM_EncryptVersion EepromSanityCheck(EEPROMDATA * eepromPtr)
{
    EEPROMDATA decryptBuf;
    EEPROM_EncryptVersion version = decryptEEPROMData((unsigned char *)eepromPtr, (unsigned char *)&decryptBuf);
    BootLogger(DEBUG_EEPROM_DRIVER, DBG_LVL_DEBUG, "Encrypt version = %u", version);
    if(version >= EEPROM_EncryptV1_0 && version <= EEPROM_EncryptV1_6)
    {
        unsigned int fourBytesParam = *(unsigned int *)(decryptBuf.XBERegion);
        BootLogger(DEBUG_EEPROM_DRIVER, DBG_LVL_DEBUG, "XBE Region = %u", fourBytesParam);
        if(fourBytesParam != EEPROM_XBERegionEuropeAustralia &&
           fourBytesParam != EEPROM_XBERegionJapan &&
           fourBytesParam != EEPROM_XBERegionNorthAmerica)
        {
            return EEPROM_EncryptInvalid;
        }

        fourBytesParam = *(unsigned int *)(eepromPtr->VideoStandard);
        BootLogger(DEBUG_EEPROM_DRIVER, DBG_LVL_DEBUG, "Video Standard = %08X", fourBytesParam);
        if(fourBytesParam != EEPROM_VideoStandardNTSC_J &&
           fourBytesParam != EEPROM_VideoStandardNTSC_M &&
           fourBytesParam != EEPROM_VideoStandardPAL_I)
        {
            return EEPROM_EncryptInvalid;
        }

        BootLogger(DEBUG_EEPROM_DRIVER, DBG_LVL_DEBUG, "Video flags = %02X %02X %02X %02X", eepromPtr->VideoFlags[0],
                                                             eepromPtr->VideoFlags[1],
                                                             eepromPtr->VideoFlags[2],
                                                             eepromPtr->VideoFlags[3]);

        if(eepromPtr->VideoFlags[2] >= (EEPROM_VidResolutionEnable720p |
                                        EEPROM_VidResolutionEnable1080i |
                                        EEPROM_VidResolutionEnable480p |
                                        EEPROM_VidScreenFullScreen |
                                        EEPROM_VidScreenWidescreen |
                                        EEPROM_VidScreenLetterbox))
        {
            return EEPROM_EncryptInvalid;
        }

        BootLogger(DEBUG_EEPROM_DRIVER, DBG_LVL_DEBUG, "DVD playback zone = %02X %02X %02X %02X", eepromPtr->DVDPlaybackKitZone[0],
                                                                   eepromPtr->DVDPlaybackKitZone[1],
                                                                   eepromPtr->DVDPlaybackKitZone[2],
                                                                   eepromPtr->DVDPlaybackKitZone[3]);
        if(eepromPtr->DVDPlaybackKitZone[1] != 0 || eepromPtr->DVDPlaybackKitZone[2] != 0 || eepromPtr->DVDPlaybackKitZone[3] != 0)
        {
            return EEPROM_EncryptInvalid;
        }

        if(eepromPtr->DVDPlaybackKitZone[0] > EEPROM_DVDRegionAirlines)
        {
            return EEPROM_EncryptInvalid;
        }

        return version;
    }

    return EEPROM_EncryptInvalid;
}

EEPROM_EncryptVersion decryptEEPROMData(unsigned char* eepromPtr, unsigned char* decryptedBuf)
{
   struct rc4_key RC4_key;
   EEPROM_EncryptVersion version = EEPROM_EncryptInvalid;
   EEPROM_EncryptVersion counter;
   unsigned char baEepromDataLocalCopy[0x30];
   unsigned char baKeyHash[20];
   unsigned char baDataHashConfirm[20];

    // Static Version change not included yet

    for (counter=EEPROM_EncryptV1_0;counter<=EEPROM_EncryptV1_6;counter++)
    {
        memset(&RC4_key,0,sizeof(rc4_key));
        memcpy(&baEepromDataLocalCopy[0], eepromPtr, 0x30);

                // Calculate the Key-Hash
        HMAC_hdd_calculation(counter, baKeyHash, &baEepromDataLocalCopy[0], 20, NULL);

        //initialize RC4 key
        rc4_prepare_key(baKeyHash, 20, &RC4_key);

            //decrypt data (from eeprom) with generated key
        rc4_crypt(&baEepromDataLocalCopy[20],28,&RC4_key);        //confounder, HDDkey and game region decryption in single block
        //rc4_crypt(&baEepromDataLocalCopy[28],20,&RC4_key);        //"real" data

                // Calculate the Confirm-Hash
        HMAC_hdd_calculation(counter, baDataHashConfirm, &baEepromDataLocalCopy[20], 8, &baEepromDataLocalCopy[28], 20, NULL);

        if (!memcmp(baEepromDataLocalCopy,baDataHashConfirm,0x14))
        {
            // Confirm Hash is correct
            // Copy actual Xbox Version to Return Value
            version=counter;
            // exits the loop
            break;
        }
    }

    //copy out decrypted Confounder, HDDKey and gameregion.
    if(decryptedBuf != NULL)
    {
        memcpy(decryptedBuf, baEepromDataLocalCopy, 0x30);
    }

    BootLogger(DEBUG_EEPROM_DRIVER, DBG_LVL_INFO, "EEPROM decrypt %s!!   Version value : %u", counter > EEPROM_EncryptV1_6 ? "failure" : "success", version);

    return version;
}

void encryptEEPROMData(unsigned char decryptedInput[0x30], EEPROMDATA * targetEEPROMPtr, EEPROM_EncryptVersion targetVersion)
{
    unsigned char baKeyHash[20];
    unsigned char baDataHashConfirm[20];
    struct rc4_key RC4_key;

    memset(&RC4_key,0,sizeof(rc4_key));
    memset(baKeyHash,0,20);
    memset(baDataHashConfirm,0,20);

    // Calculate the Confirm-Hash
    HMAC_hdd_calculation(targetVersion, baDataHashConfirm, &decryptedInput[20], 8, &decryptedInput[28], 20, NULL);

    memcpy(decryptedInput,baDataHashConfirm,20);

    // Calculate the Key-Hash
    HMAC_hdd_calculation(targetVersion, baKeyHash, &decryptedInput[0], 20, NULL);

    //initialize RC4 key
    rc4_prepare_key(baKeyHash, 20, &RC4_key);

    //decrypt data (from eeprom) with generated key
    rc4_crypt(&decryptedInput[20],28,&RC4_key);

    // Save back to EEprom
    memcpy(targetEEPROMPtr, &decryptedInput[0], 0x30);
}

const char* getGameRegionText(EEPROM_XBERegion gameRegion)
{
    return Gameregiontext[gameRegion];
}

const char* getDVDRegionText(EEPROM_DVDRegion dvdRegion)
{
    return DVDregiontext[dvdRegion];
}

const char* getVideoStandardText(EEPROM_VideoStandard vidStandard)
{
    //Re-use XBE region strings as they are the same
    switch(vidStandard)
    {
    case EEPROM_VideoStandardNTSC_M:
        return Gameregiontext[EEPROM_XBERegionNorthAmerica];
    case EEPROM_VideoStandardNTSC_J:
        return Gameregiontext[EEPROM_XBERegionJapan];
    case EEPROM_VideoStandardPAL_I:
        return Gameregiontext[EEPROM_XBERegionEuropeAustralia];
    case EEPROM_VideoStandardInvalid:
        /* Fall through */
    default:
        break;
    }

    return Gameregiontext[EEPROM_XBERegionInvalid];
}

const char* getScreenFormatText(EEPROM_VidScreenFormat vidFormat)
{
    switch(vidFormat & (EEPROM_VidScreenWidescreen | EEPROM_VidScreenLetterbox | EEPROM_VidScreenFullScreen))
    {
    case EEPROM_VidScreenWidescreen:
        return Vidformattext[1];
    case EEPROM_VidScreenLetterbox:
        return Vidformattext[2];
    case EEPROM_VidScreenFullScreen:
        /* Fall through */
    default:
        break;
    }

    return Vidformattext[0];
}
