#ifndef _Boot_H_
#define _Boot_H_

#include "config.h"

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

/////////////////////////////////
// configuration

#include "consts.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cromwell_types.h"
#include "memory_layout.h"


unsigned int cromwell_config;
unsigned int cromwell_retryload;
unsigned int cromwell_2blversion;
unsigned int cromwell_2blsize;

unsigned int xbox_ram;

#define XROMWELL    0
#define CROMWELL    1

#define ICON_WIDTH 64
#define ICON_HEIGHT 64

static inline double min (double a, double b)
{
    if (a < b) return a; else return b;
}

static inline double max (double a, double b)
{
    if (a > b) return a; else return b;
}

#include "iso_fs.h"
#include "BootVideo.h"

#define ASSERT(exp) { if(!(exp)) { bprintf("Assert failed file " __FILE__ " line %d\n", __LINE__); } }

extern volatile CURRENT_VIDEO_MODE_DETAILS vmode;
unsigned int video_encoder;

volatile unsigned int VIDEO_CURSOR_POSX;
volatile unsigned int VIDEO_CURSOR_POSY;
volatile unsigned int VIDEO_ARGB;
volatile unsigned int VIDEO_LUMASCALING;
volatile unsigned int VIDEO_RSCALING;
volatile unsigned int VIDEO_BSCALING;
volatile unsigned int BIOS_TICK_COUNT;
volatile unsigned int VIDEO_VSYNC_POSITION;
volatile unsigned int VIDEO_VSYNC_DIR;
volatile unsigned int DVD_TRAY_STATE;

unsigned char VIDEO_AV_MODE ;

#define DVD_CLOSED         0
#define DVD_CLOSING         1
#define DVD_OPEN           2
#define DVD_OPENING           3

/////////////////////////////////
// Superfunky i386 internal structures

typedef struct gdt_t {
        unsigned short       m_wSize __attribute__ ((packed));
        unsigned long m_dwBase32 __attribute__ ((packed));
        unsigned short       m_wDummy __attribute__ ((packed));
} ts_descriptor_pointer;

typedef struct {  // inside an 8-byte protected mode interrupt vector
    unsigned short m_wHandlerHighAddressLow16;
    unsigned short m_wSelector;
    unsigned short m_wType;
    unsigned short m_wHandlerLinearAddressHigh16;
} ts_pm_interrupt;

typedef enum {
    EDT_UNKNOWN= 0,
    EDT_XBOXFS
} enumDriveType;

typedef struct tsHarddiskInfo {  // this is the retained knowledge about an IDE device after init
    unsigned short m_fwPortBase;
    unsigned short m_wCountHeads;
    unsigned short m_wCountCylinders;
    unsigned short m_wCountSectorsPerTrack;
    unsigned long m_dwCountSectorsTotal; /* total */
    unsigned char m_bLbaMode;    /* am i lba (0x40) or chs (0x00) */
    unsigned char m_szIdentityModelNumber[40];
    unsigned char term_space_1[2];
    unsigned char m_szSerial[20];
    unsigned char term_space_2[2];
    char m_szFirmware[8];
    unsigned char term_space_3[2];
    unsigned char m_fDriveExists;
    unsigned char m_fAtapi;  // true if a CDROM, etc
    enumDriveType m_enumDriveType;
    unsigned char m_bCableConductors;  // valid for device 0 if present
    unsigned short m_wAtaRevisionSupported;
    unsigned char s_length;
    unsigned char m_length;
    unsigned char m_bIORDY : 1;
    unsigned char m_fDMAInit : 1;
    unsigned char m_fFlushCacheSupported : 1;
    unsigned char m_fFlushCacheExtSupported : 1;
    unsigned char unused : 3;
    unsigned short m_securitySettings; //This contains the contents of the ATA security regs
    unsigned short m_masterPassSupport;
    unsigned char m_maxBlockTransfer;  //Max number of blocks allowed in a single transfer.
    unsigned short m_minPIOcycle;
    bool m_fHasSMARTcapabilities;
    bool m_fSMARTEnabled;
    unsigned char m_SMARTFeaturesSupported;
} tsHarddiskInfo;

///////////////////////////////
/* BIOS-wide error codes        all have b31 set  */

enum {
    ERR_SUCCESS = 0,  // completed without error

    ERR_I2C_ERROR_TIMEOUT = 0x80000001,  // I2C action failed because it did not complete in a reasonable time
    ERR_I2C_ERROR_BUS = 0x80000002, // I2C action failed due to non retryable bus error

    ERR_BOOT_PIC_ALG_BROKEN = 0x80000101 // PIC algorithm did not pass its self-test
};

/////////////////////////////////
// some Boot API prototypes

//////// BootPerformPicChallengeResponseAction.c

/* ----------------------------  IO primitives -----------------------------------------------------------
*/

static __inline void IoOutputByte(unsigned short wAds, unsigned char bValue) {
//    __asm__  ("                 out    %%al,%%dx" : : "edx" (dwAds), "al" (bValue)  );
    __asm__ __volatile__ ("outb %b0,%w1": :"a" (bValue), "Nd" (wAds));
}

static __inline void IoOutputWord(unsigned short wAds, unsigned short wValue) {
//    __asm__  ("     out    %%ax,%%dx    " : : "edx" (dwAds), "ax" (wValue)  );
    __asm__ __volatile__ ("outw %0,%w1": :"a" (wValue), "Nd" (wAds));
    }

