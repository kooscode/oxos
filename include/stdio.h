#ifdef __PC_SIMULATOR__
#include </usr/include/stdio.h>
#else

#ifndef stdio_h
#define stdio_h

#include <stdarg.h>


int sprintf(char * buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list args);

#endif /* #ifndef stdio_h */
#endif
