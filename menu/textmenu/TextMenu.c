/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "TextMenu.h"
#include "boot.h"
#include "video.h"
// #include "lib/LPCMod/BootLCD.h"
#include "lib/cromwell/cromSystem.h"
#include "string.h"
// #include "xblast/settings/xblastSettingsDefs.h"
// #include "xblast/settings/xblastSettingsChangeTracker.h"
#include "lib/cromwell/cromString.h"
#include "lib/time/timeManagement.h"
// #include "include/BootLogger.h"

int breakOutOfMenu = 0;
unsigned int temp, oldTemp; 
int timeRemain = 0;
int oldTimeRemain = 0;
int visibleCount = 0;



void TextMenuDraw(TEXTMENU *menu, TEXTMENUITEM *firstVisibleMenuItem, TEXTMENUITEM *selectedItem);

void TextMenuAddItem(TEXTMENU *menu, TEXTMENUITEM *newMenuItem)
{
    TEXTMENUITEM *menuItem = menu->firstMenuItem;
    TEXTMENUITEM *currentMenuItem=NULL;
    
    while (menuItem != NULL)
    {
        currentMenuItem = menuItem;
        menuItem = menuItem->nextMenuItem;
    }
    
    if (currentMenuItem == NULL)
    {
        //This is the first icon in the chain
        menu->firstMenuItem = newMenuItem;
    }
    //Append to the end of the chain
    else
    {
        currentMenuItem->nextMenuItem = newMenuItem;
    }
    newMenuItem->nextMenuItem = NULL;
    newMenuItem->previousMenuItem = currentMenuItem; 
}

void TextMenuAddItemInOrder(TEXTMENU *menu, TEXTMENUITEM *newMenuItem)
{
    TEXTMENUITEM *menuItem = menu->firstMenuItem;
    TEXTMENUITEM *currentMenuItem=NULL;

    if(NULL == menuItem)
    {
        /* This is the first icon in the chain */
        menu->firstMenuItem = newMenuItem;
        newMenuItem->nextMenuItem = NULL;
        newMenuItem->previousMenuItem = NULL;
    }
    else
    {
        while (menuItem != NULL)
        {
            currentMenuItem = menuItem;
            if(0 > strcasecmp(newMenuItem->szCaption, menuItem->szCaption))
            {
                /* New szCaption alphabetically comes first */
                newMenuItem->previousMenuItem = menuItem->previousMenuItem;
                menuItem->previousMenuItem = newMenuItem;
                if(NULL != newMenuItem->previousMenuItem)
                {
                    newMenuItem->previousMenuItem->nextMenuItem = newMenuItem;
                }
                else
                {
                    menu->firstMenuItem = newMenuItem;
                    newMenuItem->previousMenuItem = NULL;
                }
                newMenuItem->nextMenuItem = menuItem;

                /* Next item of menuItem stays the same */

                return;
            }
            menuItem = menuItem->nextMenuItem;
        }
        currentMenuItem->nextMenuItem = newMenuItem;
        newMenuItem->previousMenuItem = currentMenuItem;
        newMenuItem->nextMenuItem = NULL;
    }
}

