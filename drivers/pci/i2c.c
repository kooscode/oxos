 /***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "i2c.h"
#include "boot.h"
#include "memory_layout.h"
#include "lib/time/timeManagement.h"
// #include "xblast/HardwareIdentifier.h"
// #include "xblast/PowerManagement.h"
#include "string.h"

/*
    WriteToSMBus()    by Lehner Franz (franz@caos.at)
    ReadfromSMBus() by Lehner Franz (franz@caos.at)
*/

int WriteToSMBus(unsigned char Address,unsigned char bRegister,unsigned char Size,unsigned int Data_to_smbus)
{
    int nRetriesToLive=50;

    while(IoInputWord(I2C_IO_BASE+0)&0x0800) ;  // Franz's spin while bus busy with any master traffic

    while(nRetriesToLive--) {
        
        unsigned char b;
        unsigned int temp;
        
        IoOutputByte(I2C_IO_BASE+4, (Address<<1)|0);
        IoOutputByte(I2C_IO_BASE+8, bRegister);

        switch (Size) {
            case 4:
                IoOutputByte(I2C_IO_BASE+9, Data_to_smbus&0xff);
                IoOutputByte(I2C_IO_BASE+9, (Data_to_smbus >> 8) & 0xff );
                IoOutputByte(I2C_IO_BASE+9, (Data_to_smbus >> 16) & 0xff );
                IoOutputByte(I2C_IO_BASE+9, (Data_to_smbus >> 24) & 0xff );
                IoOutputWord(I2C_IO_BASE+6, 4);
                break;
            case 2:
                IoOutputWord(I2C_IO_BASE+6, Data_to_smbus&0xffff);
                break;
            default:    // 1
                IoOutputWord(I2C_IO_BASE+6, Data_to_smbus&0xff);
                break;
        }
    
    
        temp = IoInputWord(I2C_IO_BASE+0);
        IoOutputWord(I2C_IO_BASE+0, temp);  // clear down all preexisting errors
    
        switch (Size) {
            case 4:
                IoOutputByte(I2C_IO_BASE+2, 0x1d);    // unsigned int modus
                break;
            case 2:
                IoOutputByte(I2C_IO_BASE+2, 0x1b);    // unsigned short modus
                break;
            default:    // 1
                IoOutputByte(I2C_IO_BASE+2, 0x1a);    // unsigned char modus
                break;
        }

        b = 0;
        
        while( (b&0x36)==0 ) { b=IoInputByte(I2C_IO_BASE+0); }

        if ((b&0x10) != 0) {
            return ERR_SUCCESS;
        
        }
        
        wait_us_blocking(1);
    }
        
    return ERR_I2C_ERROR_BUS;

}



int ReadfromSMBus(unsigned char Address,unsigned char bRegister,unsigned char Size,unsigned int *Data_to_smbus)
{
    int nRetriesToLive=50;
    
    while(IoInputWord(I2C_IO_BASE+0)&0x0800) ;  // Franz's spin while bus busy with any master traffic

    while(nRetriesToLive--) {
        unsigned char b;
        int temp;
        
        IoOutputByte(I2C_IO_BASE+4, (Address<<1)|1);
        IoOutputByte(I2C_IO_BASE+8, bRegister);
        
        temp = IoInputWord(I2C_IO_BASE+0);
        IoOutputWord(I2C_IO_BASE+0, temp);  // clear down all preexisting errors
                
        switch (Size) {
            case 4:    
                IoOutputByte(I2C_IO_BASE+2, 0x0d);    // unsigned int modus ?
                break;
            case 2:
                IoOutputByte(I2C_IO_BASE+2, 0x0b);    // unsigned short modus
                break;
            default:
                IoOutputByte(I2C_IO_BASE+2, 0x0a);    // unsigned char
                break;
        }

        b = 0;
        
            
        while( (b&0x36)==0 ) { b=IoInputByte(I2C_IO_BASE+0); }

        if(b&0x24) {
            //printf("I2CTransmitByteGetReturn error %x\n", b);
        }
        
        if(!(b&0x10)) {
            //printf("I2CTransmitByteGetReturn no complete, retry\n");
        }
        else {
            switch (Size) {
                case 4:
                    IoInputByte(I2C_IO_BASE+6);
                    IoInputByte(I2C_IO_BASE+9);
                    IoInputByte(I2C_IO_BASE+9);
                    IoInputByte(I2C_IO_BASE+9);
                    IoInputByte(I2C_IO_BASE+9);
                    break;
                case 2:
                    *Data_to_smbus = IoInputWord(I2C_IO_BASE+6);
                    break;
                default:
                    *Data_to_smbus = IoInputByte(I2C_IO_BASE+6);
                    break;
            }
            

            return ERR_SUCCESS;

        }
        
    }
           
    return ERR_I2C_ERROR_BUS;
}

