/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

/* This is where you should customise the menu, by adding your own icons.
 * The code in IconMenu.c should normally be left alone.
 */

#include "MenuInits.h"
#include "config.h"
#include "BootIde.h"
#include "IconMenu.h"
#include "MenuActions.h"
#include <stddef.h>
#include "string.h"

#define BNKFULLTSOP    0x00u
#define NOBNKID  0xFF

void IconMenuInit(void)
{
    selectedIcon=NULL;

    // icon512BankIcon = calloc(1, sizeof(ICON));
    // icon512BankIcon->iconSlot = ICON_SOURCE_SLOT5;
    // icon512BankIcon->bankID = BNK512;
    // icon512BankIcon->functionPtr = assertBankScriptExecBankBoot;
    // icon512BankIcon->functionDataPtr = malloc(sizeof(unsigned char));
    // *(unsigned char *)icon512BankIcon->functionDataPtr = BNK512;
    // icon512BankIcon->dataPtrAlloc = true;

    // icon256BankIcon = calloc(1, sizeof(ICON));
    // icon256BankIcon->iconSlot = ICON_SOURCE_SLOT4;
    // icon256BankIcon->bankID = BNK256;
    // icon256BankIcon->functionPtr = assertBankScriptExecBankBoot;
    // icon256BankIcon->functionDataPtr = malloc(sizeof(unsigned char));
    // *(unsigned char *)icon256BankIcon->functionDataPtr = BNK256;
    // icon256BankIcon->dataPtrAlloc = true;

    // iconSplitTSOPBank0Icon = calloc(1, sizeof(ICON));
    // iconSplitTSOPBank0Icon->iconSlot = ICON_SOURCE_SLOT3;
    // iconSplitTSOPBank0Icon->bankID = BNKTSOPSPLIT0;
    // iconSplitTSOPBank0Icon->functionPtr = assertBankScriptExecTSOPBoot;
    // iconSplitTSOPBank0Icon->functionDataPtr = malloc(sizeof(unsigned char));
    // *(unsigned char *)iconSplitTSOPBank0Icon->functionDataPtr = BNKTSOPSPLIT0;
    // iconSplitTSOPBank0Icon->dataPtrAlloc = true;

    // iconSplitTSOPBank1Icon = calloc(1, sizeof(ICON));
    // iconSplitTSOPBank1Icon->iconSlot = ICON_SOURCE_SLOT3;
    // iconSplitTSOPBank1Icon->szCaption = "Boot OnBoard Bank1";
    // iconSplitTSOPBank1Icon->bankID = BNKTSOPSPLIT1;
    // iconSplitTSOPBank1Icon->functionPtr = assertBankScriptExecTSOPBoot;
    // iconSplitTSOPBank1Icon->functionDataPtr = malloc(sizeof(unsigned char));
    // *(unsigned char *)iconSplitTSOPBank1Icon->functionDataPtr = BNKTSOPSPLIT1;
    // iconSplitTSOPBank1Icon->dataPtrAlloc = true;

    iconFullTSOPBankIcon = calloc(1, sizeof(ICON));
    iconFullTSOPBankIcon->iconSlot = ICON_SOURCE_SLOT3;
    iconFullTSOPBankIcon->bankID = BNKFULLTSOP;
    iconFullTSOPBankIcon->functionPtr = AdvancedMenu;//(); assertBankScriptExecTSOPBoot;
    iconFullTSOPBankIcon->functionDataPtr = TextMenuInit(); //malloc(sizeof(unsigned char));
    *(unsigned char *)iconFullTSOPBankIcon->functionDataPtr = BNKFULLTSOP;
    iconFullTSOPBankIcon->dataPtrAlloc = true;

    advancedMenuIcon = (ICON *)malloc(sizeof(ICON));
    advancedMenuIcon->iconSlot = ICON_SOURCE_SLOT2;
    advancedMenuIcon->szCaption = "Settings";
    advancedMenuIcon->bankID = NOBNKID;
    advancedMenuIcon->functionPtr = AdvancedMenu;
    advancedMenuIcon->functionDataPtr = TextMenuInit();
    advancedMenuIcon->dataPtrAlloc = false;
}

