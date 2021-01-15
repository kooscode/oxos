/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "ConfirmDialog.h"
#include "boot.h"
#include "video.h"
#include "cromwell.h"
#include "string.h"
#include "include/BootLogger.h"
#include "lib/cromwell/cromSystem.h"

#define ComparebufSize 100
char bypassConfirmDialog[ComparebufSize];        //Arbitrary length

bool ConfirmDialog(const char * string, bool critical)
{
    unsigned int yPos = 120;
    unsigned int stringLength = strlen(string);
    bool result = true;         //True = cancel.
        
    stringLength = stringLength > ComparebufSize ? ComparebufSize : stringLength;
    if(strncmp(string, bypassConfirmDialog, stringLength) == false && critical == false)
    {
        return false;
    }

    BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
    VIDEO_ARGB=0xffff1515;                        //Red characters.
    yPos = centerScreenPrintk(yPos, "\2%s\n", string);
    VIDEO_ARGB=0xffffff;                //Back to white
    yPos = centerScreenPrintk(yPos, "Hold RT, LT, Start and White to confirm\n");
    centerScreenPrintk(yPos, "Press Back to cancel");
    
    while(cromwellLoop())
    {
        if(risefall_xpad_BUTTON(TRIGGER_XPAD_TRIGGER_RIGHT) &&
           risefall_xpad_BUTTON(TRIGGER_XPAD_TRIGGER_LEFT) &&
           risefall_xpad_STATE(XPAD_STATE_START)
           && XPAD_current[0].keys[5]) //white button
        {
            BootLogger(DEBUG_GENERAL_UI, DBG_LVL_DEBUG, "All correct buttons pressed. Accepting.");
            strncpy(bypassConfirmDialog, string, stringLength);
            result = false;
            break;
        }

        if(risefall_xpad_STATE(XPAD_STATE_BACK) == 1)
        {
            break;
        }
    }
    BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
    
    return result;
}
