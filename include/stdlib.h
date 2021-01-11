#ifndef stdlib_h
#define stdlib_h

#include <stddef.h>
#include <limits.h>


long strtol(const char *nptr, char **ptr, int base);
unsigned long strtoul(const char *nptr, char **ptr, int base);

void free(void *ptr);
void * malloc(size_t size);
void *calloc (size_t __nmemb, size_t __size);

#define RAND_MAX INT_MAX
int rand(void);

#endif /* #ifndef stdlib.h */