static __inline void IoOutputDword(unsigned short wAds, unsigned int dwValue) {
//    __asm__  ("     out    %%eax,%%dx    " : : "edx" (dwAds), "ax" (wValue)  );
    __asm__ __volatile__ ("outl %0,%w1": :"a" (dwValue), "Nd" (wAds));
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

static __inline unsigned int IoInputDword(unsigned short wAds) {
  unsigned int _v;

  __asm__ __volatile__ ("inl %w1,%0":"=a" (_v):"Nd" (wAds));
  return _v;
}

#define rdmsr(msr,val1,val2) \
       __asm__ __volatile__("rdmsr" \
                : "=a" (val1), "=d" (val2) \
                : "c" (msr))

#define wrmsr(msr,val1,val2) \
     __asm__ __volatile__("wrmsr" \
              : /* no outputs */ \
              : "c" (msr), "a" (val1), "d" (val2))


void BootPciInterruptEnable(void);

    // boot process
int BootPerformPicChallengeResponseAction(void);

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *m_plistentryNext;
    struct _LIST_ENTRY *m_plistentryPrevious;
} LIST_ENTRY;

void ListEntryInsertAfterCurrent(LIST_ENTRY *plistentryCurrent, LIST_ENTRY *plistentryNew);
void ListEntryRemove(LIST_ENTRY *plistentryCurrent);

////////// BootPerformXCodeActions.c

int BootPerformXCodeActions(void);

#include "BootEEPROM.h"
#include "BootParser.h"

////////// BootStartBios.c

void StartBios(CONFIGENTRY *config,int nActivePartition, int nFATXPresent,int bootfrom);
int BootMenu(CONFIGENTRY *config,int nDrive,int nActivePartition, int nFATXPresent);

////////// BootResetActions.c
void ClearIDT (void);
void BootResetAction(void);
void BootCpuCache(bool fEnable) ;
void BiosCmosWrite(unsigned char bAds, unsigned char bData);
unsigned char BiosCmosRead(unsigned char bAds);


///////// BootPciPeripheralInitialization.c
void BootPciPeripheralInitialization(void);
void BootAGPBUSInitialization(void);
void BootDetectMemorySize(void);
extern void    ReadPCIByte(unsigned int bus, unsigned int dev, unsigned intfunc,     unsigned int reg_off, unsigned char *pbyteval);
extern void    WritePCIByte(unsigned int bus, unsigned int dev, unsigned int func,    unsigned int reg_off, unsigned char byteval);
extern void    ReadPCIDword(unsigned int bus, unsigned int dev, unsigned int func,    unsigned int reg_off, unsigned int *pdwordval);
extern void    WritePCIDword(unsigned int bus, unsigned int dev, unsigned int func,        unsigned int reg_off, unsigned int dwordval);
extern void    ReadPCIBlock(unsigned int bus, unsigned int dev, unsigned int func,        unsigned int reg_off, unsigned char *buf,    unsigned int nbytes);
extern void    WritePCIBlock(unsigned int bus, unsigned int dev, unsigned int func,     unsigned int reg_off, unsigned char *buf, unsigned int nbytes);

void PciWriteByte (unsigned int bus, unsigned int dev, unsigned int func,
        unsigned int reg_off, unsigned char byteval);
unsigned char PciReadByte(unsigned int bus, unsigned int dev, unsigned int func, unsigned int reg_off);
unsigned int PciWriteDword(unsigned int bus, unsigned int dev, unsigned int func, unsigned int reg_off, unsigned int dw);
unsigned int PciReadDword(unsigned int bus, unsigned int dev, unsigned int func, unsigned int reg_off);

///////// BootIde.c

extern tsHarddiskInfo tsaHarddiskInfo[];  // static struct stores data about attached drives
int BootIdeInit(void);
int BootIdeReadSector(int nDriveIndex, void * pbBuffer, unsigned int block, int byte_offset, int n_bytes);
int BootIdeBootSectorHddOrElTorito(int nDriveIndex, unsigned char * pbaResult);
int BootIdeAtapiAdditionalSenseCode(int nDrive, unsigned char * pba, int nLengthMaxReturn);
int BootIdeSetTransferMode(int nIndexDrive, int nMode);
int BootIdeSetMultimodeSectors(unsigned char nIndexDrive, unsigned char nbSectors);
//int BootIdeSetPIOMode(unsigned char nIndexDrive, unsigned short cycleTime);
int BootIdeWaitNotBusy(unsigned uIoBase);
bool BootIdeAtapiReportFriendlyError(int nDriveIndex, char * szErrorReturn, int nMaxLengthError);
void BootIdeAtapiPrintkFriendlyError(int nDriveIndex);

///////// BootUSB.c

void BootStopUSB(void);
void BootStartUSB(void);
void USBGetEvents(void);

#include "xpad.h"

extern struct xpad_data XPAD_current[4];
extern struct xpad_data XPAD_last[4];

void MemoryManagementInitialization(void * pvStartAddress, unsigned int dwTotalMemoryAllocLength);
void * malloc(size_t size);
void free(void *);

extern volatile int nCountI2cinterrupts, nCountUnusedInterrupts, nCountUnusedInterruptsPic2, nCountInterruptsSmc, nCountInterruptsIde;
typedef enum {
    ETS_NOTHING = 0,
    ETS_OPEN_OR_OPENING,
    ETS_CLOSING,
    ETS_CLOSED
} TRAY_STATE;
extern volatile TRAY_STATE traystate;


extern void BootInterruptsWriteIdt(void);

int copy_swap_trim(unsigned char *dst, unsigned char *src, int len);
void HMAC_SHA1( unsigned char *result,
                unsigned char *key, int key_length,
                unsigned char *text1, int text1_length,
                unsigned char *text2, int text2_length );

void setLED(char *pattern);

#endif // _Boot_H_
