/*
 * EEPROMStrings.h
 *
 *  Created on: Jan 6, 2017
 *      Author: cromwelldev
 */

#ifndef LIB_EEPROM_EEPROMSTRINGS_H_
#define LIB_EEPROM_EEPROMSTRINGS_H_

//String enum for DVD_ZONE
static const char * const DVDregiontext[] = {
    "Region Clear",
    "USA (1)",
    "Europe (2)",
    "India (3)",
    "Australia (4)",
    "USSR (5)",
    "China (6)",
    "Free (7)",
    "Airlines (8)"
};

//String enum for XBE_REGSION
static const char * const Gameregiontext[] = {
    "Unknown/Error",
    "NTSC-U",
    "NTSC-J",
    "n/a",
    "PAL"
};

//String enum for VidScreenFormat
static const char * const Vidformattext[] = {
    "Fullscreen",
    "Widescreen",
    "Letterbox"
};

//Always match with EEPROMModItem enum
static const char * const SettingChangeLabel[EEPROMModItem_Count] = {
  "HDDKkey=",
  "GameRegion=",
  "SerialNumber=",
  "MACAddress=",
  "VideoStandard=",
  "VideoFormat=",
  "Enable480p=",
  "Enable720p=",
  "Enable71080i",
  "DVDZone=",
};

#endif /* LIB_EEPROM_EEPROMSTRINGS_H_ */
