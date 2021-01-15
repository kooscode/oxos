/*
 *  This code was originally from:
 *   UBL, The Universal Talkware Boot Loader
 *    Copyright (C) 2000 Universal Talkware Inc.
 */

#include "boot.h"
//#include "shared.h"
#include "BootEEPROM.h"
#include "BootIde.h"
#include "cromwell.h"
#include "string.h"
#include "stdio.h"
#include "video.h"
// #include "lib/LPCMod/BootLPCMod.h"
#include "lib/time/timeManagement.h"
// #include "xblast/HardwareIdentifier.h"

#undef sprintf

////////////////////////////////////
// IDE types and constants
#define IDE_SECTOR_SIZE         0x200
#define IDE_BASE1                     (0x1F0u) /* primary controller */

#define IDE_REG_EXTENDED_OFFSET       (0x204u) /* 0x3F4 */

/* Read/Write registers*/
#define IDE_REG_DATA(base)              ((base) + 0u) /* word register */
#define IDE_REG_SECTOR_COUNT(base)      ((base) + 2u)                          //Sector Count register (read-write)
#define IDE_REG_LBA_LOW(base)           ((base) + 3u)                          //(read-write)
#define IDE_REG_LBA_MID(base)           ((base) + 4u)                          //(read-write)
#define IDE_REG_LBA_HIGH(base)          ((base) + 5u)                          //(read-write)
#define IDE_REG_DRIVEHEAD(base)         ((base) + 6u)                          //Device control register (read-write)
#define IDE_REG_DEVICE(base)            ((base) + 6u)                          //Same as above but easier to remember


/* Read only registers*/
#define IDE_REG_ERROR(base)             ((base) + 1u)                          //Error register (read-only)
#define IDE_REG_STATUS(base)            ((base) + 7u)                          //Status register (read-only)
#define IDE_REG_ALTSTATUS(base)         ((base) + IDE_REG_EXTENDED_OFFSET + 2u)

/* Write Only registers*/
#define IDE_REG_FEATURE(base)           ((base) + 1u)                          //Features register (write-only)
#define IDE_REG_COMMAND(base)           ((base) + 7u)                          //Command Register(write-only)
#define IDE_REG_CONTROL(base)           ((base) + IDE_REG_EXTENDED_OFFSET + 2u)

/*
Normal command input(sending to ATA device) must send the following registers:
      -Features
      -Sector Count
      -LBA Low
      -LBA Mid
      -LBA High
      -Device
      -Command

Command register must be the last one to be sent for the command to execute. It's then a good practice
to send all the necessary registers in the same order as presented above.


Normal command output(receiving from ATA device) will require a read on the following registers
    -Error
    -Sector Count
    -LBA Low
    -LBA Mid
    -LBA High
    -Device
    -Status
*/

typedef struct {
    unsigned char m_bPrecomp;
    unsigned char m_bCountSector;
    unsigned char m_bSector;
    unsigned short m_wCylinder;
    unsigned char m_bDrivehead;
    
       /* 48-bit LBA */   
    unsigned char m_bCountSectorExt;   
    unsigned char m_bSectorExt;   
    unsigned short m_wCylinderExt; 
    
#       define IDE_DH_DEFAULT (0xA0)
#       define IDE_DH_HEAD(x) ((x) & 0x0F)
#       define IDE_DH_MASTER  (0x00)
#       define IDE_DH_SLAVE   (0x10)
#       define IDE_DH_DRIVE(x) ((((x) & 1) != 0)?IDE_DH_SLAVE:IDE_DH_MASTER)
#       define IDE_DH_LBA     (0x40)
#       define IDE_DH_CHS     (0x00)

} tsIdeCommandParams;

#define IDE_DEFAULT_COMMAND { 0xFFu, 0x01, 0x00, 0x0000, IDE_DH_DEFAULT | IDE_DH_SLAVE, 0x00, 0x00, 0x0000 }
#define printk_debug bprintf


const char * const szaSenseKeys[] = {
    "No Sense", "Recovered Error", "Not Ready", "Medium Error",
    "Hardware Error", "Illegal request", "Unit Attention", "Data Protect",
    "Reserved 8", "Reserved 9", "Reserved 0xa", "Aborted Command",
    "Miscompare", "Reserved 0xf"
};

///////////////////////////////////////////////////////////////////////////////////////////////////
//  Helper routines
//
/* -------------------------------------------------------------------------------- */

int BootIdeWaitNotBusy(unsigned uIoBase)
{
    unsigned char b = 0x80;
    //Start being busy
    while ((b & 0x80) && !(b & 0x08)) 
    {         
        b=IoInputByte(IDE_REG_STATUS(uIoBase));
        //Device is not ready until bit7(BSY) is cleared and bit3(DRQ) is set.
        if(b == 0 || (b&0x20))	
          //No device
          return -1;
    }
    
    //bit0 == ERR
    return b&0x01;         
}

/* -------------------------------------------------------------------------------- */

int BootIdeWaitDataReady(unsigned uIoBase)
{
    int i = 0x800000;
//    wait_us(1);

    //Assuming that the while assertion, i decrementing and if condition assertion all
    //take only a single cycle per operation(very unlikely), it would take around 34ms
    //for i to reach 0 with a CPU running at 733MHz. So no need for extra smalldelay here.
    //Since our program is single-threaded anyway, there's not much harm in polling the
    //HDD's STATUS register until the necessary state is reached.
    do {
        if (((IoInputByte(IDE_REG_STATUS(uIoBase)) & 0x88) == 0x08)){       //DRQ bit raised and BSY bit cleared.
            if(IoInputByte(IDE_REG_STATUS(uIoBase)) & 0x01){
                BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Error bit raised.");
                return 2;        //ERR bit raised, return 2.
            }
            return 0;                                                           //Everything good, move on.
        }
        i--;
    } while (i != 0);

    if(IoInputByte(IDE_REG_STATUS(uIoBase)) & 0x01) {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Error bit raised after timed out.");
        return 2;                //ERR bit raised.
    }
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Timeout error.");
    return 1;                                                                   //Timeout error.
}

/* -------------------------------------------------------------------------------- */

int BootIdeIssueAtaCommand(
    unsigned uIoBase,
    ide_command_t command,
    tsIdeCommandParams * params, bool skipFirstWait)
{
    int n;

    //IoInputByte(IDE_REG_STATUS(uIoBase));     //No need since we'll be checking ALT_STATUS register in BootIdeWaitNotBusy function.
    if(!skipFirstWait)
    {
        n=BootIdeWaitNotBusy(uIoBase);
        if(n == -1)    {// as our command may be being used to clear the error, not a good policy to check too closely!
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Error on BootIdeIssueAtaCommand wait 1: ret=%d, error %02X", n, IoInputByte(IDE_REG_ERROR(uIoBase)));
            return n;
        }
    }
     
     /* 48-bit LBA */   
        /* this won't hurt for non 48-bit LBA commands since we re-write these registers below */   
    //BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Issuing ATA command.");
    IoOutputByte(IDE_REG_SECTOR_COUNT(uIoBase), params->m_bCountSectorExt);   
    IoOutputByte(IDE_REG_LBA_LOW(uIoBase), params->m_bSectorExt);
    IoOutputByte(IDE_REG_LBA_MID(uIoBase), params->m_wCylinderExt & 0xFF);
    IoOutputByte(IDE_REG_LBA_HIGH(uIoBase), (params->m_wCylinderExt >> 8) );
    /* End 48-bit LBA */   

    IoOutputByte(IDE_REG_SECTOR_COUNT(uIoBase), params->m_bCountSector);	//Sector count reg
    IoOutputByte(IDE_REG_LBA_LOW(uIoBase), params->m_bSector);		//LBA LOW
    IoOutputByte(IDE_REG_LBA_MID(uIoBase), params->m_wCylinder & 0xFF);	//LBA MID
    IoOutputByte(IDE_REG_LBA_HIGH(uIoBase), (params->m_wCylinder >> 8));	//LBA HIGH
    /* For 1ba28, m_bDrivehead lower nibble has already been populated with LBA28 addr 4 MSBs */
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), params->m_bDrivehead);		//DEVICE REG

    IoOutputByte(IDE_REG_COMMAND(uIoBase), command);				//COMMAND REG
//    wait_us(1);

    n=BootIdeWaitNotBusy(uIoBase);
    if(n)
    {
//        printk("\n      error on BootIdeIssueAtaCommand wait 3: ret=%d, error %02X\n", n, IoInputByte(IDE_REG_ERROR(uIoBase)));
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Error on BootIdeIssueAtaCommand wait 2: ret=%d, error %02X", n, IoInputByte(IDE_REG_ERROR(uIoBase)));
        return n;
    }

    return 0;
}

/* -------------------------------------------------------------------------------- */

int BootIdeReadData(unsigned uIoBase, void * buf, size_t size)
{
    unsigned short * ptr = (unsigned short *) buf;
    if (BootIdeWaitDataReady(uIoBase)) {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Data not ready..");
        return 1;
    }

    while (size > 1) {
        *ptr++ = IoInputWord(IDE_REG_DATA(uIoBase));
        size -= 2;
    }


    IoInputByte(IDE_REG_STATUS(uIoBase));
    if(IoInputByte(IDE_REG_STATUS(uIoBase)) & 0x01) {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Ended with an error.");
        return 2;
    }
    return 0;
}

/* -------------------------------------------------------------------------------- */

// issues a block of data ATA-style

int BootIdeWriteData(unsigned uIoBase, const void * buf, unsigned int size)
{
    register unsigned short * ptr = (unsigned short *) buf;
    int n;

    n=BootIdeWaitDataReady(uIoBase);

    while (size > 1) {

        IoOutputWord(IDE_REG_DATA(uIoBase), *ptr);
        size -= 2;
        ptr++;
    }

    
    n=BootIdeWaitNotBusy(uIoBase);
    if(n) {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Waiting for good status reg returned error : %d", n);
        return n;
    }

    if(IoInputByte(IDE_REG_STATUS(uIoBase)) & 0x01) return 2;     //ERR flag raised.
    
    return 0;
}





