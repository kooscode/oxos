#ifndef _MENUACTIONS_H_
#define _MENUACTIONS_H_
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "TextMenu.h"

//For the icon->text menu jump
void AdvancedMenu(void *menu);

//Used to display child menus
void DrawChildTextMenu(void* menu);
void ResetDrawChildTextMenu(TEXTMENU* menu);
void dynamicDrawChildTextMenu(void* menuInitFct);
void DrawLargeHDDTextMenu(unsigned char drive);

void SetWidescreen(void *);
void SetVideoStandard(void *);
void SetLEDColor(void *);

void freeTextMenuAllocMem(TEXTMENU* menu);

void UiHeader(char *title);
void UIFooter(void);

#endif
