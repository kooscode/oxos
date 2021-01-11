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
    // START NETWORK DEAMONS HERE...

    callbackTimer_execute();
    
    return 1;
}
