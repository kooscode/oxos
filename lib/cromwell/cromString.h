/*
 * cromString.h
 *
 *  Created on: Aug 8, 2016
 *      Author: cromwelldev
 */

#ifndef CROMSTRING_H_
#define CROMSTRING_H_

#include <stddef.h>

int printk(const char *szFormat, ...);
unsigned int centerPrintK(int XPos, int YPos, const char *szFormat, ...);
unsigned int centerScreenPrintk(int YPos,const char *szFormat, ...);

#endif /* CROMSTRING_H_ */