/* ************************************************************************************************************* */


/* --------------------- Normal 8 bit operations -------------------------- */


int I2CTransmitByteGetReturn(unsigned char bPicAddressI2cFormat, unsigned char bDataToWrite)
{
    unsigned int temp;
    if (ReadfromSMBus(bPicAddressI2cFormat,bDataToWrite,1,&temp) != ERR_SUCCESS)
        return ERR_I2C_ERROR_BUS;
    return temp;
}


// transmit a word, no returned data from I2C device

int I2CTransmitWord(unsigned char bPicAddressI2cFormat, unsigned short wDataToWrite)
{
    return WriteToSMBus(bPicAddressI2cFormat,(wDataToWrite>>8)&0xff,1,(wDataToWrite&0xff));
}


int I2CWriteBytetoRegister(unsigned char bPicAddressI2cFormat, unsigned char bRegister, unsigned char wDataToWrite)
{
    return WriteToSMBus(bPicAddressI2cFormat,bRegister,1,(wDataToWrite&0xff));
    
}


void I2CModifyBits(unsigned char bAds, unsigned char bReg, unsigned char bData, unsigned char bMask)
{
    unsigned char b=I2CTransmitByteGetReturn(0x45, bReg)&(~bMask);
    I2CTransmitWord(0x45, (bReg<<8)|((bData)&bMask)|b);
}

// ----------------------------  PIC challenge/response -----------------------------------------------------------

int I2cSetFrontpanelLed(unsigned char b)
{
    I2CTransmitWord( 0x10, 0x800 | b);  // sequencing thanks to Jarin the Penguin!
    I2CTransmitWord( 0x10, 0x701);


    return ERR_SUCCESS;
}

bool I2CGetTemperature(int * pnLocalTemp, int * pExternalTemp)
{
    unsigned char cpuTempCount = 0;
    unsigned char cpu, cpudec;
    float temp1, cpuFrac = 0.0;

    //read temps
    ReadfromSMBus(0x10, 0x0A, 1, pExternalTemp);
    *pnLocalTemp=I2CTransmitByteGetReturn(0x4c, 0x01);

    // if errors, likely a 1.6..
    if (*pnLocalTemp==ERR_I2C_ERROR_BUS || *pExternalTemp==ERR_I2C_ERROR_BUS)
    {
      //1.6 specific Temperature code.
      //GPU trim
      *pExternalTemp *= 0.8f;    //Might be unnecessary.

      //Fetch CPU temp
      while(cpuTempCount < 10)
      {
          // if its a 1.6 then we get the CPU temperature from the xcalibur
          IoOutputByte(0xc004, (0x70 << 1) | 0x01);  // address
          IoOutputByte(0xc008, 0xC1);                // command
          IoOutputWord(0xc000, IoInputWord(0xc000));      // clear errors
          IoOutputByte(0xc002, 0x0d);                // start block transfer
          while ((IoInputByte(0xc000) & 8));         // wait for response

          if (!(IoInputByte(0xc000) & 0x23)) // if there was a error then just skip this read..
          {
              IoInputByte(0xc004);                       // read out the data reg (no. bytes in block, will be 4)
              cpudec = IoInputByte(0xc009);              // first byte
              cpu    = IoInputByte(0xc009);              // second byte
              IoInputByte(0xc009);                       // read out the two last bytes, dont' think its neccesary
              IoInputByte(0xc009);                       // but done to be on the safe side

              /* the temperature recieved from the xcalibur is very jumpy, so we try and smooth it
                out by taking the average over 10 samples */
              temp1 = (float)cpu + (float)cpudec / 256;
              temp1 /= 10;
              cpuFrac += temp1;

              // if we have taken 10 samples then commit the new temperature
              if (cpuTempCount == 9)
              { 
                  *pnLocalTemp = cpuFrac;
                  break;
              }
              else
              {
                  cpuTempCount++;     // increase sample count
              }

          }//end if error
        } //end while cputempcount
    } //endif error check
    
  return true;
}

