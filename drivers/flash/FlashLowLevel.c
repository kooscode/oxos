/*
 * FlashLowLevel.c
 *
 *  Created on: Oct 18, 2016
 *      Author: cromwelldev
 */

#include "FlashLowLevel.h"
#include "memory_layout.h"
#include "string.h"
#include "stdio.h"
#include "lib/time/timeManagement.h"
#include "include/BootLogger.h"

// Variables
static bool is28xxxProtocol;
bool firstBusyRead;
unsigned char lastStatusRegisterState;

// Internal functions
static inline bool _28xxxDeviceIsBusy(void);
static inline bool _29xxxDeviceIsBusy(void);
static void _ResetFlashICStateMachine(void);
static inline void _28xxxResetFlashICStateMachine(void);
static inline void _29xxxResetFlashICStateMachine(void);

static void _ReadDeviceIDBytes(KNOWN_FLASH_TYPE* output);
static void _28xxxReadDeviceID(KNOWN_FLASH_TYPE* output);
static void _29xxxReadDeviceID(KNOWN_FLASH_TYPE* output);

static bool _MatchDevice(const KNOWN_FLASH_TYPE* output);

static inline void _28xxxWriteBytes(unsigned char byte, unsigned int addr);
static inline void _29xxxWriteBytes(unsigned char byte, unsigned int addr);

static inline void _28xxxSectorErase(unsigned int addr);
static inline void _29xxxSectorErase(unsigned int addr);
static inline void _29xxxBlockErase(unsigned int addr);
static inline void _29xxxChipErase(void);
static void _29xxxCommonEraseSequence(void);


void FlashLowLevel_Init(void)
{
    is28xxxProtocol = false;
    memset(&flashDevice, 0x00, sizeof(flashDevice));
	flashDevice.m_pbMemoryMappedStartAddress = (unsigned char *)LPCFlashadress;
	BootLogger(DEBUG_FLASH_LOWLEVEL, DBG_LVL_DEBUG, "Setting LPC address to 0x%08X", LPCFlashadress);
}

bool FlashLowLevel_ReadDevice(void)
{
    KNOWN_FLASH_TYPE flashRead;
    bool retry = true;

    firstBusyRead = true;

    while(1)
    {
        _ResetFlashICStateMachine();
        _ReadDeviceIDBytes(&flashRead);

        BootLogger(DEBUG_FLASH_LOWLEVEL, DBG_LVL_DEBUG, "Read device ID. manf=0x%02X  dev=0x%02X", flashRead.m_bManufacturerId, flashRead.m_bDeviceId);

        if(_MatchDevice(&flashRead))
        {
            // Found Device.
            BootLogger(DEBUG_FLASH_LOWLEVEL, DBG_LVL_DEBUG, "Found matching device: %s", flashDevice.flashType.m_szFlashDescription);
            BootLogger(DEBUG_FLASH_LOWLEVEL, DBG_LVL_DEBUG, "Additional info: %s", flashDevice.m_szAdditionalErrorInfo);
            BootLogger(DEBUG_FLASH_LOWLEVEL, DBG_LVL_DEBUG, "Can Erase/Write: %s", flashDevice.m_fIsBelievedCapableOfWriteAndErase ? "Yes" : "No");
            return true;
        }

        if(retry == true)
        {
            // Device not found, try other method.
            is28xxxProtocol = !is28xxxProtocol;
            BootLogger(DEBUG_FLASH_LOWLEVEL, DBG_LVL_WARN, "is28xxxProtocol: %s", is28xxxProtocol ? "true" : "false");
            retry = false;
        }
        else
        {
            // Tried both method. No device found.
            BootLogger(DEBUG_FLASH_LOWLEVEL, DBG_LVL_WARN, "Device not found...");
            return false;
        }
    }
}

bool FlashLowLevel_DeviceIsBusy(void)
{
    if(is28xxxProtocol)
    {
        return _28xxxDeviceIsBusy();
    }

    return _29xxxDeviceIsBusy();
}

static inline bool _28xxxDeviceIsBusy(void)
{
    lastStatusRegisterState = flashDevice.m_pbMemoryMappedStartAddress[0];

    if(lastStatusRegisterState & 0x80)
    {
        // Device is busy
        return true;
    }

    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0x50;
    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0xff;

    if(lastStatusRegisterState & 0x7e)
    {
        flashDevice.m_fIsBelievedCapableOfWriteAndErase = false;
        if(lastStatusRegisterState & 0x08)
        {
            strcpy(flashDevice.m_szAdditionalErrorInfo, "This chip requires +5V on pin 11 (Vpp).");
        }
        else
        {
            sprintf(flashDevice.m_szAdditionalErrorInfo, "Chip Status after Erase: 0x%02X", lastStatusRegisterState);
        }
    }

    return false;
}

static inline bool _29xxxDeviceIsBusy(void)
{
    unsigned char oldStatusByteValue = lastStatusRegisterState;
    lastStatusRegisterState = flashDevice.m_pbMemoryMappedStartAddress[0];

    if(firstBusyRead || ((oldStatusByteValue & 0x40) != (lastStatusRegisterState & 0x40)))
    {
        firstBusyRead = false;
        return true;
    }

    firstBusyRead = true;
    return false;
}

static void _ResetFlashICStateMachine(void)
{
	if(is28xxxProtocol)
	{
		_28xxxResetFlashICStateMachine();
	}
	else
	{
		_29xxxResetFlashICStateMachine();
	}
}

static inline void _28xxxResetFlashICStateMachine(void)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0xff;
}