/* -------------------------------------------------------------------------------- */

int BootIdeWriteAtapiData(unsigned uIoBase, void * buf, size_t size)
{
    unsigned short * ptr = (unsigned short *) buf;
    unsigned short w;
    int n;

    n=BootIdeWaitDataReady(uIoBase);

    wait_us_blocking(1);

    w=IoInputByte(IDE_REG_LBA_MID(uIoBase));
    w|=(IoInputByte(IDE_REG_LBA_HIGH(uIoBase)))<<8;

    n=IoInputByte(IDE_REG_STATUS(uIoBase));
    if(n&1) { // error
        return 1;
    }

    while (size > 1) {

        IoOutputWord(IDE_REG_DATA(uIoBase), *ptr);
        size -= 2;
        ptr++;
    }
    n=IoInputByte(IDE_REG_STATUS(uIoBase));
    if(n&1) { // error
        return 1;
    }
    wait_us_blocking(1);
    n=BootIdeWaitNotBusy(uIoBase);
    if(n) {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Waiting for good status reg returned error : %d", n);
        return n;
    }
    wait_us_blocking(1);

   if(IoInputByte(IDE_REG_STATUS(uIoBase)) & 0x01) return 2;
    
    return 0;
}


/* -------------------------------------------------------------------------------- */

int BootIdeIssueAtapiPacketCommandAndPacket(int nDriveIndex, unsigned char *pAtapiCommandPacket12Bytes)
{
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned     uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;

    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nDriveIndex);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);

    tsicp.m_wCylinder=2048;
    BootIdeWaitNotBusy(uIoBase);
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_ATAPI_PACKET, &tsicp, false)) 
    {
            return 1;
    }
    
    if(BootIdeWaitNotBusy(uIoBase)) 
    {
        return 1;
    }



    if(BootIdeWriteAtapiData(uIoBase, pAtapiCommandPacket12Bytes, 12)) 
    {
        return 1;
    }

    if(pAtapiCommandPacket12Bytes[0]!=0x1e) 
    {
        if(BootIdeWaitDataReady(uIoBase)) 
        {
            return 1;
        }
    }
    return 0;
}


/* -------------------------------------------------------------------------------- */


/////////////////////////////////////////////////
//  BootIdeDriveInit
//
//  Called by BootIdeInit for each drive
//  detects and inits each drive, and the structs containing info about them

int BootIdeDriveInit(unsigned uIoBase, int nIndexDrive)
{
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned short* drive_info;
    unsigned char baBuffer[512];
    int n = 0;
    unsigned char cl, ch;
     
    tsaHarddiskInfo[nIndexDrive].m_fwPortBase = uIoBase;
    tsaHarddiskInfo[nIndexDrive].m_wCountHeads = 0u;
    tsaHarddiskInfo[nIndexDrive].m_wCountCylinders = 0u;
    tsaHarddiskInfo[nIndexDrive].m_wCountSectorsPerTrack = 0u;
    tsaHarddiskInfo[nIndexDrive].m_dwCountSectorsTotal = 1ul;
    tsaHarddiskInfo[nIndexDrive].m_bLbaMode = IDE_DH_CHS;
    tsaHarddiskInfo[nIndexDrive].m_fDriveExists = 0;
    tsaHarddiskInfo[nIndexDrive].m_enumDriveType=EDT_UNKNOWN;
    tsaHarddiskInfo[nIndexDrive].m_fAtapi=false;
    tsaHarddiskInfo[nIndexDrive].m_wAtaRevisionSupported=0;
    tsaHarddiskInfo[nIndexDrive].m_securitySettings = 0;
    tsaHarddiskInfo[nIndexDrive].m_masterPassSupport = 0;
    tsaHarddiskInfo[nIndexDrive].m_bIORDY = 0;
    tsaHarddiskInfo[nIndexDrive].m_fHasSMARTcapabilities = false;
    tsaHarddiskInfo[nIndexDrive].m_fSMARTEnabled = false;
    tsaHarddiskInfo[nIndexDrive].m_SMARTFeaturesSupported = 0;

//Disable IRQ
    IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x02); // kill interrupt, only bit1 needs to be set
//Why disable DMA?
    IoOutputByte(IDE_REG_FEATURE(uIoBase), 0x00); // kill DMA

//Select drive
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nIndexDrive);

    n=BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false);
    
    if(n == -1){
    	BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_WARN, "No device at %s position. Halting init.", nIndexDrive? "slave" : "master");
    	return 1;
    }
    
    if(n == 1) {
    	cl = IoInputByte(IDE_REG_LBA_MID(uIoBase));
    	ch = IoInputByte(IDE_REG_LBA_HIGH(uIoBase));
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Drive is ATAPI, surely DVD drive.");
        if((cl == 0x14 && ch == 0xEB) || (cl == 0x69 && ch == 0x96)){
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Issuing ATAPI IDENTIFY command.");
            if (BootIdeIssueAtaCommand(uIoBase,IDE_CMD_PACKET_IDENTIFY,&tsicp, false)) {
                BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_WARN, "ATAPI IDENTIFY command returned error. Drive not detected. Halting!");
                return 1;
            }
            tsaHarddiskInfo[nIndexDrive].m_fAtapi=true;
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Succesfully identified ATAPI device.");
        }
        else{
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Magic ATAPI values identifier not valid.");
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Unknown device at %s position. Halting init.",nIndexDrive? "slave" : "master");
            return 1;
        }
    } 
    else{
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Drive is not ATAPI, so IDE.");
        tsaHarddiskInfo[nIndexDrive].m_fAtapi=false;
    }
        
    BootIdeWaitDataReady(uIoBase);
    
    if(BootIdeReadData(uIoBase, baBuffer, IDE_SECTOR_SIZE)) 
    {
        return 1;
    }  

    drive_info = (unsigned short*)baBuffer;
    tsaHarddiskInfo[nIndexDrive].m_wCountHeads = drive_info[3];
    tsaHarddiskInfo[nIndexDrive].m_wCountCylinders = drive_info[1];
    tsaHarddiskInfo[nIndexDrive].m_wCountSectorsPerTrack = drive_info[6];
    tsaHarddiskInfo[nIndexDrive].m_dwCountSectorsTotal = *((unsigned int*)&(drive_info[60]));
    tsaHarddiskInfo[nIndexDrive].m_wAtaRevisionSupported = drive_info[88];


    VIDEO_ARGB=0xffc8c8c8;
        /* 48-bit LBA - we should check bits 83 AND 86 to check both
     * supported AND enabled  - however, some drives do not set
     * Bit 83. Bit 86 seems to be the accepted way to detect whether
     * 48-bit LBA is available. */
        if( drive_info[86] & 1ul<<10 )  {
    
        tsaHarddiskInfo[nIndexDrive].m_dwCountSectorsTotal = 
            *((unsigned int*)&(drive_info[100]));
    }
    /* End 48-bit LBA */   

    tsaHarddiskInfo[nIndexDrive].m_fFlushCacheSupported = (drive_info[86] & 1ul<<12) ? 1 : 0;
    tsaHarddiskInfo[nIndexDrive].m_fFlushCacheExtSupported = (drive_info[86] & 1ul<<13)  ? 1 : 0;
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "flush cache support:%u   ext:%u", tsaHarddiskInfo[nIndexDrive].m_fFlushCacheSupported, tsaHarddiskInfo[nIndexDrive].m_fFlushCacheExtSupported);
    
    { 
        unsigned short * pw=(unsigned short *)&(drive_info[10]);
        tsaHarddiskInfo[nIndexDrive].s_length =
            copy_swap_trim(tsaHarddiskInfo[nIndexDrive].m_szSerial,(unsigned char *)pw,20);
        pw=(unsigned short *)&(drive_info[27]);
        tsaHarddiskInfo[nIndexDrive].m_length =
            copy_swap_trim(tsaHarddiskInfo[nIndexDrive].m_szIdentityModelNumber,(unsigned char *)pw,40);
        copy_swap_trim(tsaHarddiskInfo[nIndexDrive].m_szFirmware,(unsigned char *)&(drive_info[23]),8);

    }

    tsaHarddiskInfo[nIndexDrive].m_fDriveExists = 1;

    if (tsaHarddiskInfo[nIndexDrive].m_fAtapi) {
     // CDROM/DVD
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "ATAPI device specific init routine.");
                // We Detected a CD-DVD or so, as there are no Heads ...
        tsaHarddiskInfo[nIndexDrive].m_fAtapi=true;
#ifndef SILENT_MODE
        printk("          hd%c: ", nIndexDrive+'a');
        VIDEO_ARGB=0xffc8c800;

        printk("          %s %s %s - ATAPI\n",tsaHarddiskInfo[nIndexDrive].m_szIdentityModelNumber,
            tsaHarddiskInfo[nIndexDrive].m_szIdentityModelNumber,
            tsaHarddiskInfo[nIndexDrive].m_szSerial,
            tsaHarddiskInfo[nIndexDrive].m_szFirmware);
#endif
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "hd%c:", nIndexDrive+'a');
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "    %s",tsaHarddiskInfo[nIndexDrive].m_szIdentityModelNumber);
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "    %s", tsaHarddiskInfo[nIndexDrive].m_szIdentityModelNumber);
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "    %s", tsaHarddiskInfo[nIndexDrive].m_szSerial);
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "    %s", tsaHarddiskInfo[nIndexDrive].m_szFirmware);
        // if (isXBE() == false)
        {
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Reset ATAPI device specific requirement from cold boot.");
              // this is the only way to clear the ATAPI ''I have been reset'' error indication
            unsigned char ba[128];
            ba[2]=0x06;
            while (ba[2]==0x06) 
            {  
                // while bitching that it 'needs attention', give it REQUEST SENSE
                int nPacketLength=BootIdeAtapiAdditionalSenseCode(nIndexDrive, ba, sizeof(ba));
                if(nPacketLength<12) 
                {
                    ba[2]=0;
                }
            }
        }
    } 
        
        
    if (!tsaHarddiskInfo[nIndexDrive].m_fAtapi) {       //Drive is HDD (not CD/DVD).
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "IDE(HDD) device specific init routine.");
        unsigned long ulDriveCapacity1024=((tsaHarddiskInfo[nIndexDrive].m_dwCountSectorsTotal /1000)*512)/1000;
        
