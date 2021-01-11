/*
 * cromString.c
 *
 *  Created on: Aug 8, 2016
 *      Author: cromwelldev
 */

#include "cromString.h"
#include "stdio.h"
#include "boot.h"
#include "video.h"
#include "string.h"
#include <stddef.h>

// printk displays to video
int printk(const char *szFormat, ...) 
{  
    char szBuffer[512 * 2];
    unsigned int wLength=0;
    va_list argList;
    va_start(argList, szFormat);
    wLength=(unsigned int) vsprintf(szBuffer, szFormat, argList);

    va_end(argList);

    szBuffer[sizeof(szBuffer)-1]=0;

    if(wLength>(sizeof(szBuffer)-1))
    {
        wLength = sizeof(szBuffer)-1;
    }

    szBuffer[wLength]='\0';

    BootVideoChunkedPrint(szBuffer);
    return wLength;
}


unsigned int centerPrintK(int XPos, int YPos, const char *szFormat, ...)
{
#define SingleLineMaxLength 128
    char szBuffer[512 * 2], singleLine[SingleLineMaxLength];
    unsigned char singleLineLength;
    unsigned int wLength = 0, i = 0, startPos = 0;
    va_list argList;
    va_start(argList, szFormat);
    wLength=(unsigned int) vsprintf(szBuffer, szFormat, argList);
    va_end(argList);

    szBuffer[sizeof(szBuffer)-1]=0;

    if (wLength>(sizeof(szBuffer)-1))
    {
        wLength = sizeof(szBuffer)-1;
    }

    szBuffer[wLength]='\0';

    VIDEO_CURSOR_POSY=YPos;

    while(i <= wLength)
    {
        if((i - startPos) > 0)
        {
            if(szBuffer[i] == '\n' || szBuffer[i] == '\0')
            {
                singleLineLength = i - startPos >= (SingleLineMaxLength - 1) ? (SingleLineMaxLength - 2) : i - startPos + 1;
                strncpy(singleLine, szBuffer + startPos, singleLineLength);
                singleLine[singleLineLength + 1] = '\0';
                unsigned int offset = (BootVideoGetStringTotalWidth(singleLine)/2);
                VIDEO_CURSOR_POSX=(XPos - offset) * 4;
                BootVideoChunkedPrint(singleLine);
                startPos = i + 1;
            }
        }

        i++;
    }

    return VIDEO_CURSOR_POSY;
}

unsigned int centerScreenPrintk(int YPos,const char *szFormat, ...)
{
    unsigned int value;
    char temp[512*2];
    va_list args;
    va_start(args, szFormat);
    vsprintf(temp, szFormat, args);
    va_end(args);
    value = centerPrintK(vmode.width / 2, YPos, "%s", temp);

    return value;
}
