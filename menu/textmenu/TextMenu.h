#ifndef _TEXTMENU_H_
#define _TEXTMENU_H_

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <stdbool.h>
struct TEXTMENUITEM;
struct TEXTMENU;

#define MENUCAPTIONSIZE 73
#define NOSELECTERROR   1

#define FULL_KEYBOARD   0

extern int breakOutOfMenu;

typedef struct TEXTMENUITEM {
    //Menu item text
    char szCaption[MENUCAPTIONSIZE+1];
    //Parameter value text
    char szParameter[MENUCAPTIONSIZE+1];
    //Pointer to function to run when menu item selected.
    //If NULL, menuitem will not do anything when selected
    void (*functionPtr) (void *);
    //Pointer to data used by the function above.
    void *functionDataPtr;
    bool dataPtrAlloc;
    //DataPtr below never get any allocated data
    void (*functionLeftPtr) (void *);
    void (*functionRightPtr) (void *);
    void *functionLeftDataPtr;
    void *functionRightDataPtr;
    void (*functionLTPtr) (void *);
    void (*functionRTPtr) (void *);
    void *functionLTDataPtr;
    void *functionRTDataPtr;
    unsigned char noSelect;
    //Next / previous menu items within this menu
    struct TEXTMENUITEM *previousMenuItem;
    struct TEXTMENUITEM *nextMenuItem;
} TEXTMENUITEM;

typedef struct TEXTMENU {
    //Menu title e.g. "Main Menu"
    char szCaption[MENUCAPTIONSIZE+1];
    //A pointer to the first item of the linked list of menuitems that
    //make up this menu.
    TEXTMENUITEM* firstMenuItem;
    unsigned int timeout;
    int visibleCount;
    bool smallChars;
    bool hideUncommittedChangesLabel;
} TEXTMENU;

extern char bypassConfirmDialog[50];

void TextMenuAddItem(TEXTMENU *menu, TEXTMENUITEM *newMenuItem);
void TextMenuAddItemInOrder(TEXTMENU *menu, TEXTMENUITEM *newMenuItem);
void textMenuLCDPrint(TEXTMENU *menu, TEXTMENUITEM *selectedItem);
void TextMenu(TEXTMENU *menu, TEXTMENUITEM *selectedItem);
#endif
