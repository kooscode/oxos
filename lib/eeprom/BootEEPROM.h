/*
 * Parts are from the Team-Assembly XKUtils thx.
 *
 */

 /***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

 
#ifndef _BootEEPROM_H_
#define _BootEEPROM_H_

#include "VideoInitialization.h"
#include <stdbool.h>

//EEPROM Data struct value enum
typedef enum { 
    EEPROM_DVDRegion0        = 0x00, //Region Clear
    EEPROM_DVDRegion1        = 0x01, //USA
    EEPROM_DVDRegion2        = 0x02, //Europe
    EEPROM_DVDRegion3        = 0x03, //India
    EEPROM_DVDRegion4        = 0x04, //Australia
    EEPROM_DVDRegion5        = 0x05, //USSR
    EEPROM_DVDRegion6        = 0x06, //China
    EEPROM_DVDRegionFree     = 0x07, //Free
    EEPROM_DVDRegionAirlines = 0x08  //Airlines
} EEPROM_DVDRegion;

typedef enum {
    EEPROM_VideoStandardInvalid = 0x00000000,
    EEPROM_VideoStandardNTSC_M  = 0x00400100,
    EEPROM_VideoStandardNTSC_J  = 0x00400200,
    EEPROM_VideoStandardPAL_I   = 0x00800300
} EEPROM_VideoStandard;

typedef enum {
    EEPROM_XBERegionInvalid = 0x00,
    EEPROM_XBERegionNorthAmerica = 0x01,
    EEPROM_XBERegionJapan = 0x02,
    EEPROM_XBERegionEuropeAustralia = 0x04
} EEPROM_XBERegion;

typedef enum {
    EEPROM_VidScreenFullScreen = 0x00,
    EEPROM_VidScreenWidescreen = 0x01,
    EEPROM_VidScreenLetterbox  = 0x10
} EEPROM_VidScreenFormat;

typedef enum {
    EEPROM_VidResolutionEnable720p = 0x02,
    EEPROM_VidResolutionEnable1080i = 0x04,
    EEPROM_VidResolutionEnable480p = 0x08
}EEPROM_VidResolutionEnable;

typedef enum {
    EEPROM_EncryptInvalid = 0x00,
    EEPROM_EncryptV1_0    = 0x0A,
    EEPROM_EncryptV1_1    = 0x0B,
    EEPROM_EncryptV1_6    = 0x0C
} EEPROM_EncryptVersion;

//Xbox motherboard revision enum.
typedef enum {
    XboxMotherboardRevision_DEVKIT   = 0x00, //Includes a bunch of revisions
    XboxMotherboardRevision_DEBUGKIT = 0x01, //2 known version ID
    XboxMotherboardRevision_1_0      = 0x02, //1.0
    XboxMotherboardRevision_1_1      = 0x03, //1.1
    XboxMotherboardRevision_1_2      = 0x04, //1.2/1.3
    XboxMotherboardRevision_1_4      = 0x05, //1.4/1.5
    XboxMotherboardRevision_1_6      = 0x06, //1.6/1.6b
    XboxMotherboardRevision_UNKNOWN  = 0x07  //dafuk?
} XboxMotherboardRevision;

//Structure that holds contents of 256 byte EEPROM image..
typedef struct {
   unsigned char HMAC_SHA1_Hash[20];       // 0x00 - 0x13 HMAC_SHA1 Hash
   unsigned char Confounder[8];            // 0x14 - 0x1B RC4 Encrypted Confounder ??
   unsigned char HDDKkey[16];              // 0x1C - 0x2B RC4 Encrypted HDD key
   unsigned char XBERegion[4];             // 0x2C - 0x2F RC4 Encrypted Region code (0x01 North America, 0x02 Japan, 0x04 Europe)

   unsigned char Checksum2[4];             // 0x30 - 0x33 Checksum of next 44 bytes
   unsigned char SerialNumber[12];         // 0x34 - 0x3F Xbox serial number
   unsigned char MACAddress[6];            // 0x40 - 0x45 Ethernet MAC address
   unsigned char UNKNOWN2[2];              // 0x46 - 0x47  Unknown Padding ?

   unsigned char OnlineKey[16];            // 0x48 - 0x57 Online Key ?

   unsigned char VideoStandard[4];         // 0x58 - 0x5B  ** 0x00014000 = NTSC, 0x00038000 = PAL, 0x00400100 = EEPROM_VideoStandardNTSC_J

   unsigned char UNKNOWN3[4];              // 0x5C - 0x5F  Unknown Padding ?
  
   //Comes configured up to here from factory..  everything after this can be zero'd out...
   //To reset XBOX to Factory settings, Make checksum3 0xFFFFFFFF and zero all data below (0x64-0xFF)
   //Doing this will Reset XBOX and upon startup will get Language & Setup screen...
   unsigned char Checksum3[4];             // 0x60 - 0x63  other Checksum of next

   unsigned char TimeZoneBias[4];          // 0x64 - 0x67 Zone Bias?
   unsigned char TimeZoneStdName[4];       // 0x68 - 0x6B Standard timezone
   unsigned char TimeZoneDltName[4];       // 0x5C - 0x6F Daylight timezone
   unsigned char UNKNOWN4[8];              // 0x70 - 0x77 Unknown Padding ?
   unsigned char TimeZoneStdDate[4];       // 0x78 - 0x7B 10-05-00-02 (Month-Day-DayOfWeek-Hour)
   unsigned char TimeZoneDltDate[4];       // 0x7C - 0x7F 04-01-00-02 (Month-Day-DayOfWeek-Hour)
   unsigned char UNKNOWN5[8];              // 0x80 - 0x87 Unknown Padding ?
   unsigned char TimeZoneStdBias[4];       // 0x88 - 0x8B Standard Bias?
   unsigned char TimeZoneDltBias[4];       // 0x8C - 0x8F Daylight Bias?

   unsigned char LanguageID[4];            // 0x90 - 0x93 Language ID
  
   unsigned char VideoFlags[4];            // 0x94 - 0x97 Video Settings - 0x96 b0==widescreen 0x96 b4 == letterbox
   unsigned char AudioFlags[4];            // 0x98 - 0x9B Audio Settings
  
   unsigned char ParentalControlGames[4];  // 0x9C - 0x9F 0=MAX rating
   unsigned char ParentalControlPwd[4];    // 0xA0 - 0xA3 7=X, 8=Y, B=LTrigger, C=RTrigger
   unsigned char ParentalControlMovies[4]; // 0xA4 - 0xA7 0=Max rating
  
   unsigned char XBOXLiveIPAddress[4];     // 0xA8 - 0xAB XBOX Live IP Address..
   unsigned char XBOXLiveDNS[4];           // 0xAC - 0xAF XBOX Live DNS Server..
   unsigned char XBOXLiveGateWay[4];       // 0xB0 - 0xB3 XBOX Live Gateway Address..
   unsigned char XBOXLiveSubNetMask[4];    // 0xB4 - 0xB7 XBOX Live Subnet Mask..
   unsigned char OtherSettings[4];         // 0xA8 - 0xBB Other XBLive settings ?

   unsigned char DVDPlaybackKitZone[4];    // 0xBC - 0xBF DVD Playback Kit Zone

   unsigned char UNKNOWN6[64];             // 0xC0 - 0xFF Unknown Codes / Memory timing data ?
}__attribute__((packed)) EEPROMDATA;

EEPROMDATA eeprom;
EEPROMDATA origEeprom; //Populated at boot and nowhere else.

typedef enum {
    EEPROMModItem_HDDKkey = 0,
    EEPROMModItem_XBERegion,
    //Below is checksum2
    EEPROMModItem_SerialNumber,
    EEPROMModItem_MACAddress,
    EEPROMModItem_VideoStandard,
    //Below is checksum3
    EEPROMModItem_VideoFlags_Format,
    EEPROMModItem_VideoFlags_480p,
    EEPROMModItem_VideoFlags_720p,
    EEPROMModItem_VideoFlags_1080i,
    EEPROMModItem_DVDPlaybackKitZone,
    //Helper
    EEPROMModItem_Count
} EEPROMModItem;


void BootEepromReadEntireEEPROM(void);
void BootEepromReloadEEPROM(EEPROMDATA * realeeprom);
void BootEepromCompareAndWriteEEPROM(EEPROMDATA * realeeprom);
void BootEepromPrintInfo(void);
void BootEepromWriteEntireEEPROM(void);
void EepromCRC(unsigned char *crc, unsigned char *data, long dataLen);
void EepromSetVideoStandard(EEPROM_VideoStandard standard);
void EepromSetVideoFormat(EEPROM_VidScreenFormat format);
EEPROM_EncryptVersion EepromSanityCheck(EEPROMDATA * eepromPtr);
EEPROM_EncryptVersion decryptEEPROMData(unsigned char* eepromPtr, unsigned char* decryptedBuf);
void encryptEEPROMData(unsigned char decryptedInput[0x30], EEPROMDATA * targetEEPROMPtr, EEPROM_EncryptVersion targetVersion);
int getGameRegionValue(EEPROMDATA * eepromPtr);
int setGameRegionValue(unsigned char value);

const char* getGameRegionText(EEPROM_XBERegion gameRegion);
const char* getDVDRegionText(EEPROM_DVDRegion dvdRegion);
const char* getVideoStandardText(EEPROM_VideoStandard vidStandard);
const char* getScreenFormatText(EEPROM_VidScreenFormat vidFormat);
#endif // _BootEEPROM_H_
