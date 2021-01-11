/*
 * ctype.h
 *
 *  Created on: Apr 14, 2018
 *      Author: cromwelldev
 */

#ifndef INCLUDE_CTYPE_H_
#define INCLUDE_CTYPE_H_

/* haha, don't need ctype.c */
#define isprint(c)           in_range(c, 0x20, 0x7f)


#define in_range(c, lo, up)  ((unsigned char)c >= lo && (unsigned char)c <= up)

#define isdigit(c)           in_range(c, '0', '9')
#define isalpha(c) (((c) | 32) - 'a' + 0U <= 'z' - 'a' + 0U)
#define is_digit isdigit
#define isxdigit(c)          (isdigit(c) || in_range(c, 'a', 'f') || in_range(c, 'A', 'F'))
#define islower(c)           in_range(c, 'a', 'z')
#define isupper(c) ((c - 'A') < 26u)
#define isspace(c)           (c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v')

int tolower(int ch);
int toupper(int ch);

#endif /* INCLUDE_CTYPE_H_ */