#ifndef SILENT_MODE
        printk("          hd%c: ", nIndexDrive+'a');
        VIDEO_ARGB=0xffc8c800;

        printk("          %s %s %u.%uGB - HDD\n",
            tsaHarddiskInfo[nIndexDrive].m_szIdentityModelNumber,
            tsaHarddiskInfo[nIndexDrive].m_szFirmware,
            ulDriveCapacity1024/1000, ulDriveCapacity1024%1000 
        );
#endif
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "hd%c:", nIndexDrive+'a');
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "    %s", tsaHarddiskInfo[nIndexDrive].m_szIdentityModelNumber);
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "    %s", tsaHarddiskInfo[nIndexDrive].m_szFirmware);
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "    %lu.%luGB", ulDriveCapacity1024/1000, ulDriveCapacity1024%1000);

        tsaHarddiskInfo[nIndexDrive].m_securitySettings = drive_info[128];
        tsaHarddiskInfo[nIndexDrive].m_masterPassSupport = drive_info[92];
        

        // if (isXBE() == false) 
        {
            if((drive_info[128]&0x0004)==0x0004) //Drive in locked status
            { 
            	char userPassword[21];
            	CalculateDrivePassword(nIndexDrive, userPassword, (char *)&eeprom);
            	userPassword[20] = '\0';
            	BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Drive requires SECURITY_UNLOCK");
            	BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Calculated password: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            	  userPassword[0], userPassword[1], userPassword[2], userPassword[3], userPassword[4], userPassword[5], userPassword[6], userPassword[7],
            	  userPassword[8], userPassword[9], userPassword[10], userPassword[11], userPassword[12], userPassword[13], userPassword[14], userPassword[15],
            	  userPassword[16], userPassword[17], userPassword[18], userPassword[19]);
                HDD_SECURITY_SendATACommand(nIndexDrive, IDE_CMD_SECURITY_UNLOCK, userPassword, false);
            }
        }  

        //Useful for READ/WRITE_MLTIPLE commands
        tsaHarddiskInfo[nIndexDrive].m_maxBlockTransfer = *((unsigned char *)&(drive_info[47]));
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "HDD support multiple sector transfers of %u sectors per command", tsaHarddiskInfo[nIndexDrive].m_maxBlockTransfer);

        //ATA specs require that we send SET MULTIPLE MOD command at least once to enable MULTIPLE READ/WRITE
        //commands. We send the SET MULTIPLE MODE command with the default number of sector per block.
        //But first let's check if we need to. 
        //If bit8 of word 59 is set along with a valid sector/block value, we don't need to send command.
        if((tsaHarddiskInfo[nIndexDrive].m_maxBlockTransfer + 0x0100) != (drive_info[59]&0x01FF)){
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Setting HDD Mutimode sectors to maximum sector per transfer.");
            if(BootIdeSetMultimodeSectors(nIndexDrive, tsaHarddiskInfo[nIndexDrive].m_maxBlockTransfer)){
                printk("\n\n\n\n\n\n\n\n              Unable to change Multimode's sectors per block.");
                BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_WARN, "Unable to change Multimode's sectors per block.");
                wait_ms(3000);
                return 1;
            }
        }
        if (drive_info[49] & 0x400) 
        { 
            /* bit 10 of capability word is IORDY supported bit */
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "IORDY control bit supported.");
            tsaHarddiskInfo[nIndexDrive].m_bIORDY = 1;
        }

        tsaHarddiskInfo[nIndexDrive].m_minPIOcycle = drive_info[67];       //Value in ns
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Minimum PIO cycle for HDD is %u.", tsaHarddiskInfo[nIndexDrive].m_minPIOcycle);
        //Set fastest PIO mode depending on cycle time supplied by HDD.


        //Depending on PIO cycle time value returned by IDENTIFY command, we select a PIO mode.
        //Can be from 0 to 4. One thing important is that bit3 must be set to 1(0x08).
        //Bits 2 to 0 select the PIO mode.


        //We'll just try mode 0 and 1 for now.
/*        if(tsaHarddiskInfo[nIndexDrive].m_minPIOcycle <= 120
              && tsaHarddiskInfo[nIndexDrive].m_bIORDY){        //Mode4
            n = BootIdeSetTransferMode(nIndexDrive, 0x0C); 
            //Mode4 timing specs are as follow:
            //Register 0x48, set at 0x20202020 for all drives(4)
            //Minimum recovery time = 30ns
            //Active pulse width = 90ns
            //For a total cycletime of 120ns.
            PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x58, 0x20202020);
            //Register 0x4C, set at 0x20200000
            //Address setup time = 30ns
            //DIOR/DIOW pulse width = 90ns
            //DIOR/DIOW recovery time = 30ns 
            //AMD-766 datasheet notes that C1A4C register should be left to default value when using PIO
            //PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x5C, 0x20200000);
        }                                                                                                                
        else if(tsaHarddiskInfo[nIndexDrive].m_minPIOcycle <= 180
                   && tsaHarddiskInfo[nIndexDrive].m_bIORDY){   //Mode3
            n = BootIdeSetTransferMode(nIndexDrive, 0x0B);
            //Mode3 timing specs are as follow:
            //Register 0x48, set at 0x22222222 for all drives(4)
            //Minimum recovery time = 90ns
            //Active pulse width = 90ns
            //For a total cycletime of 180ns.
            PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x58, 0x22222222);
            //Register 0x4C, set at 0x22220000
            //Address setup time = 30ns
            //DIOR/DIOW pulse width = 90ns
            //DIOR/DIOW recovery time = 90ns 
            //AMD-766 datasheet notes that C1A4C register should be left to default value when using PIO
            //PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x5C, 0x22220000);
        }
        else if(tsaHarddiskInfo[nIndexDrive].m_minPIOcycle <= 240){   //Mode2
            n = BootIdeSetTransferMode(nIndexDrive, 0x0A);
            //Mode2 timing specs are as follow:
            //Register 0x48, set at 0x42424242 for all drives(4)
            //Minimum recovery time = 90ns
            //Active pulse width = 150ns
            //For a total cycletime of 240ns.
            PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x58, 0x42424242);
            //Register 0x4C, set at 0x3F3F0000
            //Address setup time = 30ns
            //DIOR/DIOW pulse width = 120ns
            //DIOR/DIOW recovery time = - (put max to be safe)(put max to be safe)
            //AMD-766 datasheet notes that C1A4C register should be left to default value when using PIO
            //PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x5C, 0x3F3F0000);
        }
        else*/ if(tsaHarddiskInfo[nIndexDrive].m_minPIOcycle <= 383){   //Mode1
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Trying to set PIO mode 1.");
            n = BootIdeSetTransferMode(nIndexDrive, 0x09);
            //Mode1 timing specs are as follow:
            //Register 0x48, set at 0x65656565 for all drives(4)
            //Minimum recovery time = 180ns
            //Active pulse width = 210ns
            //For a total cycletime of 383ns.
            PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x58, 0x65656565);
            //Register 0x4C, set at 0x4F4F0055
            //Address setup time = 60ns
            //DIOR/DIOW pulse width = 150ns
            //DIOR/DIOW recovery time = - (put max to be safe)
            //AMD-766 datasheet notes that C1A4C register should be left to default value when using PIO
            //PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x5C, 0x4F4F0055);
        }
        else{                                                         //Mode0
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Trying to set PIO mode 0 (default).");
            n = BootIdeSetTransferMode(nIndexDrive, 0x08);
            //Mode0 timing specs are as follow:
            //Register 0x48, set at 0xA8A8A8A8 for all drives(4)
            //Minimum recovery time = 270ns
            //Active pulse width = 330ns
            //For a total cycletime of 600ns.
            PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x58, 0xA8A8A8A8);
            //Register 0x4C, set at 0x5F5F00AA
            //Address setup time = 90ns
            //DIOR/DIOW pulse width = 180ns
            //DIOR/DIOW recovery time = - (put max to be safe)
            //AMD-766 datasheet notes that C1A4C register should be left to default value when using PIO
            //PciWriteDword(BUS_0, DEV_9, FUNC_0, 0x5C, 0x5F5F00AA);
        }
        if(n){
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "BootIdeSetPIOMode:Drive %d: Cannot set PIO mode.", nIndexDrive);
            printk("\n       BootIdeSetPIOMode:Drive %d: Cannot set PIO mode.\n", nIndexDrive);
        }

