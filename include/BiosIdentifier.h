/*
 * BiosIdentifier.h
 *
 *  Created on: Dec 9, 2016
 *      Author: cromwelldev
 */

#ifndef BIOSIDENTIFIER_H_
#define BIOSIDENTIFIER_H_


#define HeaderVersionV1 1U   //MD5 hash is calculated on the 0x0 to 0x3F000 range.
                             //BiosSize in identifier is always 256KB.

#define HeaderVersionV2 2U   //MD5 hash is calculated on the real used range inside the 256KB bin image.
                             //BiosSize in identifier is the size in bytes of the compressed cromwell image.

#define CurrentHeaderVersion HeaderVersionV2


#define Option1_SaveSettingsLocationBit 0x01    //Instruct flash engine to calculates save settings location in flash
                                                //If not defined, invariably save settings @ 0x3F000 in flash.

#define BiosID_Version10                0x01
#define BiosID_Version11                0x02
#define BiosID_Version12                0x04
#define BiosID_Version13                0x08
#define BiosID_Version14                0x10
#define BiosID_Version15                0x20
#define BiosID_Version16                0x40
#define BiosID_Version17                0x80

#define BiosID_VideoEncoder_Conexant    0x01
#define BiosID_VideoEncoder_Focus       0x02
#define BiosID_VideoEncoder_Xcalibur    0x04

static const char* MagicBiosHeaderValue = "AUTO";

struct BiosIdentifier {

    unsigned char   Magic[4];               // AUTO
    unsigned char   HeaderVersion;
    unsigned char   XboxVersion;            // Which Xbox Version does it Work ? (Options)
    unsigned char   VideoEncoder;
    unsigned char   HeaderPatch;
    unsigned char   Option1;
    unsigned char   Option2;
    unsigned char   Option3;
    unsigned int    BiosSize;               // in Bytes
    char Name[32];
    unsigned char MD5Hash[16];
} __attribute__((packed));

#endif /* INCLUDE_BIOSIDENTIFIER_H_ */
