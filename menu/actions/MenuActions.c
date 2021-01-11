/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "boot.h"
#include "video.h"
#include "MenuInits.h"
#include "include/BootLogger.h"
#include "lib/cromwell/cromString.h"
#include "lib/cromwell/cromSystem.h"
#include "lib/time/timeManagement.h"
#include <stddef.h>

void AdvancedMenu(void *textmenu)
{
    TextMenu((TEXTMENU*)textmenu, NULL);
}

void DrawChildTextMenu(void* menu)
{
    TEXTMENU* menuPtr = (TEXTMENU*)menu;
    TextMenu(menuPtr, menuPtr->firstMenuItem);
    //freeTextMenuAllocMem(menuPtr);
}

void ResetDrawChildTextMenu(TEXTMENU* menu)
{
    BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "Drawing menu %s", menu->szCaption);
    TextMenu(menu, menu->firstMenuItem);
    BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "Exiting menu %s", menu->szCaption);
    freeTextMenuAllocMem(menu);
    BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "Returning to previous menu");
}

void dynamicDrawChildTextMenu(void* menuInitFct)
{
    TEXTMENU* (*fctPtr)(void) = menuInitFct;
    if(NULL == menuInitFct)
    {
        return;
    }

    TEXTMENU* menu = (*fctPtr)();
    BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "Generated menu %s", menu->szCaption);
    ResetDrawChildTextMenu(menu);
}

void DrawLargeHDDTextMenu(unsigned char drive)
{
    breakOutOfMenu = 1;
    // LargeHDDMenuDynamic((void *)&drive);
    //Memory allocation freeing is done in ResetDrawChildTextMenu which is called by LargeHDDMenuInit.
}

void freeTextMenuAllocMem(TEXTMENU* menu)
{
    TEXTMENUITEM* currentItem = menu->firstMenuItem;
    TEXTMENUITEM* nextItem;
    int itemCount = 0;

    if(menu != NULL)
    {
        BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "freeing menu %s", menu->szCaption);
        while(currentItem != NULL)
        {
            nextItem = currentItem->nextMenuItem;
            BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "free menu item : %s", currentItem->szCaption);
            if(currentItem->functionDataPtr != NULL && currentItem->dataPtrAlloc)
            {
                BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "free alloc param");
                free(currentItem->functionDataPtr);
            }
            free(currentItem);
            currentItem = nextItem;
        }

        //Finally free menuPtr since it no longer points to an allocated item entry.
        free(menu);
        menu = NULL;
    }
}

void UiHeader(char *title)
{
    BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
    VIDEO_ATTR=0xffffef37;
    printk("\n\n\2       %s\2\n\n\n", title);
}

void UIFooter(void)
{
    VIDEO_ATTR=0xffc8c8c8;
    printk("\n\n           Press Button 'B' or 'Back' to return.");
    while(cromwellLoop())
    {
        if(risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_B) == 1 || risefall_xpad_STATE(XPAD_STATE_BACK) == 1)
        {
            break;
        }
    }
    // initialSetLED(LPCmodSettings.OSsettings.LEDColor);
}