//We'll give PIO Mode4 a shot first.
/*
        if(BootIdeSetTransferMode(nIndexDrive, 0x40 | 2)){  //Enable UDMA2 for drive. Best mode for 40 conductors cable.
                                                            //I don't care if you have a 80 conductors for now.
            printk("\n       Unable to init DMA for drive %u", nIndexDrive);
        }
        else
            tsaHarddiskInfo[nIndexDrive].m_fDMAInit=1;      //1 = Hardware init done.

        //Next steps in DMA would be to send a IDE_CMD_SET_FEATURES command selecting
        //a DMA transfer mode.
        //After, we need to set up a PRD table and send it to the hdd's controller.
        //Then we would be ready to put some data in memory and send DMA R/W commands to HDD.
*/
    }

    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Common init routine for both IDE and ATAPI devices.");
    if (drive_info[49] & 0x200) 
    { 
        /* bit 9 of capability word is lba supported bit */
        tsaHarddiskInfo[nIndexDrive].m_bLbaMode = IDE_DH_LBA;
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Drive is accessed by LBA.");
    } else {
        tsaHarddiskInfo[nIndexDrive].m_bLbaMode = IDE_DH_CHS;
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_WARN, "Drive is accessed by CHS(seriously?).");
    }

    //If drive is a hard disk, see what type of partitioning it has.
    if (!tsaHarddiskInfo[nIndexDrive].m_fAtapi) {
        //Check if drive support S.M.A.R.T.
        tsaHarddiskInfo[nIndexDrive].m_fHasSMARTcapabilities = drive_info[82] & 0x0001;
        if(tsaHarddiskInfo[nIndexDrive].m_fHasSMARTcapabilities){
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Drive support S.M.A.R.T..");
            //Is S.M.A.R.T. enabled?
            tsaHarddiskInfo[nIndexDrive].m_fSMARTEnabled = drive_info[85] & 0x0001;
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "S.M.A.R.T. is %s.", tsaHarddiskInfo[nIndexDrive].m_fSMARTEnabled ? "enabled" : "disabled");
            //Bit0 : Support Error Logging.
            //Bit1 : Support S.M.A.R.T. Self-tests.
            tsaHarddiskInfo[nIndexDrive].m_SMARTFeaturesSupported = drive_info[84] & 0x0003;
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Drive support S.M.A.R.T.%s%s.", (tsaHarddiskInfo[nIndexDrive].m_SMARTFeaturesSupported&0x1) ? " error logging" : "", (tsaHarddiskInfo[nIndexDrive].m_SMARTFeaturesSupported&0x2) ? " self-tests" : "");
        }
    } 
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Drive init done.");
    return 0;
}




/* -------------------------------------------------------------------------------- */
int DriveSecurityChange(unsigned uIoBase, int driveId, ide_command_t ide_cmd, unsigned char *password) {
    //Todo: Check drive is in correct state for command desired.
    char ide_cmd_data[2+512];    
    char baBuffer[512];
    unsigned short*    drive_info = (unsigned short*)baBuffer;
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    tsIdeCommandParams tsicp1 = IDE_DEFAULT_COMMAND;

    
    if(BootIdeWaitNotBusy(uIoBase)) 
    {
        return 1;
    }
    tsicp1.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(driveId);

    if (ide_cmd == IDE_CMD_SECURITY_SET_PASSWORD) {
        //Cromwell locks drives in high security mode (NOT maximum security mode).
        //This means that even if you lose the normal (user) password, you can
        //unlock them again using the master password set below.
        //Master password is TEAMASSEMBLY (in ascii, NULL padded)
        char *master_password="TEAMASSEMBLY";
        
        //We first lock the drive with the master password
        //Just in case we ever need to unlock it in an emergency
        memset(ide_cmd_data,0x00,512);
        //Set master password flag
        ide_cmd_data[0]|=0x01;
    
        memcpy(&ide_cmd_data[2],master_password,12);
        

        //Keep same Master Password Identifier.
        if(tsaHarddiskInfo[driveId].m_masterPassSupport != 0xFFFF)
            memcpy(&ide_cmd_data[34], &(tsaHarddiskInfo[driveId].m_masterPassSupport),2);

        if(BootIdeIssueAtaCommand(uIoBase, ide_cmd, &tsicp1, false)) return 1;
        BootIdeWaitDataReady(uIoBase);
        BootIdeWriteData(uIoBase, ide_cmd_data, IDE_SECTOR_SIZE);

        if(BootIdeWaitNotBusy(uIoBase)){
            printk("\n           Setting Master Password failed.");
            return 1;
        }
    }

    memset(ide_cmd_data,0x00,512);
    
    //Password is only 20 bytes long - the rest is 0-padded.
    memcpy(&ide_cmd_data[2],password,20);

    if(BootIdeIssueAtaCommand(uIoBase, ide_cmd, &tsicp1, false)) return 1;
        
    BootIdeWaitDataReady(uIoBase);
    BootIdeWriteData(uIoBase, ide_cmd_data, IDE_SECTOR_SIZE);
           
    if (BootIdeWaitNotBusy(uIoBase))    
    {
        return 1;
    }
    // check that we are unlocked
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(driveId);
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false)) 
    {
        return 1;
    }
    BootIdeWaitDataReady(uIoBase);
    if(BootIdeReadData(uIoBase, baBuffer, IDE_SECTOR_SIZE)) 
    {
        return 1;
    }

    tsaHarddiskInfo[driveId].m_securitySettings = drive_info[128];
    tsaHarddiskInfo[driveId].m_masterPassSupport = drive_info[92];
    //Success, hopefully.
    return 0;
}

int CalculateDrivePassword(int driveId, unsigned char *key, unsigned char *eepromPtr) {

    unsigned char baMagic[0x200], baKeyFromEEPROM[0x10], baEeprom[0x30];
    int nVersionHashing=0;
    //Ick - forward decl. Should remove this. 
    unsigned int BootHddKeyGenerateEepromKeyData(unsigned char *eeprom_data,unsigned char *HDKey);
    
    if(eepromPtr == NULL || key == NULL)
        return 1;

    memcpy(baEeprom, eepromPtr, 0x30); // first 0x30 bytes from EEPROM image we picked up earlier

    memset(&baKeyFromEEPROM,0x00,0x10);
    nVersionHashing = BootHddKeyGenerateEepromKeyData( baEeprom, baKeyFromEEPROM);
    memset(&baMagic,0x00,0x200);
    // Calculate the hdd pw from EEprom and Serial / Model Number
    HMAC_SHA1 (&baMagic[2], baKeyFromEEPROM, 0x10,
         tsaHarddiskInfo[driveId].m_szIdentityModelNumber,
         tsaHarddiskInfo[driveId].m_length,
         tsaHarddiskInfo[driveId].m_szSerial,
         tsaHarddiskInfo[driveId].s_length);

    //Failed to generate a key
    if (nVersionHashing==0 || nVersionHashing == 13) return 1;

    memcpy(key,&baMagic[2],20);
    return 0;
}

bool driveMasterPasswordUnlock(unsigned uIoBase, int driveId, const char *master_password){
    char ide_cmd_data[2+512];
    char baBuffer[512];
    unsigned short* drive_info = (unsigned short *)baBuffer;
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Trying master password unlock on drive %u", driveId);

    //IDE_CMD_SECURITY_UNLOCK
    if(BootIdeWaitNotBusy(uIoBase))
    {
        //printk("  %d:  Not Ready\n", driveId);
        return false;
    }
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(driveId);

    memset(ide_cmd_data,0x00,512);
    //Set master password flag
    ide_cmd_data[0]|=0x01;

    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Master password is: %s\n", master_password);
    memcpy(&ide_cmd_data[2],master_password,strlen(master_password));

    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_SECURITY_UNLOCK, &tsicp, false)) return 1;

    BootIdeWaitDataReady(uIoBase);
    BootIdeWriteData(uIoBase, ide_cmd_data, IDE_SECTOR_SIZE);

    if (BootIdeWaitNotBusy(uIoBase))
    {
        return false;
    }


    // check that we are unlocked
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false))
    {
        return false;
    }
    BootIdeWaitDataReady(uIoBase);
    if(BootIdeReadData(uIoBase, baBuffer, IDE_SECTOR_SIZE))
    {
        return false;
    }

    tsaHarddiskInfo[driveId].m_securitySettings = drive_info[128];
    tsaHarddiskInfo[driveId].m_masterPassSupport = drive_info[92];
    if((drive_info[128]&0x0004)==0x0004) //Drive in locked status, not good...
    {
        printk("\n              Unlock ATA command using Master Password failed.");
        return false;
    }


    //IDE_CMD_SECURITY_DISABLE
    if(BootIdeWaitNotBusy(uIoBase))
    {
        //printk("  %d:  Not Ready\n", driveId);
        return false;
    }

    //ide_cmd_data's content is the same for both commands so we'll just reuse it to disable lock.
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_SECURITY_DISABLE, &tsicp, false)) return 1;

    BootIdeWaitDataReady(uIoBase);
    BootIdeWriteData(uIoBase, ide_cmd_data, IDE_SECTOR_SIZE);

    if (BootIdeWaitNotBusy(uIoBase))
    {
        return false;
    }
    // check that we are unlocked
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false))
    {
        return false;
    }
    BootIdeWaitDataReady(uIoBase);
    if(BootIdeReadData(uIoBase, baBuffer, IDE_SECTOR_SIZE))
    {
        return false;
    }

    tsaHarddiskInfo[driveId].m_securitySettings = drive_info[128];
    tsaHarddiskInfo[driveId].m_masterPassSupport = drive_info[92];
    if((drive_info[128]&0x0002)==0x0002) //Drive security is still enabled.
    {
        printk("\n              Security Disable ATA command using Master Password failed.");
        return false;
    }
    //Success, surely.

    return true;
}


/////////////////////////////////////////////////
//  BootIdeInit
//
//  Called at boot-time to init and detect connected devices

