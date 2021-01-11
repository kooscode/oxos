/*
 * rand.c
 *
 *  Created on: Jan 11, 2017
 *      Author: cromwelldev
 */


#include "stdlib.h"

#define znew (z=36969*(z&65535)+(z>>16))
#define wnew (w=18000*(w&65535)+(w>>16))
#define MWC ((znew<<16)+wnew )
#define SHR3 (jsr^=(jsr<<17), jsr^=(jsr>>13), jsr^=(jsr<<5))
#define CONG (jcong=69069*jcong+1234567)
#define KISS ((MWC^CONG)+SHR3)
typedef unsigned long UL;
/* Global static variables: */
static UL z=362436069, w=521288629, jsr=123456789, jcong=380116160;


void seed_z(unsigned int in)
{
    w = (jsr + z) % w;
    z = in;
}

void seed_jsr(unsigned int in)
{
    jcong = (z * (jsr - w)) % w;
    jsr = in;
}

extern unsigned int getRandSeed(void);

int rand (void)
{
    int value;
    seed_jsr(getRandSeed());
    value = (KISS % RAND_MAX);
    seed_z(value % getRandSeed());

    return value;
}
