#ifndef video_h
#define video_h

#include "stdlib.h"
#include <stdbool.h>

typedef unsigned long RGBA; // LSB=R -> MSB = A

// video helpers
typedef struct {
    unsigned char * pData;
    unsigned char *pBackdrop;
    unsigned char iconCount;
    int width;
    int height;
    int channels;
} JPEG;


int BootVideoOverlayString(unsigned int * pdwaTopLeftDestination, unsigned int m_dwCountBytesPerLineDestination, RGBA rgbaOpaqueness, const char * szString);
void BootVideoChunkedPrint(const char * szBuffer);
int VideoDumpAddressAndData(unsigned int dwAds, const unsigned char * baData, unsigned int dwCountBytesUsable);
unsigned int BootVideoGetStringTotalWidth(const char * szc);
void BootVideoClearScreen(JPEG * pJpeg, int nStartLine, int nEndLine);

void BootVideoJpegBlitBlend(
    unsigned char *pDst,
    unsigned int dst_width,
    JPEG * pJpeg,
    unsigned char *pFront,
    RGBA m_rgbaTransparent,
    unsigned char *pBack,
    int x,
    int y
);

bool BootVideoInitJPEGBackdropBuffer(JPEG * pJpeg);

// bool BootVideoJpegUnpackAsRgb(unsigned char *pbaJpegFileImage, JPEG * pJpeg, int size);

void BootVideoEnableOutput(unsigned char bAvPack);
unsigned char * BootVideoGetPointerToEffectiveJpegTopLeft(JPEG * pJpeg);
void BootVideoChunkedPrint(const char * szBuffer);

// Helpers
const char *VideoEncoderName(void);
const char *AvCableName(void);

extern unsigned char baBackdrop[60*72*4];
extern JPEG jpegBackdrop;

#endif /* #ifndef video_h */