int BootIdeInit(void)
{
    memset(&tsaHarddiskInfo[0],0x00,sizeof(struct tsHarddiskInfo));
    memset(&tsaHarddiskInfo[1],0x00,sizeof(struct tsHarddiskInfo));
    
    tsaHarddiskInfo[0].m_bCableConductors=40;
    tsaHarddiskInfo[1].m_bCableConductors=40;
    tsaHarddiskInfo[0].m_fDMAInit=0;            //DMA not initialized yet.
    tsaHarddiskInfo[1].m_fDMAInit=0;
    IoOutputByte(0xff60+0, 0x00); // stop bus mastering
    IoOutputByte(0xff60+2, 0x62); // DMA possible for both drives
    //Init both master and slave

    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "IDE init for Master drive");
    BootIdeDriveInit(IDE_BASE1, 0);
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "IDE init for Slave drive");
    BootIdeDriveInit(IDE_BASE1, 1);
        
    if(tsaHarddiskInfo[0].m_fDriveExists && !tsaHarddiskInfo[0].m_fAtapi)
    {
        unsigned int uIoBase = tsaHarddiskInfo[0].m_fwPortBase;
        tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;

        tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(0);
        IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);

        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Reading IDE cable information through Master drive");
        if(!BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false)) 
        {
            unsigned short waBuffer[256];
            BootIdeWaitDataReady(uIoBase);
            BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Read data back from Master drive");
            if(!BootIdeReadData(uIoBase, (unsigned char *)&waBuffer[0], IDE_SECTOR_SIZE)) 
            {
                if( ((waBuffer[93]&0xc000)!=0) && ((waBuffer[93]&0x8000)==0) && ((waBuffer[93]&0xe000)!=0x6000))     
                {
                    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Detected 80-conductors IDE cable");
                    tsaHarddiskInfo[0].m_bCableConductors=80;
                }
                else
                {
                    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Assume 40-conductors IDE cable");
                }

            } else {
                BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "Error getting data from IDENTIFY command");
            }
        }
    }


#ifndef SILENT_MODE
        VIDEO_ARGB=0xffffffff;
        printk("          Cable: ");
        VIDEO_ARGB=0xffc8c800;
        printk("      %u Conductor\n", tsaHarddiskInfo[0].m_bCableConductors);
#endif

    if(tsaHarddiskInfo[0].m_bCableConductors != 40) 
    {
        int nAta=0;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&2) nAta=1;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&4) nAta=2;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&8) nAta=3;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&16) nAta=4;
        if(tsaHarddiskInfo[0].m_wAtaRevisionSupported&32) nAta=5;
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "UDMA%u supported by Master drive", nAta);
    }
    return 0;
}

/* -------------------------------------------------------------------------------- */

/////////////////////////////////////////////////
//  BootIdeAtapiModeSense
//
//  returns the ATAPI extra error info block
/*
int BootIdeAtapiModeSense(int nDriveIndex, unsigned char bCodePage, unsigned char * pba, int nLengthMaxReturn) 
{
    unsigned uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;

    unsigned char ba[2048];
    int nReturn;

    if(!tsaHarddiskInfo[nDriveIndex].m_fDriveExists) return 4;

    memset(ba, 0, sizeof(ba));
    //memset(&ba[0], 0, 12);
    ba[0]=0x5a;
    ba[2]=bCodePage;
    ba[7]=(unsigned char)(sizeof(ba)>>8); 
    ba[8]=(unsigned char)sizeof(ba);

    if(BootIdeIssueAtapiPacketCommandAndPacket(nDriveIndex, ba)) 
    {
//            unsigned char bStatus=IoInputByte(IDE_REG_STATUS(uIoBase)), bError=IoInputByte(IDE_REG_ERROR(uIoBase));
//            printk("  Drive %d: BootIdeAtapiAdditionalSenseCode FAILED, status=%02X, error=0x%02X, ASC unavailable\n", nDriveIndex, bStatus, bError);
            return 1;
    }

    nReturn=IoInputByte(IDE_REG_LBA_MID(uIoBase));
    nReturn |=IoInputByte(IDE_REG_LBA_HIGH(uIoBase))<<8;
    if(nReturn>nLengthMaxReturn) nReturn=nLengthMaxReturn;
    BootIdeReadData(uIoBase, pba, nReturn);

    return nReturn;
}
*
/* -------------------------------------------------------------------------------- */

/////////////////////////////////////////////////
//  BootIdeAtapiAdditionalSenseCode
//
//  returns the ATAPI extra error info block

int BootIdeAtapiAdditionalSenseCode(int nDriveIndex, unsigned char * pba, int nLengthMaxReturn) 
{
    unsigned uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;

    unsigned char ba[2048];
    int nReturn;

    if(!tsaHarddiskInfo[nDriveIndex].m_fDriveExists) return 4;

    //memset(&ba[0], 0, 12);
    memset(ba, 0, sizeof(ba));
    ba[0]=0x03;
    ba[4]=0xfe;

    if(BootIdeIssueAtapiPacketCommandAndPacket(nDriveIndex, ba)) 
    {
            return 1;
    }

    nReturn=IoInputByte(IDE_REG_LBA_MID(uIoBase));
    nReturn |=IoInputByte(IDE_REG_LBA_HIGH(uIoBase))<<8;
    if(nReturn>nLengthMaxReturn) nReturn=nLengthMaxReturn;
    BootIdeReadData(uIoBase, pba, nReturn);

    return nReturn;
}
/* -------------------------------------------------------------------------------- */

bool BootIdeAtapiReportFriendlyError(int nDriveIndex, char * szErrorReturn, int nMaxLengthError)
{
    unsigned char ba[2048];
    char szError[512];
    int nReturn;
    bool f=true;

    memset(ba, 0, sizeof(ba));
    nReturn=BootIdeAtapiAdditionalSenseCode(nDriveIndex, ba, sizeof(ba));
    if(nReturn<12) {
        sprintf(szError, "Unable to get Sense Code\n");
        f=false;
    } else {
        sprintf(szError, "Sense key 0x%02X (%s), ASC=0x%02X, qualifier=0x%02X\n", ba[2]&0x0f, szaSenseKeys[ba[2]&0x0f], ba[12], ba[13]);
        VideoDumpAddressAndData(0, ba, nReturn);
    }

    strncpy(szErrorReturn, szError, nMaxLengthError);
    return f;
}

/* -------------------------------------------------------------------------------- */
/*
void BootIdeAtapiPrintkFriendlyError(int nDriveIndex)
{
    char sz[512];
    memset(&sz,0x00,sizeof(sz));
    BootIdeAtapiReportFriendlyError(nDriveIndex, sz, sizeof(sz));
    printk(sz);
}
*/

/* -------------------------------------------------------------------------------- */

/////////////////////////////////////////////////
//  BootIdeReadSector
//
//  Read an absolute sector from the device
//  knows if it should use ATA or ATAPI according to HDD or DVD
//  This is the main function for reading things from a drive

