/*
 * system.c
 *
 *  Created on: Jan 30, 2017
 *      Author: cromwelldev
 */

#include "lib/time/timeManagement.h"
#include "CallbackTimer.h"

unsigned char cromwellLoop(void)
{
    // process network and other related backends here.

    callbackTimer_execute();
    
    return 1;
}
