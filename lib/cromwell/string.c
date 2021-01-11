/*
 * string.c
 *
 *  Created on: Aug 8, 2016
 *      Author: bennyboy
 */

#include "include/string.h"

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

char * strcpy(char * dest,const char *src)
{
    int d0, d1, d2;
    __asm__ __volatile__(
               "1:\tlodsb\n\t"
            "stosb\n\t"
               "testb %%al,%%al\n\t"
               "jne 1b"
            : "=&S" (d0), "=&D" (d1), "=&a" (d2)
             :"0" (src),"1" (dest) : "memory");
    return dest;
}

char * strncpy(char * dest,const char *src,int count)
{
    int d0, d1, d2, d3;
    __asm__ __volatile__(
            "1:\tdecl %2\n\t"
            "js 2f\n\t"
            "lodsb\n\t"
               "stosb\n\t"
              "testb %%al,%%al\n\t"
            "jne 1b\n\t"
              "rep\n\t"
           "stosb\n"
         "2:"
              : "=&S" (d0), "=&D" (d1), "=&c" (d2), "=&a" (d3)
        :"0" (src),"1" (dest),"2" (count) : "memory");
    return dest;
}

char * strstr(const char * s1,const char * s2)
{
        int l1, l2;

    l2 = strlen(s2);
    if (!l2) return (char *) s1;
        l1 = strlen(s1);
    while (l1 >= l2) {
        l1--;
        if (!memcmp(s1,s2,l2)) return (char *) s1;
        s1++;
    }
    return NULL;
}

char * strpbrk(const char * cs,const char * ct)
{
        const char *sc1,*sc2;

        for( sc1 = cs; *sc1 != '\0'; ++sc1) {
        for( sc2 = ct; *sc2 != '\0'; ++sc2) {
            if (*sc1 == *sc2) return (char *) sc1;
        }
    }
    return NULL;
}

char * strsep(char **s, const char *ct)
{
        char *sbegin = *s, *end;

           if (sbegin == NULL) return NULL;

    end = strpbrk(sbegin, ct);
    if (end) *end++ = '\0';
    *s = end;
    return sbegin;
}

int strncmp(const char * cs,const char * ct,size_t count)
{
    register int __res;
    int d0, d1, d2;
    __asm__ __volatile__(
        "1:\tdecl %3\n\t"
        "js 2f\n\t"
        "lodsb\n\t"
        "scasb\n\t"
        "jne 3f\n\t"
        "testb %%al,%%al\n\t"
        "jne 1b\n"
        "2:\txorl %%eax,%%eax\n\t"
        "jmp 4f\n"
        "3:\tsbbl %%eax,%%eax\n\t"
        "orb $1,%%al\n"
        "4:"
            :"=a" (__res), "=&S" (d0), "=&D" (d1), "=&c" (d2)
            :"1" (cs),"2" (ct),"3" (count));
    return __res;
}

int atoi(const char *str)
{
    int i, res = 0; // Initialize result

    // Iterate through all characters of input string and update result
    for (i = 0; str[i] >= '0' && str[i] <= '9'; ++i)
        res = res*10 + str[i] - '0';

    // return result.
    return res;
}

int strcmp(const char* s1,const char* s2)
{
   while (*s1 == *s2++)
       if (*s1++ == 0)
           return (0);
   return (*(const unsigned char *)s1 - *(const unsigned char *)(s2 - 1));
}

void chrreplace(char *string, char search, char ch) {
    char *ptr = string;
    while(*ptr != 0) {
        if(*ptr == search) {
            *ptr = ch;
        } else {
            ptr++;
        }
    }
}

/* Shamelessly copied from linux-2.6.15.1/lib/string.c */
void *memmove(void *dest, const void *src, size_t count)
{
    char *tmp;
    const char *s;

    if (dest <= src) {
        tmp = dest;
        s = src;
        while (count--)
            *tmp++ = *s++;
    } else {
        tmp = dest;
        tmp += count;
        s = src;
        s += count;
        while (count--)
            *--tmp = *--s;
    }
    return dest;
}

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

size_t strlen (const char *__s)
{
    int d0;
    register int __res;
    __asm__ __volatile__(
            "repne\n\t"
               "scasb\n\t"
            "notl %0\n\t"
               "decl %0"
        :"=c" (__res), "=&D" (d0) :"1" (__s),"a" (0), "0" (0xffffffffu));
    return __res;
}

char *strchr(const char *s, int c)
{
    while (*s != (char)c)
        if (!*s++)
            return 0;
    return (char *)s;
}