int BootIdeReadSector(int nDriveIndex, void * pbBuffer, unsigned int block, int byte_offset, int n_bytes) 
{
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned uIoBase;
    unsigned char baBufferSector[IDE_SECTOR_SIZE];
    unsigned int track;
    int status;
    unsigned char ideReadCommand = IDE_CMD_READ_MULTI_RETRY; /* 48-bit LBA */
    
    
    if(!tsaHarddiskInfo[nDriveIndex].m_fDriveExists) return 4;

    uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;

    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nDriveIndex);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);

    if ((nDriveIndex < 0) || (nDriveIndex >= 2) ||
        (tsaHarddiskInfo[nDriveIndex].m_fDriveExists == 0))
    {
        return 1;
    }

    if(tsaHarddiskInfo[nDriveIndex].m_fAtapi) 
    {
                   // CD - DVD ROM
        unsigned char ba[12];
        int nReturn;
	
	
        IoInputByte(IDE_REG_STATUS(uIoBase));
        if(IoInputByte(IDE_REG_STATUS(uIoBase)&1)) 
        {     // sticky error
            if(IoInputByte(IDE_REG_ERROR(uIoBase)&0x20)) 
            {     // needs attention
                if(BootIdeAtapiAdditionalSenseCode(nDriveIndex, ba, 2048)<12) 
                {     // needed as it clears NEED ATTENTION
//                    printk("BootIdeReadSector sees unit needs attention but failed giving it\n");
                } else {
//                    printk("BootIdeReadSector sees unit needs attention, gave it, current Error=%02X\n", IoInputByte(IDE_REG_ERROR(uIoBase)));
                }
            }
        }

        BootIdeWaitNotBusy(uIoBase);

        if(n_bytes<2048) 
        {
            return 1;
        }

        tsicp.m_wCylinder=2048;
        memset(ba, 0, sizeof(ba));
        ba[0]=0x28; 
        ba[2]=block>>24; 
        ba[3]=block>>16; 
        ba[4]=block>>8; 
        ba[5]=block; 
        ba[7]=0; 
        ba[8]=1;

	    
        if(BootIdeIssueAtapiPacketCommandAndPacket(nDriveIndex, ba)) 
        {
            return 1;
        }

        nReturn=IoInputByte(IDE_REG_LBA_MID(uIoBase));
        nReturn |=IoInputByte(IDE_REG_LBA_HIGH(uIoBase))<<8;

        if(nReturn>2048) nReturn=2048;
        status = BootIdeReadData(uIoBase, pbBuffer, nReturn);
        if (status != 0) 
        {
            while(1) 
            {
            	//XXX: Reduce wait delay length?
                wait_ms(50);
                status = BootIdeReadData(uIoBase, pbBuffer, nReturn);                
                if (status == 0) 
                {
                    break;
                }
            }
        }
        return 0;
    }

    if (tsaHarddiskInfo[nDriveIndex].m_wCountHeads > 8)
    {
        IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x0a);
    } else {
        IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x02);
    }

    tsicp.m_bCountSector = 1;

    
    
    if( block >= 0x10000000 ) 
    {     
        /* 48-bit LBA access required for this block */ 
        tsicp.m_bCountSectorExt = 0;   
        
         /* This routine can have a max LBA of 32 bits (due to unsigned int data type used for block parameter) */   
        
        tsicp.m_wCylinderExt = 0; /* 47:32 */   
        tsicp.m_bSectorExt = (block >> 24) & 0xff; /* 31:24 */   
        tsicp.m_wCylinder = (block >> 8) & 0xffff; /* 23:8 */   
        tsicp.m_bSector = block & 0xff; /* 7:0 */   
        tsicp.m_bDrivehead = IDE_DH_DRIVE(nDriveIndex) | IDE_DH_LBA;   
        ideReadCommand = IDE_CMD_READ_EXT;   
    
    } else {
            // Looks Like we do not have LBA 48 need
            if (tsaHarddiskInfo[nDriveIndex].m_bLbaMode == IDE_DH_CHS) 
            { 
            track = block / tsaHarddiskInfo[nDriveIndex].m_wCountSectorsPerTrack;
            
            tsicp.m_bSector = 1+(block % tsaHarddiskInfo[nDriveIndex].m_wCountSectorsPerTrack);
            tsicp.m_wCylinder = track / tsaHarddiskInfo[nDriveIndex].m_wCountHeads;
            tsicp.m_bDrivehead = IDE_DH_DEFAULT |
                IDE_DH_HEAD(track % tsaHarddiskInfo[nDriveIndex].m_wCountHeads) |
                IDE_DH_DRIVE(nDriveIndex) |
                IDE_DH_CHS;
        } else {

            tsicp.m_bSector = block & 0xff; /* lower byte of block (lba) */
            tsicp.m_wCylinder = (block >> 8) & 0xffff; /* middle 2 bytes of block (lba) */
            tsicp.m_bDrivehead = IDE_DH_DEFAULT | /* set bits that must be on */
                ((block >> 24) & 0x0f) | /* lower nibble of byte 3 of block */
                IDE_DH_DRIVE(nDriveIndex) |
                IDE_DH_LBA;
        }
        }       
        
    if(BootIdeIssueAtaCommand(uIoBase, ideReadCommand, &tsicp, false)) 
    {
        return 1;
    }
    
    if (n_bytes != IDE_SECTOR_SIZE)
    {
        status = BootIdeReadData(uIoBase, baBufferSector, IDE_SECTOR_SIZE);
        if (status == 0) {
            memcpy(pbBuffer, baBufferSector+byte_offset, n_bytes);
        
        } else {
            // UPS, it failed, but we are brutal, we try again ....
            while(1) {
                //XXX: Reduce wait delay length?
                wait_ms(50);
                status = BootIdeReadData(uIoBase, baBufferSector, IDE_SECTOR_SIZE);
                if (status == 0) {
                    memcpy(pbBuffer, baBufferSector+byte_offset, n_bytes);
                    break;
                }
            }
            
        }
    
    } else {
        status = BootIdeReadData(uIoBase, pbBuffer, IDE_SECTOR_SIZE);
        if (status!=0) {
            // UPS, it failed, but we are brutal, we try again ....
            while(1) {
                //XXX: Reduce wait delay length?
                wait_ms(50);
                status = BootIdeReadData(uIoBase, pbBuffer, IDE_SECTOR_SIZE);        
                if (status == 0) {
                    break;
                }
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------------- */



/////////////////////////////////////////////////
//  BootIdeWriteSector
//
// !!!!! EXPERIMENTAL

int BootIdeWriteSector(int nDriveIndex, const void * pbBuffer, unsigned int block, unsigned char retry)
{
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned uIoBase;
    unsigned int track;
    int status;
    unsigned char ideWriteCommand = IDE_CMD_WRITE_MULTI_RETRY; 
    
    if(!tsaHarddiskInfo[nDriveIndex].m_fDriveExists) return 4;

    uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;

    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nDriveIndex);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);

    if ((nDriveIndex < 0) || (nDriveIndex >= 2))
    {
        return 1;
    }

    if (tsaHarddiskInfo[nDriveIndex].m_wCountHeads > 8)
    {
        IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x0a);
    } else {
        IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x02);
    }

    tsicp.m_bCountSector = 1;
    
    if( block >= 0x10000000 ) 
    {     
        /* 48-bit LBA access required for this block */ 
        tsicp.m_bCountSectorExt = 0;   
        
         /* This routine can have a max LBA of 32 bits (due to unsigned int data type used for block parameter) */   
        tsicp.m_wCylinderExt = 0; /* 47:32 */   
        tsicp.m_bSectorExt = (block >> 24) & 0xff; /* 31:24 */   
        tsicp.m_wCylinder = (block >> 8) & 0xffff; /* 23:8 */   
        tsicp.m_bSector = block & 0xff; /* 7:0 */   
        tsicp.m_bDrivehead = IDE_DH_DRIVE(nDriveIndex) | IDE_DH_LBA;   
        ideWriteCommand = IDE_CMD_WRITE_EXT;
    } else {
            // Looks Like we do not have LBA 48 need
            if (tsaHarddiskInfo[nDriveIndex].m_bLbaMode == IDE_DH_CHS) 
            { 

            track = block / tsaHarddiskInfo[nDriveIndex].m_wCountSectorsPerTrack;
            
            tsicp.m_bSector = 1+(block % tsaHarddiskInfo[nDriveIndex].m_wCountSectorsPerTrack);
            tsicp.m_wCylinder = track / tsaHarddiskInfo[nDriveIndex].m_wCountHeads;
            tsicp.m_bDrivehead = IDE_DH_DEFAULT |
                IDE_DH_HEAD(track % tsaHarddiskInfo[nDriveIndex].m_wCountHeads) |
                IDE_DH_DRIVE(nDriveIndex) |
                IDE_DH_CHS;
        } else {

            tsicp.m_bSector = block & 0xff; /* lower byte of block (lba) */
            tsicp.m_wCylinder = (block >> 8) & 0xffff; /* middle 2 bytes of block (lba) */
            tsicp.m_bDrivehead = IDE_DH_DEFAULT | /* set bits that must be on */
                ((block >> 24) & 0x0f) | /* lower nibble of byte 3 of block */
                IDE_DH_DRIVE(nDriveIndex) |
                IDE_DH_LBA;
        }
        }       
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Send Write cmd.");
    if(BootIdeIssueAtaCommand(uIoBase, ideWriteCommand, &tsicp, false)) 
    {
        printk("ide error %02X...\n", IoInputByte(IDE_REG_ERROR(uIoBase)));
        return 1;
    }
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Send Write data.");
    status = BootIdeWriteData(uIoBase, pbBuffer, IDE_SECTOR_SIZE);

    if(retry > 0)
        retry -= 1;
    if(status && retry){ //Status set to 1 or 2 means error. retry count must not be 0.
        printk("\n                 BootIdeWriteSector: write sector failed. %u retry left.", retry);
        status = BootIdeWriteSector(nDriveIndex, pbBuffer, block, retry);      //Retry one more time.
    }
/*
    //Some drives requires a CACHE_FLUSH command being sent after each write command. We do it just to be sure.
    //So issue command. BSY has been cleared in BootIdeWriteData function
    tsicp = IDE_DEFAULT_COMMAND;
    //tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nDriveIndex);
    if(tsaHarddiskInfo[nDriveIndex].m_dwCountSectorsTotal >= 0x10000000)        //LBA48 drive
        BootIdeIssueAtaCommand(uIoBase, IDE_CMD_CACHE_FLUSH_EXT, &tsicp);
    else
        BootIdeIssueAtaCommand(uIoBase, IDE_CMD_CACHE_FLUSH, &tsicp);
*/
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "status=%u.", status);
    return status;
}

/* -------------------------------------------------------------------------------- */



/////////////////////////////////////////////////
//  BootIdeWriteMultiple
//
//  Write a block of sector in a single command instead of sector by sector.
//  This command will make the drive generate an IRQ only a the end of the block
//  Hard drives usually allow 16 sectors per blocks.
//
// !!!!! EXPERIMENTAL
// "pbBuffer" must be of size specified by "len"
// "len" is the size in sectors. For now support up to 256 sectors per issued command. 0 = 256.
// "StartLBA is the absolute LBA value of the start of the block
// "retry" is the number of time to try to write in the event the command would fail the first time.
//              Value of 3 will write once and retry 2 times if previously failed.

int BootIdeWriteMultiple(int nDriveIndex, void * pbBuffer, unsigned int startLBA, unsigned short len, unsigned char retry)
{
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned short remainingLen = (len == 0)? 256 : (len > 256) ? 256 : len;   //Set remainingLen to 256 if len == 0.
    unsigned char partialBlock = len % tsaHarddiskInfo[nDriveIndex].m_maxBlockTransfer;     //Size in sector of partial block.
    unsigned uIoBase;
    unsigned int track, bufferPtr = 0;
    int status;
    unsigned char ideWriteCommand = IDE_CMD_WRITE_MULTIPLE;

    if(!tsaHarddiskInfo[nDriveIndex].m_fDriveExists) return 4;

    uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;

    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "start:%u len:%u retry:%u", startLBA, len, retry);

    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nDriveIndex);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);

    if ((nDriveIndex < 0) || (nDriveIndex >= 2))
    {
        printk("\n               unknown drive\n");
        return 1;
    }

    if (tsaHarddiskInfo[nDriveIndex].m_wCountHeads > 8)
    {
        IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x0a);
    } else {
        IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x02);
    }

    tsicp.m_bCountSector = len; //0 = 256 sectors.

    if((startLBA + len) >= 0x10000000 )
    {
        /* 48-bit LBA access required for this block */
        tsicp.m_bCountSectorExt = 0;

         /* This routine can have a max LBA of 32 bits (due to unsigned int data type used for block parameter) */
        tsicp.m_wCylinderExt = 0; /* 47:32 */
        tsicp.m_bSectorExt = (startLBA >> 24) & 0xff; /* 31:24 */
        tsicp.m_wCylinder = (startLBA >> 8) & 0xffff; /* 23:8 */
        tsicp.m_bSector = startLBA & 0xff; /* 7:0 */
        tsicp.m_bDrivehead = IDE_DH_DRIVE(nDriveIndex) | IDE_DH_LBA;
        ideWriteCommand = IDE_CMD_WRITE_MULTIPLE_EXT;
    } else {
            // Looks Like we do not have LBA 48 need
            if (tsaHarddiskInfo[nDriveIndex].m_bLbaMode == IDE_DH_CHS)          //CHS mode, unlikely
            {

            track = startLBA / tsaHarddiskInfo[nDriveIndex].m_wCountSectorsPerTrack;

            tsicp.m_bSector = 1+(startLBA % tsaHarddiskInfo[nDriveIndex].m_wCountSectorsPerTrack);
            tsicp.m_wCylinder = track / tsaHarddiskInfo[nDriveIndex].m_wCountHeads;
            tsicp.m_bDrivehead = IDE_DH_DEFAULT |
                IDE_DH_HEAD(track % tsaHarddiskInfo[nDriveIndex].m_wCountHeads) |
                IDE_DH_DRIVE(nDriveIndex) |
                IDE_DH_CHS;
            } else {                                                            //LBA28 mode

                tsicp.m_bSector = startLBA & 0xff; /* lower byte of block (lba) */
                tsicp.m_wCylinder = (startLBA >> 8) & 0xffff; /* middle 2 bytes of block (lba) */
                tsicp.m_bDrivehead = IDE_DH_DEFAULT | /* set bits that must be on */
                        ((startLBA >> 24) & 0x0f) | /* lower nibble of byte 3 of block */
                        IDE_DH_DRIVE(nDriveIndex) |
                        IDE_DH_LBA;
            }
        }
    if(BootIdeIssueAtaCommand(uIoBase, ideWriteCommand, &tsicp, false))
    {
        printk("\n                      ide error %02X...\n", IoInputByte(IDE_REG_ERROR(uIoBase)));
        return 1;
    }

    while(remainingLen >= tsaHarddiskInfo[nDriveIndex].m_maxBlockTransfer && !status){
        status = BootIdeWriteData(uIoBase, (unsigned char *)pbBuffer + bufferPtr, tsaHarddiskInfo[nDriveIndex].m_maxBlockTransfer * IDE_SECTOR_SIZE);   //Size in bytes here.
        bufferPtr += (tsaHarddiskInfo[nDriveIndex].m_maxBlockTransfer * IDE_SECTOR_SIZE);
        remainingLen -= tsaHarddiskInfo[nDriveIndex].m_maxBlockTransfer;
    }

    if(partialBlock && !status) //We have a last block to send and everything is good up to now
        status = BootIdeWriteData(uIoBase, (unsigned char *)pbBuffer + bufferPtr, partialBlock * IDE_SECTOR_SIZE);   //Size in bytes here.

