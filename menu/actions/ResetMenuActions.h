#ifndef _VIDEOMENUACTIONS_H_
#define _VIDEOMENUACTIONS_H_
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

void QuickReboot(void* ignored);
void SlowReboot(void* ignored);
void PowerOff(void* ignored);

void SlowRebootNoSave(void* ignored);
void QuickRebootNoSave(void* ignored);
void PowerOffNoSave(void* ignored);

#endif
