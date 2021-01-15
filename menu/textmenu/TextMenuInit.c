/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "MenuInits.h"
#include "config.h"
// #include "lpcmod_v1.h"
#include "VideoInitialization.h"
#include "string.h"
// #include "lib/LPCMod/BootLPCMod.h"
// #include "xblast/HardwareIdentifier.h"

TEXTMENU *TextMenuInit(void)
{
    
    TEXTMENUITEM *itemPtr;
    TEXTMENU *menuPtr;
    
    //Create the root menu - MANDATORY
    menuPtr = malloc(sizeof(TEXTMENU));
    memset(menuPtr,0x00,sizeof(TEXTMENU)); // Sets timeout and visibleCount to 0.
    strcpy(menuPtr->szCaption, "MAIN MENU");
    menuPtr->firstMenuItem=NULL;

    // Power Menu
    itemPtr = calloc(1, sizeof(TEXTMENUITEM));
    strcpy(itemPtr->szCaption, "Power menu");
    itemPtr->functionPtr=dynamicDrawChildTextMenu;
    itemPtr->functionDataPtr = ResetMenuInit;
    TextMenuAddItem(menuPtr, itemPtr);
    
    return menuPtr;
}
