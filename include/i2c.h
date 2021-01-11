/*
 * i2c.h
 *
 *  Created on: Sep 14, 2016
 *      Author: bennyboy
 */

#ifndef INCLUDE_I2C_H_
#define INCLUDE_I2C_H_

#include <stdbool.h>

int WriteToSMBus(unsigned char Address,unsigned char bRegister,unsigned char Size,unsigned int Data_to_smbus);
int ReadfromSMBus(unsigned char Address,unsigned char bRegister,unsigned char Size,unsigned int *Data_to_smbus);
int I2CTransmitByteGetReturn(unsigned char bPicAddressI2cFormat, unsigned char bDataToWrite);
int I2CTransmitWord(unsigned char bPicAddressI2cFormat, unsigned short wDataToWrite);
int I2CWriteBytetoRegister(unsigned char bPicAddressI2cFormat, unsigned char bRegister, unsigned char wDataToWrite);

int I2cSetFrontpanelLed(unsigned char b);
bool I2CGetTemperature(int * pnLocalTemp, int * pExternalTemp);
void I2CRebootQuick(void);
void I2CRebootSlow(void);
void I2CPowerOff(void);
unsigned char I2CGetFanSpeed(void);
void I2CSetFanSpeed(unsigned char speed);
unsigned char I2CGetXboxMBRev(void);


#endif /* INCLUDE_I2C_H_ */
