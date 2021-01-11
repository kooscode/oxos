/*
 * FlashLowLevel.h
 *
 *  Created on: Oct 18, 2016
 *      Author: cromwelldev
 */

#ifndef FLASHLOWLEVEL_H_
#define FLASHLOWLEVEL_H_

#include "FlashHelpers.h"

void FlashLowLevel_Init(void);
bool FlashLowLevel_ReadDevice(void);
bool FlashLowLevel_DeviceIsBusy(void);
void FlashLowLevel_InititiateSectorErase(unsigned int addr); // 4KB
void FlashLowLevel_InititiateBlockErase(unsigned int addr);  // 64KB
void FlashLowLevel_InititiateChipErase(void);
void FlashLowLevel_WriteByte(unsigned char byte, unsigned int addr);

inline unsigned char FlashLowLevel_ReadByte(unsigned int addr)
{
    return flashDevice.m_pbMemoryMappedStartAddress[addr];
}

#endif /* FLASHLOWLEVEL_H_ */
