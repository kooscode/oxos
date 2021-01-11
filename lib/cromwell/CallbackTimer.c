/*
 * CallbackTimer.c
 *
 *  Created on: May 7, 2018
 *      Author: cromwelldev
 */

#include "CallbackTimer.h"
#include "lib/time/timeManagement.h"
#include "stdlib.h"
#include "include/BootLogger.h"
#include <stddef.h>

typedef struct TimerInstance_t
{
    unsigned int id;
    unsigned int lastExec_ms;
    unsigned short interval_ms;
    unsigned char singleRun;
    unsigned char markForDeletion;
    callbackTimerHandler handler;
    struct TimerInstance_t* nextTimer;
}TimerInstance_t;

static TimerInstance_t* firstInstance;
static unsigned char initDone = 0;

void callbackTimer_init()
{
    firstInstance = NULL;
    initDone = 1;
}

void callbackTimer_execute()
{
    TimerInstance_t* currentTimer = firstInstance;
    TimerInstance_t* previousTimer = firstInstance;

    while(initDone && NULL != currentTimer)
    {
        if(getElapsedTimeSince(currentTimer->lastExec_ms) >= currentTimer->interval_ms)
        {
            BootLogger(DEBUG_CALLBACKTIMER, DBG_LVL_DEBUG | DBG_FLG_SPI, "execute handler id:%u", currentTimer->id);
            (*currentTimer->handler)();
            currentTimer->lastExec_ms = getMS();
            if(currentTimer->singleRun)
            {
                BootLogger(DEBUG_CALLBACKTIMER, DBG_LVL_DEBUG, "Single use timer id:%u", currentTimer->id);
                currentTimer->markForDeletion = 1;
            }
        }

        currentTimer = currentTimer->nextTimer;
    }

    currentTimer = firstInstance;
    while(NULL != currentTimer)
    {
        if(currentTimer->markForDeletion)
        {
            stopCallbackTimer(currentTimer->id);
            currentTimer = previousTimer;
        }
        else
        {
            previousTimer = currentTimer;
            currentTimer = currentTimer->nextTimer;
        }
    }
}

int newCallbackTimer(callbackTimerHandler handler, int interval_ms, unsigned char singleUseTimer)
{
    TimerInstance_t* lastTimer = firstInstance;
    TimerInstance_t* newTimer;
    int timerId = 0;

    if(0 == initDone)
    {
        return -1;
    }

    newTimer = calloc(1, sizeof(TimerInstance_t));
    if(NULL == newTimer)
    {
        return -1;
    }

    if(NULL != lastTimer)
    {
        while(NULL != lastTimer->nextTimer)
        {
            lastTimer = lastTimer->nextTimer;
        }
        timerId = lastTimer->id + 1;
        if(0 == timerId)
        {
            BootLogger(DEBUG_CALLBACKTIMER, DBG_LVL_FATAL, "Ran out of timerId. Rollover.");
        }
        lastTimer->nextTimer = newTimer;
    }
    else
    {
        firstInstance = newTimer;
    }


    BootLogger(DEBUG_CALLBACKTIMER, DBG_LVL_INFO, "Adding callback timer id:%u", timerId);

    newTimer->handler = handler;
    newTimer->id = timerId;
    newTimer->interval_ms = interval_ms;
    newTimer->lastExec_ms = getMS();
    newTimer->singleRun = singleUseTimer ? 1 : 0;

    return 0;
}

void stopCallbackTimer(int id)
{
    TimerInstance_t* targetTimer = firstInstance;
    TimerInstance_t* previousTimer = firstInstance;

    while(initDone && NULL != targetTimer)
    {
        if(targetTimer->id == id)
        {
            if(targetTimer == firstInstance)
            {
                firstInstance = firstInstance->nextTimer;
            }
            else
            {
                previousTimer->nextTimer = targetTimer->nextTimer;
            }
            BootLogger(DEBUG_CALLBACKTIMER, DBG_LVL_INFO, "Removing callback timer id:%u", targetTimer->id);
            free(targetTimer);
            return;
        }

        previousTimer = targetTimer;
        targetTimer = targetTimer->nextTimer;
    }

    BootLogger(DEBUG_CALLBACKTIMER, DBG_LVL_WARN, "Timer id:%u not found", id);
}
