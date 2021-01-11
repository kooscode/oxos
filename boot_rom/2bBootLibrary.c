
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************
*/
// 20040924 - Updated by dmp to include more str functions, and use ASM
// where possible. ASM shamelessly stolen from linux-2.6.8.1

#include <stddef.h>
#include <stdint.h>

void *memcpy (void *__restrict __dest, const void *__restrict __src, size_t __n)
{
    int d0, d1, d2;
    __asm__ __volatile__(
               "rep ; movsl\n\t"
               "testb $2,%b4\n\t"
              "je 1f\n\t"
               "movsw\n"
              "1:\ttestb $1,%b4\n\t"
             "je 2f\n\t"
             "movsb\n"
               "2:"
             : "=&c" (d0), "=&D" (d1), "=&S" (d2)
        :"0" (__n/4), "q" (__n),"1" ((long) __dest),"2" ((long) __src)
               : "memory");
    return (__dest);
}

void *memset (void *__s, int __c, size_t __n)
{
      int d0, d1;
    __asm__ __volatile__(
            "rep\n\t"
            "stosb"
            : "=&c" (d0), "=&D" (d1)
            :"a" (__c),"1" (__s),"0" (__n)
            :"memory");
    return __s;
}
                

int memcmp(const void * cs,const void * ct,size_t count)
{
        const unsigned char *su1, *su2;
        int res = 0;

    for( su1 = cs, su2 = ct; 0 < count; ++su1, ++su2, count--)
        if ((res = *su1 - *su2) != 0) break;
    return res;
}