void I2CRebootQuick(void) {
    //if(canPowerDown())
    {
        WriteToSMBus(0x10,0x02,1,0x01);
        while (1);
    }
}


void I2CRebootSlow(void)
{
    //if(canPowerDown())
    {
        WriteToSMBus(0x10,0x02,1,0x40);
        while (1);
    }
}

void I2CPowerOff(void)
{
    //if(canPowerDown())
    {
        WriteToSMBus(0x10,0x02,1,0x80);
        while (1);
    }
}

unsigned char I2CGetFanSpeed(void){
    //ReadfromSMBus(0x10, 0x10, 1, (unsigned int *)&temp);
    return (I2CTransmitByteGetReturn(0x10, 0x10) << 1);
}

void I2CSetFanSpeed(unsigned char speed)
{
/*    unsigned char giveUp = 0;
    do {
        WriteToSMBus(0x10,0x05,1,1);             //Activate manual fan speed control
        wait_us(5);
        WriteToSMBus(0x10,0x06,1,speed >> 1);    //Send new speed to PIC
    }while((I2CGetFanSpeed() != (speed >> 1)) && giveUp++ < 10);    //If the Xbox is hard of hearing, repeat max 10 times.
*/
	WriteToSMBus(0x10,0x06,1,speed >> 1);    //Send new speed to PIC, divide incoming value by 2.
	wait_us_blocking(5);
        WriteToSMBus(0x10,0x05,1,1);             //Activate manual fan speed control
        wait_us_blocking(5);
        WriteToSMBus(0x10,0x06,1,speed >> 1);    //Send new speed to PIC
}


//Return coded Xbox revision. Check enum in boot.h
//Thanks XBMC team for the code.
//TODO: switch case for cleaner look?
unsigned char I2CGetXboxMBRev(void)
{
  return 0x07; //UNKNOWN

    // unsigned char result = XboxMotherboardRevision_UNKNOWN;
    // unsigned int temp[3];
    // char ver[4];
    // ver[3] = 0;        //Terminator.
    // I2CTransmitWord(0x10, 0x0100);                //Reset ID counter.
    // ReadfromSMBus(0x10, 0x01, 1, &temp[0]);
    // ReadfromSMBus(0x10, 0x01, 1, &temp[1]);
    // ReadfromSMBus(0x10, 0x01, 1, &temp[2]);
    // ver[0] = (char)temp[0];
    // ver[1] = (char)temp[1];
    // ver[2] = (char)temp[2];

    // if ( !strcmp(ver,("01D")) || !strcmp(ver,("D01")) || !strcmp(ver,("1D0")) || !strcmp(ver,("0D1"))) {
    //     result = XboxMotherboardRevision_DEVKIT;
    // }
    // else if (!strcmp(ver,("DBG")) || !strcmp(ver,("B11"))){
    //     result = XboxMotherboardRevision_DEBUGKIT;
    // }
    // else if (!strcmp(ver,("P01"))){
    //     result = XboxMotherboardRevision_1_0;
    // }
    // else if (!strcmp(ver,("P05"))){
    //     result = XboxMotherboardRevision_1_1;
    // }
    // else if (!strcmp(ver,("P11")) || !strcmp(ver,("1P1")) || !strcmp(ver,("11P"))){
    //     if(ReadfromSMBus(0x6A, 0x00, 0, temp) == 0){
    //         result = XboxMotherboardRevision_1_4;
    //     }
    //     else {
    //         result = XboxMotherboardRevision_1_2;
    //     }
    // }
    // else if (!strcmp(ver,("P2L"))){
    //     result = XboxMotherboardRevision_1_6;
    // }
    // else {
    //     result = XboxMotherboardRevision_UNKNOWN;
    // }
    // return result;
}
