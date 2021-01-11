/* MD5.H - header file for MD5C.C
 */
/* MD5 context. */

#include <stdint.h>

/* POINTER defines a generic pointer type */
typedef uint8_t *POINTER;

/* UINT2 defines a two byte word */
typedef uint16_t UINT2;

/* UINT4 defines a four byte word */
typedef uint32_t UINT4;

typedef struct {
  UINT4 state[4];                                   /* state (ABCD) */
  UINT4 count[2];        /* number of bits, modulo 2^64 (lsb first) */
  uint8_t buffer[64];                         /* input buffer */
}__attribute__ ((packed)) MD5_CTX;

void MD5Init(MD5_CTX *context);
void MD5Update(MD5_CTX *context, const unsigned char *input, unsigned int input_len);
void MD5Final(unsigned char digest[16], MD5_CTX *context);
/*

    MD5_CTX hashcontext;
    unsigned char digest[16];
    
    MD5Init(&hashcontext);
    MD5Update(&hashcontext, file, lenght);
    MD5Final(digest, &hashcontext);
*/
