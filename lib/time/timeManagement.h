/*
 * timeManagement.h
 *
 *  Created on: Aug 7, 2016
 *      Author: bennyboy
 *
 *  Handles timers and counters specifically related to the Xbox
 *  Currently, 2 know time sources are handled:
 *  -Programmable Interval Timer (PIT)
 *  -APIC Timer
 *
 *  PIT is an Intel 8253 compatible timer running at 1.193182 MHz
 *  It is currently set to count up indefinitely and generate
 *  an interrupt on overflow. As in standard x86 arch, it's wired
 *  to INT0 source on PIC1. Cromwell configures timer0's divider to
 *  a value of 1193 to generate an interrupt every ~1ms(1000.15Hz).
 *  Check function "BootPciPeripheralInitialization()".
 *  Value is obtain via global "BIOS_TICK_COUNT" which counts the
 *  number of interrupt since boot.
 *
 *  APIC Timer is standard 24/32 bit counter with a frequency of
 *  3.579545MHz. It does not generate an interrupt.
 *  Actual count value can be read with a DWord IO read at address
 *  0x8008.
 */

#ifndef LIB_TIME_TIMEMANAGEMENT_H_
#define LIB_TIME_TIMEMANAGEMENT_H_

void wait_ms_blocking(unsigned int ticks);
void wait_us_blocking(unsigned int ticks);

void wait_ms(unsigned int waitTime_ms);

void updateTime(void);

unsigned int getMS(void);
unsigned int getUS(void);
unsigned int getElapsedTimeSince(unsigned int startValue_ms);
unsigned int getElapseMicroSecondsSince(unsigned startValue_us);


#endif /* LIB_TIME_TIMEMANAGEMENT_H_ */
