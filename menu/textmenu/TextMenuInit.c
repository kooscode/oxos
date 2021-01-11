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

    
    // if(isXBlastCompatible())         //No need to display this menu if no modchip is present.
    // {
    //     //XBlast(modchip) SETTINGS MENU
    //     itemPtr = calloc(1, sizeof(TEXTMENUITEM));
    //     strcpy(itemPtr->szCaption, "XBlast settings");
    //     itemPtr->functionPtr=dynamicDrawChildTextMenu;
    //     itemPtr->functionDataPtr = ModchipMenuInit;
    //     TextMenuAddItem(menuPtr, itemPtr);
    // }

    // //SYSTEM SETTINGS MENU
    // itemPtr = calloc(1, sizeof(TEXTMENUITEM));
    // strcpy(itemPtr->szCaption, "System settings");
    // itemPtr->functionPtr=dynamicDrawChildTextMenu;
    // itemPtr->functionDataPtr = SystemMenuInit;
    // TextMenuAddItem(menuPtr, itemPtr);

    // if(isLCDSupported())    //No need to display this menu if no modchip is present.
    // {                       //LCD is supported on SmartXX & X3 chips.
    //     //LCD SETTINGS MENU
    //     itemPtr = calloc(1, sizeof(TEXTMENUITEM));
    //     strcpy(itemPtr->szCaption, "LCD settings");
    //     itemPtr->functionPtr=dynamicDrawChildTextMenu;
    //     itemPtr->functionDataPtr = LCDMenuInit;
    //     TextMenuAddItem(menuPtr, itemPtr);
    // }

//     //TOOLS MENU
//     itemPtr = calloc(1, sizeof(TEXTMENUITEM));
//     strcpy(itemPtr->szCaption, "Tools");
//     itemPtr->functionPtr=dynamicDrawChildTextMenu;
//     itemPtr->functionDataPtr = ToolsMenuInit;
//     TextMenuAddItem(menuPtr, itemPtr);


// #ifdef FLASH
//     if(isXBlastOnTSOP())
//     {
//         //FLASH MENU
//         itemPtr = calloc(1, sizeof(TEXTMENUITEM));
//         strcpy(itemPtr->szCaption, "Flash menu");
//         if(LPCmodSettings.OSsettings.TSOPcontrol)
//         {
//             itemPtr->functionPtr = TSOPBankSelectMenuDynamic;
//         }
//         else
//         {
//             itemPtr->functionPtr = BankSelectDynamic;
//             itemPtr->functionDataPtr = malloc(sizeof(FlashBank));
//             *(FlashBank *)itemPtr->functionDataPtr = FlashBank_NoBank;
//             itemPtr->dataPtrAlloc = true;
//         }
//         TextMenuAddItem(menuPtr, itemPtr);
//     }
//     else if(isXBlastOnLPC())
//     {
//         //FLASH MENU
//         itemPtr = calloc(1, sizeof(TEXTMENUITEM));
//         strcpy(itemPtr->szCaption, "Flash menu");
//         itemPtr->functionPtr = dynamicDrawChildTextMenu;
//         itemPtr->functionDataPtr = BankSelectMenuInit;
//         TextMenuAddItem(menuPtr, itemPtr);
//     }
//     else
//     {
//         //FLASH MENU. Shown when no XBlast modchip is detected.
//         itemPtr = calloc(1, sizeof(TEXTMENUITEM));
//         strcpy(itemPtr->szCaption, "Flash menu");
//         itemPtr->functionPtr = BankSelectDynamic;
//         itemPtr->functionDataPtr = malloc(sizeof(FlashBank));
//         *(FlashBank *)itemPtr->functionDataPtr = FlashBank_NoBank;
//         itemPtr->dataPtrAlloc = true;
//         TextMenuAddItem(menuPtr, itemPtr);
//     }
// #endif

//     //HDD MENU
//     itemPtr = calloc(1, sizeof(TEXTMENUITEM));
//     strcpy(itemPtr->szCaption, "HDD menu");
//     itemPtr->functionPtr=dynamicDrawChildTextMenu;
//     itemPtr->functionDataPtr = HDDMenuInit;
//     TextMenuAddItem(menuPtr, itemPtr);

//     //CD MENU
//     itemPtr = calloc(1, sizeof(TEXTMENUITEM));
//     strcpy(itemPtr->szCaption, "CD menu");
//     itemPtr->functionPtr=dynamicDrawChildTextMenu;
//     itemPtr->functionDataPtr = CDMenuInit;
//     TextMenuAddItem(menuPtr, itemPtr);

//     // Info Menu
//     itemPtr = calloc(1, sizeof(TEXTMENUITEM));
//     strcpy(itemPtr->szCaption, "Info menu");
//     itemPtr->functionPtr=dynamicDrawChildTextMenu;
//     itemPtr->functionDataPtr = InfoMenuInit;
//     TextMenuAddItem(menuPtr, itemPtr);

    // Power Menu
    itemPtr = calloc(1, sizeof(TEXTMENUITEM));
    strcpy(itemPtr->szCaption, "Power menu");
    itemPtr->functionPtr=dynamicDrawChildTextMenu;
    itemPtr->functionDataPtr = ResetMenuInit;
    TextMenuAddItem(menuPtr, itemPtr);
    
    return menuPtr;
}