/*
    //Send FLUSH CACHE (0xE7)
    //BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Sending FLUSH CACHE\n");
    ideWriteCommand = IDE_CMD_CACHE_FLUSH;
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nDriveIndex);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);
    if(BootIdeIssueAtaCommand(uIoBase, ideWriteCommand, &tsicp))
    {
        printk("\n                      ide error %02X...\n", IoInputByte(IDE_REG_ERROR(uIoBase)));
        return 1;
    }
    //BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "FLUSH CACHE done");
*/    

    if(retry > 0)
        retry -= 1;
    if(status && retry){ //Status set to 1 or 2 means error. retry count must not be 0.
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "error! offset:%u retry:%u  remaining:%u", startLBA+bufferPtr, retry, remainingLen);
        printk("\n                 BootIdeWriteMultiple: write sector failed. %u retry left.", retry);

        //Retry (partial) block from the sector where it failed.
        status = BootIdeWriteMultiple(nDriveIndex, pbBuffer, startLBA, len, retry);      //Retry one more time.
    }
    return status;
}

int BootIdeFlushCache(int nDriveIndex)
{
    int result = 1;
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned short uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nDriveIndex);

    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "drive:%u   ext:%u   normal:%u", nDriveIndex, tsaHarddiskInfo[nDriveIndex].m_fFlushCacheExtSupported, tsaHarddiskInfo[nDriveIndex].m_fFlushCacheSupported);

    if(tsaHarddiskInfo[nDriveIndex].m_fFlushCacheExtSupported)        //LBA48 drive
    {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_TRACE, "Flush cache Ext");
        result = BootIdeIssueAtaCommand(uIoBase, IDE_CMD_CACHE_FLUSH_EXT, &tsicp, false);
    }
    else if(tsaHarddiskInfo[nDriveIndex].m_fFlushCacheSupported)
    {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_TRACE, "Flush cache Ext");
        result = BootIdeIssueAtaCommand(uIoBase, IDE_CMD_CACHE_FLUSH, &tsicp, false);
    }

    if(result)
    {
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_ERROR, "error : %u", result);
    }

    return result;
}

unsigned long long BootIdeGetSectorCount(int nDriveIndex)
{
    return tsaHarddiskInfo[nDriveIndex].m_dwCountSectorsTotal;
}

int BootIdeGetSectorSize(int nDriveIndex)
{
    // TODO: retrieve from IDENTIFY cmd
    return IDE_SECTOR_SIZE;
}

int BootIdeDeviceConnected(int nDriveIndex)
{
    return tsaHarddiskInfo[nDriveIndex].m_fDriveExists;
}

int BootIdeDeviceIsATAPI(int nDriveIndex)
{
    return tsaHarddiskInfo[nDriveIndex].m_fAtapi;
}

int BootIdeDeviceIsLocked(int nDriveIndex)
{
    return tsaHarddiskInfo[nDriveIndex].m_securitySettings & 0x0002;
}

/* -------------------------------------------------------------------------------- */

///////////////////////////////////////////////
//      BootIdeBootSectorHddOrElTorito
//
//  Attempts to load boot code from Hdd or from CDROM/DVDROM
//   If HDD, loads MBR from Sector 0, if CDROM, uses El Torito to load default boot sector
//
// returns 0 if *pbaResult loaded with (512-byte/Hdd, 2048-byte/Cdrom) boot sector
//  otherwise nonzero return indicates error type
/*
int BootIdeBootSectorHddOrElTorito(int nDriveIndex, unsigned char * pbaResult)
{
    static const unsigned char baCheck11hFormat[] = {
            0x00,0x43,0x44,0x30,0x30,0x31,0x01,0x45,
            0x4C,0x20,0x54,0x4F,0x52,0x49,0x54,0x4F,
            0x20,0x53,0x50,0x45,0x43,0x49,0x46,0x49,
            0x43,0x41,0x54,0x49,0x4F,0x4E
    };
    int n;
    unsigned int * pdw;

    if(!tsaHarddiskInfo[nDriveIndex].m_fDriveExists) return 4;

    if(tsaHarddiskInfo[nDriveIndex].m_fAtapi) {
*/
/******   Numbnut's guide to El Torito CD Booting   ********

  Sector 11h of a bootable CDROM looks like this (11h is a magic number)
  The unsigned int starting at +47h is the sector index of the 'boot catalog'

00000000: 00 43 44 30 30 31 01 45 : 4C 20 54 4F 52 49 54 4F    .CD001.EL TORITO
00000010: 20 53 50 45 43 49 46 49 : 43 41 54 49 4F 4E 00 00     SPECIFICATION..
00000020: 00 00 00 00 00 00 00 00 : 00 00 00 00 00 00 00 00    ................
00000030: 00 00 00 00 00 00 00 00 : 00 00 00 00 00 00 00 00    ................
00000040: 00 00 00 00 00 00 00 13 : 00 00 00 00 00 00 00 00    ................
*/
/*
        if(BootIdeReadSector(nDriveIndex, &pbaResult[0], 0x11, 0, 2048)) {
            bprintf("Unable to get first sector\n");
            return 1;
        }

        for(n=0;n<sizeof(baCheck11hFormat);n++) {
            if(pbaResult[n]!=baCheck11hFormat[n]) {
                bprintf("Sector 11h not bootable format\n");
                return 2;
            }
        }

        pdw=(unsigned int *)&pbaResult[0x47];
*/
/*
At sector 13h (in this example only), the boot catalog:

00000000: 01 00 00 00 4D 69 63 72 : 6F 73 6F 66 74 20 43 6F    ....Microsoft Co
00000010: 72 70 6F 72 61 74 69 6F : 6E 00 00 00 4C 49 55 AA    rporation...LIU.
(<--- validation entry)
00000020: 88 00 00 00 00 00 04 00 : 25 01 00 00 00 00 00 00    ........%.......
(<-- initial/default entry - 88=bootable, 04 00 = 4 x (512-byte virtual sectors),
  = 1 x 2048-byte CDROM sector in boot, 25 01 00 00 = starts at sector 0x125)
*/
/*
        if(BootIdeReadSector(nDriveIndex, &pbaResult[0], *pdw, 0, 2048)) {
            bprintf("Unable to get boot catalog\n");
            return 3;
        }

        if((pbaResult[0]!=1) || (pbaResult[0x1e]!=0x55) || (pbaResult[0x1f]!=0xaa)) {
            bprintf("Boot catalog header corrupt\n");
            return 4;
        }

        if(pbaResult[0x20]!=0x88) {
            bprintf("Default boot catalog entry is not bootable\n");
            return 4;
        }

        pdw=(unsigned int *)&pbaResult[0x28];
*/
/*
And so at sector 0x125 (in this example only), we finally see the boot code

00000000: FA 33 C0 8E D0 BC 00 7C : FB 8C C8 8E D8 52 E8 00    .3.....|.....R..
00000010: 00 5E 81 EE 11 00 74 12 : 81 FE 00 7C 75 75 8C C8    .^....t....|uu..
00000020: 3D 00 00 75 7F EA 37 00 : C0 07 C6 06 AE 01 33 90    =..u..7.......3.
...
000007E0: 00 00 00 00 00 00 00 00 : 00 00 00 00 00 00 00 00    ................
000007F0: 00 00 00 00 00 00 00 00 : 00 00 00 00 00 00 55 AA    ..............U.
*/
/*
        if(BootIdeReadSector(nDriveIndex, &pbaResult[0], *pdw, 0, 2048)) {
            bprintf("Unable to get boot catalog\n");
            return 3;
        }

        if((pbaResult[0x7fe]!=0x55) || (pbaResult[0x7ff]!=0xaa)) {
            bprintf("Boot sector does not have boot signature!\n");
            return 4;
        }

        return 0; // success

    } else { // HDD boot

        if(BootIdeReadSector(nDriveIndex, &pbaResult[0], 0, 0, 512)) {
            bprintf("Unable to get MBR\n");
            return 3;
        }

        if((pbaResult[0x1fe]!=0x55) || (pbaResult[0x1ff]!=0xaa)) {
            bprintf("Boot sector does not have boot signature!\n");
            return 4;
        }

        return 0; // succes
    }
}
*/