void TextMenuDraw(TEXTMENU* menu, TEXTMENUITEM *firstVisibleMenuItem, TEXTMENUITEM *selectedItem)
{
    TEXTMENUITEM *item = NULL;
    int menucount;    

    VIDEO_CURSOR_POSX = 75;
    VIDEO_CURSOR_POSY = 45;
    
    //Draw the menu title.
    VIDEO_ATTR = 0xff00ff;

    printk("\2          %s\n\2", menu->szCaption);
    
    if(temp != 0)
    {
        // If we have a timeout running...
        printk("  (%i)\2", timeRemain);
    }

    VIDEO_CURSOR_POSY+=20;
    
    //Draw the menu items
    //VIDEO_CURSOR_POSX=150;
    
    //If we were moving up, the 
    
    item=firstVisibleMenuItem;

    visibleCount = menu->visibleCount;

    if(visibleCount == 0)
    {
        visibleCount = 10;
    }

    for (menucount=0; menucount<visibleCount; menucount++)
    {
        if (item==NULL)
        {
            //No more menu items to draw
            break;
        }
        //Selected item in yellow
        if (item == selectedItem)
        {
            VIDEO_ATTR=0xffef37;
        }
        //If noSelect flag, draw in red.
        else if(item->noSelect == NOSELECTERROR)
        {
            VIDEO_ATTR=0xffff1515;
        }
        else
        {
            VIDEO_ATTR=0xffffff;
        }
        //Font size 2=big.
        if(menu->smallChars)
        {
            printk("               %s%s\n",item->szCaption, item->szParameter);
        }
        else
        {
            printk("\2               %s%s\n",item->szCaption, item->szParameter);
        }
        item=item->nextMenuItem;
    }

    // if(menu->hideUncommittedChangesLabel == false)
    // {
    //     unsigned char uncommittedChanges = LPCMod_CountNumberOfChangesInSettings(false, NULL);
    //     uncommittedChanges += generateEEPROMChangeList(false, NULL); //do not generate strings
    //     if(LPCMod_checkForBootScriptChanges())
    //     {
    //         uncommittedChanges += 1;
    //     }

    //     if(LPCMod_checkForBackupEEPROMChange())
    //     {
    //         uncommittedChanges += 1;
    //     }

    //     if(uncommittedChanges > 0)
    //     {
    //         //There are settings that have changed.
    //         VIDEO_CURSOR_POSY = vmode.height - 30;
    //         VIDEO_CURSOR_POSX = vmode.width - 480;
    //         VIDEO_ATTR=0x88c8c8c8;
    //         printk("Uncommitted changes: %u", uncommittedChanges);
    //     }
    // }

    // textMenuLCDPrint(menu, selectedItem);
}

