/*
 * CallbackTimer.h
 *
 *  Created on: May 7, 2018
 *      Author: cromwelldev
 */

#ifndef LIB_CROMWELL_CALLBACKTIMER_H_
#define LIB_CROMWELL_CALLBACKTIMER_H_

#define IsSingleUseTimer 1
#define IsPerpetualTimer 0

typedef void (*callbackTimerHandler)(void);

void callbackTimer_init(void);
void callbackTimer_execute(void);
int newCallbackTimer(callbackTimerHandler handler, int interval_us, unsigned char singleUseTimer);
void stopCallbackTimer(int id);

#endif /* LIB_CROMWELL_CALLBACKTIMER_H_ */
