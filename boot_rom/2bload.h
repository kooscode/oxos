/***************************************************************************
      Includes used by XBox boot code
 ***************************************************************************/
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

//#include "2bconsts.h"
#include <stdint.h>
#include "cromwell_types.h"
#include <stddef.h>

#define BootloaderVersion1 0x00000001u

#define I2C_IO_BASE 0xc000
///////////////////////////////
/* BIOS-wide error codes        all have b31 set  */

enum {
    ERR_SUCCESS = 0,  // completed without error

    ERR_I2C_ERROR_TIMEOUT = 0x80000001,  // I2C action failed because it did not complete in a reasonable time
    ERR_I2C_ERROR_BUS = 0x80000002, // I2C action failed due to non retryable bus error

    ERR_BOOT_PIC_ALG_BROKEN = 0x80000101 // PIC algorithm did not pass its self-test
};


//////// BootPerformPicChallengeResponseAction.c

/* ----------------------------  IO primitives -----------------------------------------------------------
*/

static __inline void IoOutputByte(unsigned short wAds, unsigned char bValue) {
    __asm__ __volatile__ ("outb %b0,%w1": :"a" (bValue), "Nd" (wAds));
}

static __inline void IoOutputWord(unsigned short wAds, unsigned short wValue) {
    __asm__ __volatile__ ("outw %0,%w1": :"a" (wValue), "Nd" (wAds));
    }

static __inline unsigned char IoInputByte(unsigned short wAds) {
  unsigned char _v;
  __asm__ __volatile__ ("inb %w1,%0":"=a" (_v):"Nd" (wAds));
  return _v;
}

static __inline unsigned short IoInputWord(unsigned short wAds) {
  unsigned short _v;
  __asm__ __volatile__ ("inw %w1,%0":"=a" (_v):"Nd" (wAds));
  return _v;
}

// boot process
int BootPerformPicChallengeResponseAction(void);

// LED control (see associated enum above)
//int I2cSetFrontpanelLed(unsigned char b);

////////// BootResetActions.c

void BootStartBiosLoader(void);

///////// BootPerformPicChallengeResponseAction.c

int I2CTransmitWord(unsigned char bPicAddressI2cFormat, unsigned short wDataToWrite);
int I2CTransmitByteGetReturn(unsigned char bPicAddressI2cFormat, unsigned char bDataToWrite);

void *memcpy (void *__restrict __dest, const void *__restrict __src, size_t __n);
void *memset (void *__s, int __c, size_t __n);
int memcmp(const void *pb, const void *pb1, size_t n);

unsigned char *BufferIN;
int BufferINlen;
unsigned char *BufferOUT;
int BufferOUTPos;
