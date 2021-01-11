#ifndef _GENTOOX_H_
#define _GENTOOX_H_
/**
 * Gentoox loader includes.
 * Copyright (C) Thomas "ShALLaX" Pedley (gentoox@shallax.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

void errorLED(void);
void busyLED(void);
void inputLED(void);
void goodLED(void);
void importantLED(void);
void downloadingLED(void);
void uberLED(void);
void highLED(void);
void midLED(void);
void lowLED(void);
void dots(void);
void cromwellError(void);
void cromwellWarning(void);
void cromwellSuccess(void);
#endif