static inline void _29xxxResetFlashICStateMachine(void)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0xaa;
    flashDevice.m_pbMemoryMappedStartAddress[0x2aaa]=0x55;
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0xf0;
}

static void _ReadDeviceIDBytes(KNOWN_FLASH_TYPE* output)
{
    if(is28xxxProtocol)
    {
        _28xxxReadDeviceID(output);
    }
    else
    {
        _29xxxReadDeviceID(output);
    }
}
static void _28xxxReadDeviceID(KNOWN_FLASH_TYPE* output)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0x90;
    output->m_bManufacturerId = FlashLowLevel_ReadByte(0);
    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0x90;
    output->m_bDeviceId = FlashLowLevel_ReadByte(1);
}

static void _29xxxReadDeviceID(KNOWN_FLASH_TYPE* output)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0xaa;
    flashDevice.m_pbMemoryMappedStartAddress[0x2aaa] = 0x55;
    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0x90;
    output->m_bManufacturerId = FlashLowLevel_ReadByte(0);
    output->m_bDeviceId = FlashLowLevel_ReadByte(1);
    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0xf0;
}

static bool _MatchDevice(const KNOWN_FLASH_TYPE* output)
{
    const KNOWN_FLASH_TYPE aknownflashtypesDefault[] =
    {
        #include "flashtypes.h"
    };

    unsigned int i = 0, n = 0;

    while(aknownflashtypesDefault[i].m_bDeviceId != 0)
    {
        if(aknownflashtypesDefault[i].m_bManufacturerId == output->m_bManufacturerId &&
           aknownflashtypesDefault[i].m_bDeviceId == output->m_bDeviceId)
        {
            flashDevice.flashType = aknownflashtypesDefault[i];
            flashDevice.m_szAdditionalErrorInfo[0] = '\0';
            flashDevice.m_fIsBelievedCapableOfWriteAndErase = true;

            if(is28xxxProtocol)
            {
                flashDevice.m_pbMemoryMappedStartAddress[0x5555] =  0x90;
                if(FlashLowLevel_ReadByte(0x03) != 0)
                {
                    i = sprintf(flashDevice.m_szAdditionalErrorInfo, "%s","Master Lock SET  "); // reuse 'i'
                    flashDevice.m_fIsBelievedCapableOfWriteAndErase = false;

                    i += sprintf(&flashDevice.m_szAdditionalErrorInfo[i],"Block(64KB) Locks: ");

                    while(n < flashDevice.flashType.m_dwLengthInBytes)
                    {
                        flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0x90;
                        i += sprintf(&flashDevice.m_szAdditionalErrorInfo[i], "%u", FlashLowLevel_ReadByte(n|0x0002) & 1);
                        n += 0x10000;
                    }

                    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0x50;
                }
            }

            return true;
        }

        i++;
    }

    return false;
}

void FlashLowLevel_WriteByte(unsigned char byte, unsigned int addr)
{
    if(is28xxxProtocol)
    {
        _28xxxWriteBytes(byte, addr);
    }
    else
    {
        _29xxxWriteBytes(byte, addr);
    }
}

static inline void _28xxxWriteBytes(unsigned char byte, unsigned int addr)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0x40;
    flashDevice.m_pbMemoryMappedStartAddress[addr]=byte;

    // Sharp has a problem, does not go busy for ~500nS
    wait_us_blocking(1);
}

static inline void _29xxxWriteBytes(unsigned char byte, unsigned int addr)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0xaa;
    flashDevice.m_pbMemoryMappedStartAddress[0x2aaa]=0x55;
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0xa0;
    flashDevice.m_pbMemoryMappedStartAddress[addr]=byte;
}

void FlashLowLevel_InititiateSectorErase(unsigned int addr) // 4KB
{
    if(is28xxxProtocol)
    {
        _28xxxSectorErase(addr);
    }
    else
    {
        _29xxxSectorErase(addr);
    }
}

static inline void _28xxxSectorErase(unsigned int addr)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555] = 0x50;
    flashDevice.m_pbMemoryMappedStartAddress[addr] = 0x20;
    flashDevice.m_pbMemoryMappedStartAddress[addr] = 0xd0;

    // Sharp has a problem, does not go busy for ~500nS
    wait_us_blocking(1);
}

static inline void _29xxxSectorErase(unsigned int addr)
{
    _29xxxCommonEraseSequence();
    flashDevice.m_pbMemoryMappedStartAddress[addr]=0x30;
}

void FlashLowLevel_InititiateBlockErase(unsigned int addr)  // 64KB
{
    if(is28xxxProtocol)
    {
        return;
    }

    _29xxxBlockErase(addr);
}

static inline void _29xxxBlockErase(unsigned int addr)
{
    _29xxxCommonEraseSequence();
    flashDevice.m_pbMemoryMappedStartAddress[addr]=0x50;
}

void FlashLowLevel_InititiateChipErase(void)
{
    if(is28xxxProtocol)
    {
        // 28xxx does not support chip erase
        return;
    }

    _29xxxChipErase();
}

static inline void _29xxxChipErase(void)
{
    _29xxxCommonEraseSequence();
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0x10;
}

static void _29xxxCommonEraseSequence(void)
{
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0xaa;
    flashDevice.m_pbMemoryMappedStartAddress[0x2aaa]=0x55;
    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0x80;

    flashDevice.m_pbMemoryMappedStartAddress[0x5555]=0xaa;
    flashDevice.m_pbMemoryMappedStartAddress[0x2aaa]=0x55;
}
