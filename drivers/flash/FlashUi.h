/*
 * FlashUi.h
 *
 *  Created on: Dec 12, 2016
 *      Author: cromwelldev
 */

#ifndef FLASHUI_H_
#define FLASHUI_H_

#include <stdbool.h>

void FlashFileFromBuffer(unsigned char *fileBuf, unsigned int fileSize, bool askConfirm);
void BootShowFlashDevice(void);
bool SaveXBlastOSSettings(void);


bool executeFlashDriverUI(void);

#endif /* FLASHUI_H_ */
