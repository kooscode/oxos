/*
 * MenuInits.h
 *
 *  Created on: Sep 14, 2016
 *      Author: bennyboy
 */

#ifndef MENU_MENUINITS_H_
#define MENU_MENUINITS_H_

#include "MenuActions.h"
#include "stdlib.h"

void IconMenuInit(void);

TEXTMENU* ResetMenuInit(void);

TEXTMENU* TextMenuInit(void);

void LargeHDDMenuDynamic(void* drive);

#endif /* MENU_MENUINITS_H_ */