/* -------------------------------------------------------------------------------- */

int BootIdeSetTransferMode(int nIndexDrive, int nMode)
{
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned int uIoBase = tsaHarddiskInfo[nIndexDrive].m_fwPortBase;
    
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nIndexDrive);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);
    
    IoOutputByte(0xff60+2, 0x62); // DMA possible for both drives

    IoOutputByte(IDE_REG_CONTROL(uIoBase), 0x08); // enable interrupt
    IoOutputByte(IDE_REG_FEATURE(uIoBase), 0x01); // enable DMA

    if(BootIdeWaitNotBusy(uIoBase)) {
            printk("\n       BootIdeSetTransferMode:Drive %d: Not Ready\n", nIndexDrive);
            return 1;
    }
    {
        int nReturn=0;
        tsicp.m_bCountSector = (unsigned char)nMode;
        IoOutputByte(IDE_REG_FEATURE(uIoBase), 3); // set transfer mode subcmd
        nReturn=BootIdeIssueAtaCommand(uIoBase, IDE_CMD_SET_FEATURES, &tsicp, false);
        return nReturn;
    }
}

int BootIdeSetMultimodeSectors(unsigned char nIndexDrive, unsigned char nbSectors){
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned int uIoBase = tsaHarddiskInfo[nIndexDrive].m_fwPortBase;
    unsigned char buffer[512];
    unsigned short *ptr = (unsigned short *)buffer;
    
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(0);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);
    BootIdeWaitNotBusy(uIoBase);
    
    tsicp.m_bCountSector = nbSectors;           //Only relevant register(excluding Command register)
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_SET_MULTIPLE_MODE, &tsicp, false)){
        return 1;
    }

    BootIdeWaitNotBusy(uIoBase);
    
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false)) {     //Check back our change
        return 1;
    }

    BootIdeWaitDataReady(uIoBase);

    if(BootIdeReadData(uIoBase, buffer, IDE_SECTOR_SIZE))
    {
        return 1;
    }

    //check if it worked.
    if((nbSectors + 0x0100) == (ptr[59]&0x01FF)){	//bit8 should be 1 if MULTIPLE READ/WRITE commands are enabled.
        tsaHarddiskInfo[nIndexDrive].m_maxBlockTransfer = (ptr[59]&0x00FF);     //It worked
    }
    else{
        tsaHarddiskInfo[nIndexDrive].m_maxBlockTransfer = 0;       //Did not work.
    }
    return 0;                   //No error.
}

/*
//Set fastest PIO mode supported by HDD. Will probably be Mode4 since it was officially inserted in
//ATA-2 specs in 1996.
int BootIdeSetPIOMode(unsigned char nIndexDrive, unsigned short cycleTime){
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned int uIoBase = tsaHarddiskInfo[nIndexDrive].m_fwPortBase;

    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(0);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);
    BootIdeWaitNotBusy(uIoBase);
    IoOutputByte(IDE_REG_FEATURE(uIoBase), 0x03);       //Set transfer mode sub-command

    //Depending on PIO cycle time value returned by IDENTIFY command, we select a PIO mode.
    //Can be from 0 to 4. One thing important is that bit3 must be set to 1(0x08).
    //Bits 2 to 0 select the PIO mode.
    if(cycleTime <= 120)        //Mode4
        tsicp.m_bCountSector = 0x0C;
    else if(cycleTime <= 180)   //Mode3
        tsicp.m_bCountSector = 0x0B;
    else if(cycleTime <= 240)   //Mode2
        tsicp.m_bCountSector = 0x0A;
    else if(cycleTime <= 383)   //Mode1
        tsicp.m_bCountSector = 0x09;
    else                        //Mode0
        tsicp.m_bCountSector = 0x08;

    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_SET_FEATURES, &tsicp)){
        return 1;
    }

    BootIdeWaitNotBusy(uIoBase);

    return 0;                   //No error.
}
*/

//Send SMART commands. Specify input command by smart_cmd.
//Return true if error.
bool driveToggleSMARTFeature(int nDriveIndex, unsigned short smart_cmd){
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned int uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;
    unsigned char buffer[512];
    unsigned short *ptr = (unsigned short *)buffer;

    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(0);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);
    BootIdeWaitNotBusy(uIoBase);

    tsicp.m_wCylinder = 0xC24F; //Necessary for SMART subcommands.
    IoOutputByte(IDE_REG_FEATURE(uIoBase), smart_cmd); // set SMART operation subcmd
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_SMART, &tsicp, false))
        return true;

    BootIdeWaitNotBusy(uIoBase);

    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false)) {     //Check back our change
        return 1;
    }

    BootIdeWaitDataReady(uIoBase);

    if(BootIdeReadData(uIoBase, buffer, IDE_SECTOR_SIZE))
    {
        return 1;
    }

    //check if it worked.
    tsaHarddiskInfo[nDriveIndex].m_fHasSMARTcapabilities = ptr[82] & 0x0001;
    if(tsaHarddiskInfo[nDriveIndex].m_fHasSMARTcapabilities){
        //Is S.M.A.R.T. enabled?
        tsaHarddiskInfo[nDriveIndex].m_fSMARTEnabled = ptr[85] & 0x0001;
        //Bit0 : Support Error Logging.
        //Bit1 : Support S.M.A.R.T. Self-tests.
        tsaHarddiskInfo[nDriveIndex].m_SMARTFeaturesSupported = ptr[84] & 0x0003;
    }
    return false;
}

int driveSMARTRETURNSTATUS(int nDriveIndex){
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned int uIoBase = tsaHarddiskInfo[nDriveIndex].m_fwPortBase;
    int result = -1;    //Start assuming error.
    unsigned short w;
    unsigned char error;

    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(0);
    IoOutputByte(IDE_REG_DRIVEHEAD(uIoBase), tsicp.m_bDrivehead);
    BootIdeWaitNotBusy(uIoBase);

    tsicp.m_wCylinder = 0xC24F; //Necessary for SMART subcommands.
    tsicp.m_bSector = 0x1;      //Summary SMART error log
    IoOutputByte(IDE_REG_FEATURE(uIoBase), 0xDA); // 0xDA = SMART RETURN STATUS subcommand.
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_SMART, &tsicp, false))
        return -1;

    BootIdeWaitNotBusy(uIoBase);

    wait_us_blocking(1);

    w=IoInputByte(IDE_REG_LBA_MID(uIoBase));
    w|=(IoInputByte(IDE_REG_LBA_HIGH(uIoBase)))<<8;

    error=IoInputByte(IDE_REG_STATUS(uIoBase));
    if(error&1) { // error
        return -1;
    }

    if(w == 0xC24F)     //Everything is fine
        result = 0;
    else if(w == 0x2CF4)        //Threshold exceeded condition.
        result = 1;
    else{
        printk("\n       Weird SMART status : %04X", w);
        result = -1;            //wtf
    }

    return result;
}

int HDD_SECURITY_SendATACommand(int nIndexDrive, ide_command_t ATACommand, char * password, bool masterPassword){
    char ide_cmd_data[512];    
    char baBuffer[512];
    unsigned short*    drive_info = (unsigned short*)baBuffer;
    tsIdeCommandParams tsicp = IDE_DEFAULT_COMMAND;
    unsigned short uIoBase = tsaHarddiskInfo[nIndexDrive].m_fwPortBase;
    int i;
    
    BootIdeWaitNotBusy(uIoBase);
    
    tsicp.m_bDrivehead = IDE_DH_DEFAULT | IDE_DH_HEAD(0) | IDE_DH_CHS | IDE_DH_DRIVE(nIndexDrive);
    
    memset(ide_cmd_data,0x00,512);
    
    if(masterPassword && (ATACommand == IDE_CMD_SECURITY_UNLOCK || ATACommand == IDE_CMD_SECURITY_DISABLE)){
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "Setting bit for Master Password usage.");
    	//Set master password flag
    	ide_cmd_data[0]|=0x01;
    }
       
    memcpy(&ide_cmd_data[2],password,strlen(password));

    if(masterPassword && tsaHarddiskInfo[nIndexDrive].m_masterPassSupport != 0xFFFF && ATACommand == IDE_CMD_SECURITY_SET_PASSWORD){
        ide_cmd_data[34] = 1;
        BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_DEBUG, "HDD support MasterPassword Index. Setting this to \"1\".");
    }

    if(BootIdeIssueAtaCommand(uIoBase, ATACommand, &tsicp, false)){
        return 1;
    }
        
    BootIdeWaitDataReady(uIoBase);
    BootIdeWriteData(uIoBase, ide_cmd_data, IDE_SECTOR_SIZE);
           
    if (BootIdeWaitNotBusy(uIoBase))    
    {
        return 1;
    }
    
    // check that we are unlocked
    BootLogger(DEBUG_IDE_DRIVER, DBG_LVL_INFO, "Security change command sent. Refreshing drives properties.");
    if(BootIdeIssueAtaCommand(uIoBase, IDE_CMD_IDENTIFY, &tsicp, false)) 
    {
        return 1;
    }
    BootIdeWaitDataReady(uIoBase);
    if(BootIdeReadData(uIoBase, baBuffer, IDE_SECTOR_SIZE)) 
    {
        return 1;
    }

    tsaHarddiskInfo[nIndexDrive].m_securitySettings = drive_info[128];
    tsaHarddiskInfo[nIndexDrive].m_masterPassSupport = drive_info[92];
    //Success, hopefully.
    return 0;
}
