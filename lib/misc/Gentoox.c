/**
 * Gentoox loader functions.
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

#include "boot.h"
#include "cromwell.h"
#include "lib/time/timeManagement.h"

void errorLED(void) {
    setLED("rxxx");
}

void busyLED(void) {
    setLED("rgog");
}

void inputLED(void) {
    setLED("oxox");
}

void goodLED(void) {
    setLED("gxgx");
}

void importantLED(void) {
    setLED("roro");
}

void downloadingLED(void) {
    setLED("ogrr");
}

void uberLED(void) {
    setLED("rxrx");
}

void highLED(void) {
    setLED("rrrr");
}

void midLED(void) {
    setLED("ooox");
}

void lowLED(void) {
    setLED("gggg");
}

void dots(void) {
   wait_ms(100);
   printk(".");
   wait_ms(100);
   printk(".");
   wait_ms(100);
   printk(".");
   wait_ms(100);
}

void cromwellError(void) {
   VIDEO_ARGB=0xffd8d8d8;
   printk("\t[ ");
   VIDEO_ARGB=0xffff0000;
   printk("!!");
   VIDEO_ARGB=0xffd8d8d8;
   printk(" ]");
   errorLED();
}

void cromwellWarning(void) {
   VIDEO_ARGB=0xffd8d8d8;
   printk("\t[ ");
   VIDEO_ARGB=0xffffae01;
   printk("!!");
   VIDEO_ARGB=0xffd8d8d8;
   printk(" ]\n");
   errorLED();
}

void cromwellSuccess(void) {
   VIDEO_ARGB=0xffd8d8d8;
   printk("\t[ ");
   VIDEO_ARGB=0xff00ff00;
   printk("ok");
   VIDEO_ARGB=0xffd8d8d8;
   printk(" ]\n");
}
