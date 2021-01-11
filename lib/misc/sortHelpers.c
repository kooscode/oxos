/*
 * sortHelpers.c
 *
 *  Created on: Sep 15, 2016
 *      Author: bennyboy
 */

#include "stdlib.h"
#include "sortHelpers.h"

/*Function taken from : http://www.acnenomor.com/2430019p1/natural-sort-in-c-array-of-strings-containing-numbers-and-letters */
/* like strcmp but compare sequences of digits numerically */
/* Return +int if s1 should come after s2. -int if s2 should come after s1.*/
/* Return 0 if equals, shouldn't happen since you can't have 2 files with the same name!*/
int strcmpbynum(const char *s1, const char *s2)
{
    const char to_caps = 32;
    const char symbol_adjust = 49;
    int temps1, temps2;
    char *lim1, *lim2;
    unsigned long n1;
    unsigned long n2;
    for (;;) {
        if (*s2 == '\0')                //End of string for s2
            return *s1 != '\0';         //return 1 if s1 is longer.
        else if (*s1 == '\0')           //It's not the end of s2 but it is for s1
            return -1;                   //They are different and s1 is shorter.
        else if (!((*s1 >= '0' && *s1 <= '9') && (*s2 >= '0' && *s2 <= '9'))){  //If one of the 2 characters is not a ascii number
            if(*s1 >= 'a' && *s1 <= 'z')
		temps1 = *s1 - to_caps;
            else if(*s1 >= '[' && *s1 <= '`')
                temps1 = *s1 - symbol_adjust;
            else
                temps1 = *s1;

            if(*s2 >= 'a' && *s2 <= 'z')
		temps2 = *s2 - to_caps;
            else if(*s2 >= '[' && *s2 <= '`')
                temps2 = *s2 - symbol_adjust;
            else
                temps2 = *s2;

            if (temps1 != temps2)             //If both characters aren't the same
                return temps1 - temps2;     //Return difference between s1 and s2, asci-wise
            else
                (++s1, ++s2);           //If they are equals, move on to the next set of numbers/characters
        }                               //Go back to start of forever loop

        else {                          //both characters are numbers
            n1 = strtoul(s1, &lim1, 10);//Convert partial string to number
            n2 = strtoul(s2, &lim2, 10);//Will return 0 if no numerical string can be converted.
                                        //This is useful to skip letters during string comparison.
            if (n1 > n2)                //if s1 contains a bigger value
                return 1;
            else if (n1 < n2)           //Opposite
                return -1;
            s1 = lim1;                  //Move pointers after converter number values.
            s2 = lim2;
        }                               //Go back to start of forever loop
    }
    return 0;
}