void TextMenu(TEXTMENU *menu, TEXTMENUITEM *selectedItem)
{
    temp = menu->timeout;
    unsigned int COUNT_start;
    COUNT_start = getMS();

    TEXTMENUITEM *selectedMenuItem, *firstVisibleMenuItem;
    BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
    
    if (selectedItem!=NULL) selectedMenuItem = selectedItem;
    else selectedMenuItem = menu->firstMenuItem;
    
    firstVisibleMenuItem = menu->firstMenuItem;
    TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
    
    //Main menu event loop.
    while(cromwellLoop())
    {
        if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_UP) == 1)
        {
            oldTemp = temp;
            temp = 0;
            if(oldTemp != 0)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            }

            if (selectedMenuItem->previousMenuItem!=NULL)
            {
                if (firstVisibleMenuItem == selectedMenuItem)
                {
                    firstVisibleMenuItem = selectedMenuItem->previousMenuItem;
                    BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                }
                selectedMenuItem = selectedMenuItem->previousMenuItem;
                TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
            }
        } 
        else if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_DOWN) == 1)
        {
            oldTemp = temp;
            temp = 0;
            if(oldTemp != 0)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            }

            int i=0;
            if (selectedMenuItem->nextMenuItem!=NULL)
            {
                if(selectedMenuItem->nextMenuItem->noSelect == 0) //Do not select items with noSelect flag
                {
                    TEXTMENUITEM *lastVisibleMenuItem = firstVisibleMenuItem;
                    //8 menu items per page.
                    for (i=0; i<visibleCount-1; i++)
                    {
                        if (lastVisibleMenuItem->nextMenuItem==NULL)
                        {
                            break;
                        }
                        lastVisibleMenuItem = lastVisibleMenuItem->nextMenuItem;
                    }
                    if (selectedMenuItem == lastVisibleMenuItem)
                    {
                        firstVisibleMenuItem = firstVisibleMenuItem->nextMenuItem;
                        BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                    }
                    selectedMenuItem = selectedMenuItem->nextMenuItem;
                    TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
                }
            }
        }
        else if (risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_A) == 1 ||
                 risefall_xpad_STATE(XPAD_STATE_START) == 1 ||
                 (unsigned int)temp>(0x369E99*MENU_TIMEWAIT))
        {
            temp = 0;
            BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            VIDEO_ATTR=0xffffff;
            //Menu item selected - invoke function pointer.

            if (selectedMenuItem->functionPtr!=NULL)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                VIDEO_ATTR=0xffffff;
                selectedMenuItem->functionPtr(selectedMenuItem->functionDataPtr);
            }
            //Clear the screen again    
            BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            VIDEO_ATTR=0xffffff;
            //Did the function that was run set the 'Quit the menu' flag?
            if (breakOutOfMenu)
            {
                breakOutOfMenu=0;
                return;
            }
            //We need to redraw ourselves
            TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
        }
        else if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_LEFT) == 1)
        {
            oldTemp = temp;
            temp = 0;
            if(oldTemp != 0)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            }

            VIDEO_ATTR=0xffffff;
            //Menu item selected - invoke function pointer.
            if (selectedMenuItem->functionLeftPtr!=NULL)
            {
                selectedMenuItem->functionLeftPtr(selectedMenuItem->functionLeftDataPtr);
                //Clear the screen again    
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                VIDEO_ATTR=0xffffff;
                //Did the function that was run set the 'Quit the menu' flag?
                if (breakOutOfMenu)
                {
                    breakOutOfMenu=0;
                    return;
                }
            }
            //We need to redraw ourselves
            TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
        }
        else if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_RIGHT) == 1)
        {
            oldTemp = temp;
            temp = 0;
            if(oldTemp != 0)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            }

            VIDEO_ATTR=0xffffff;
            //Menu item selected - invoke function pointer.
            if (selectedMenuItem->functionRightPtr!=NULL)
            {
                selectedMenuItem->functionRightPtr(selectedMenuItem->functionRightDataPtr);
                //Clear the screen again    
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                VIDEO_ATTR=0xffffff;
                //Did the function that was run set the 'Quit the menu' flag?
                if (breakOutOfMenu)
                {
                    breakOutOfMenu=0;
                    return;
                }
            }
            //We need to redraw ourselves
            TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
        }
        else if (risefall_xpad_BUTTON(TRIGGER_XPAD_TRIGGER_RIGHT) == 1)
        {
            wait_ms(50);
            oldTemp = temp;
            temp = 0;
            if(oldTemp != 0)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            }

            VIDEO_ATTR=0xffffff;
            //Menu item selected - invoke function pointer.
            if (selectedMenuItem->functionRTPtr!=NULL)
            {
                selectedMenuItem->functionRTPtr(selectedMenuItem->functionRTDataPtr);
                //Clear the screen again    
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                VIDEO_ATTR=0xffffff;
                //Did the function that was run set the 'Quit the menu' flag?
                if (breakOutOfMenu)
                {
                    breakOutOfMenu=0;
                    return;
                }
            }
            //We need to redraw ourselves
            TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
        }
        else if (risefall_xpad_BUTTON(TRIGGER_XPAD_TRIGGER_LEFT) == 1)
        {
            wait_ms(50);
            oldTemp = temp;
            temp = 0;
            if(oldTemp != 0)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            }

            VIDEO_ATTR=0xffffff;
            //Menu item selected - invoke function pointer.
            if (selectedMenuItem->functionLTPtr!=NULL)
            {
                selectedMenuItem->functionLTPtr(selectedMenuItem->functionLTDataPtr);
                //Clear the screen again    
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                VIDEO_ATTR=0xffffff;
                //Did the function that was run set the 'Quit the menu' flag?
                if (breakOutOfMenu)
                {
                    breakOutOfMenu=0;
                    return;
                }
            }
            //We need to redraw ourselves
            TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
        }

        else if (risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_B) == 1 ||
                 risefall_xpad_STATE(XPAD_STATE_BACK) == 1)
        {
            BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            VIDEO_ATTR=0xffffff;
            temp = 0;
            bypassConfirmDialog[0] = 0;    //reset for confirmDialog
            return;
        }

        if (temp != 0)
        {
            temp = getMS() - COUNT_start;
            oldTimeRemain = timeRemain;
            timeRemain = MENU_TIMEWAIT - temp/1000;
            if (oldTimeRemain != timeRemain)
            {
                BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
                TextMenuDraw(menu, firstVisibleMenuItem, selectedMenuItem);
            }
        }
    }
}
