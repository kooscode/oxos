/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

// 2002-09-10  agreen@warmcat.com  created

// These are helper functions for displaying bitmap video
// includes an antialiased (4bpp) proportional bitmap font (n x 16 pixel)

int sprintf(char * buf, const char *fmt, ...);

#include "boot.h"
#include "video.h"
#include "memory_layout.h"
#include "fontx16.h"  // brings in font struct
#include "cromwell.h"
#include "string.h"
#include "nanojpeg.h"

#include <stdarg.h>

#define WIDTH_SPACE_PIXELS 5
//TODO: define
#define UndefinedHorizontalOffsetInPixels 0

extern const void* _start_backdrop;
extern const void* _end_backdrop;

unsigned int BootVideoGetCharacterWidth(unsigned char bCharacter, bool fDouble)
{
    unsigned int nStart, nWidth;
    int nSpace=WIDTH_SPACE_PIXELS;
    
    if(fDouble) nSpace=8;

    if(bCharacter == '\n') return 0;
    // we only have glyphs for 0x21 through 0x7e inclusive
    if(bCharacter<0x21) return nSpace;
    if(bCharacter>0x7e) return nSpace;

    nStart=waStarts[bCharacter-0x21];
    nWidth=waStarts[bCharacter-0x20]-nStart;

    if(fDouble) return nWidth<<1; else return nWidth;
}

// returns number of x pixels taken up by string

unsigned int BootVideoGetStringTotalWidth(const char * szc) {
    unsigned int nWidth=0;
    bool fDouble=false;
    while(*szc) {
        if(*szc=='\2') {
            fDouble=!fDouble;
            szc++;
        } else {
            nWidth+=BootVideoGetCharacterWidth(*szc++, fDouble);
        }
    }
    return nWidth;
}

// convert pixel count to size of memory in bytes required to hold it, given the character height

unsigned int BootVideoFontWidthToBitmapBytecount(unsigned int uiWidth)
{
    return (uiWidth << 2) * uiPixelsY;
}

void BootVideoJpegBlitBlend(
    unsigned char *pDst,
    unsigned int dst_width,
    JPEG * pJpeg,
    unsigned char *pFront,
    RGBA m_rgbaTransparent,
    unsigned char *pBack,
    int x,
    int y
) {
    int n=0;

    int nTransAsByte=m_rgbaTransparent>>24;
    int nBackTransAsByte=255-nTransAsByte;
    unsigned int dw;

    m_rgbaTransparent|=0xff000000;
    m_rgbaTransparent&=0xff80c080;      //Loosen tolerance from 0xffc0c0c0

    while(y--) {

        for(n=0;n<x;n++) {
            
            dw = ((*((unsigned int *)pFront))|0xff000000)&0xff80c080;      //Loosen tolerance from 0xffc0c0c0

            if(dw!=m_rgbaTransparent) {
                pDst[2]=((pFront[0]*nTransAsByte)+(pBack[0]*nBackTransAsByte))>>8;
                pDst[1]=((pFront[1]*nTransAsByte)+(pBack[1]*nBackTransAsByte))>>8;
                pDst[0]=((pFront[2]*nTransAsByte)+(pBack[2]*nBackTransAsByte))>>8;
            }
            pDst+=4;
            pFront+=3;
            pBack+=3;
        }
        pBack+=(pJpeg->iconCount*ICON_WIDTH*3) -(x * 3);
        pDst+=(dst_width * 4) - (x * 4);
        pFront+=(pJpeg->iconCount*ICON_WIDTH*3) -(x * 3);
    }
}

// usable for direct write or for prebuffered write
// returns width of character in pixels
// RGBA .. full-on RED is opaque --> 0xFF0000FF <-- red

int BootVideoOverlayCharacter(
    unsigned int * pdwaTopLeftDestination,
    unsigned int m_dwCountBytesPerLineDestination,
    RGBA rgbaColourAndOpaqueness,
    unsigned char bCharacter,
    bool fDouble
)
{
    int nSpace;
    unsigned int n, nStart, nWidth, y, nHeight
//        nOpaquenessMultiplied,
//        nTransparentnessMultiplied
    ;
    unsigned char b=0, b1; // *pbColour=(unsigned char *)&rgbaColourAndOpaqueness;
    unsigned char * pbaDestStart;

        // we only have glyphs for 0x21 through 0x7e inclusive

    if(bCharacter=='\t')
    {
        unsigned int dw=((unsigned int)pdwaTopLeftDestination) % m_dwCountBytesPerLineDestination;
        unsigned int dw1=((dw+1)%(32<<2));  // distance from previous boundary
        return ((32<<2)-dw1)>>2;
    }
    nSpace=WIDTH_SPACE_PIXELS;
    if(fDouble)
    {
        nSpace=8;
    }
    if(bCharacter<'!')
    {
        return nSpace;
    }
    if(bCharacter>'~')
    {
        return nSpace;
    }

    nStart=waStarts[bCharacter-(' '+1)];
    nWidth=waStarts[bCharacter-' ']-nStart;
    nHeight=uiPixelsY;

    if(fDouble)
    {
        nWidth<<=1;
        nHeight<<=1;
    }

    pbaDestStart=((unsigned char *)pdwaTopLeftDestination);

    for(y=0;y<nHeight;y++)
    {
        unsigned char * pbaDest=pbaDestStart;
        int n1=nStart;

        for(n=0;n<nWidth;n++)
        {
            b=baCharset[n1>>1];
            if(!(n1&1))
            {
                b1=b>>4;
            }
            else
            {
                b1=b&0x0f;
            }

            if(fDouble)
            {
                if(n & 1) n1++;
            }
            else
            {
                n1++;
            }

            if(b1)
            {
                    *pbaDest=(unsigned char)((b1*(rgbaColourAndOpaqueness&0xff))>>4); pbaDest++;
                *pbaDest=(unsigned char)((b1*((rgbaColourAndOpaqueness>>8)&0xff))>>4); pbaDest++;
                *pbaDest=(unsigned char)((b1*((rgbaColourAndOpaqueness>>16)&0xff))>>4); pbaDest++;
                *pbaDest++=0xff;
            }
            else
            {
                pbaDest+=4;
            }
        }
        if(fDouble)
        {
            if(y&1) nStart+=uiPixelsX;
        }
        else
        {
            nStart+=uiPixelsX;
        }
        pbaDestStart+=m_dwCountBytesPerLineDestination;
    }

    return nWidth;
}

// usable for direct write or for prebuffered write
// returns width of string in pixels
int BootVideoOverlayString(unsigned int * pdwaTopLeftDestination, unsigned int m_dwCountBytesPerLineDestination, RGBA rgbaOpaqueness, const char * szString)
{
    unsigned int uiWidth=0;
    bool fDouble=0;
    while((*szString != 0) && (*szString != '\n'))
    {
        if(*szString=='\2')
        {
            fDouble=!fDouble;
        }
        else
        {
            uiWidth+=BootVideoOverlayCharacter(
                pdwaTopLeftDestination+uiWidth, m_dwCountBytesPerLineDestination, rgbaOpaqueness, *szString, fDouble
                );
        }
        szString++;
    }
    return uiWidth;
}

bool BootVideoInitJPEGBackdropBuffer(JPEG * pJpeg)
{
    int i;

    //Decode embedded JPG
    unsigned char *jpg_buff;
    int embed_size = _end_backdrop - _start_backdrop;
    njInit();
    if (njDecode(&_start_backdrop, embed_size) != NJ_OK)
    {

  // TODO = Move err display to somwhere common
        int curcolor =  VIDEO_ATTR;
        VIDEO_ATTR = 0xffff0000;
        printk("Error decode picture\n");
        VIDEO_ATTR = curcolor;
        return false;
    }

    //decoded jpeg and image details
    unsigned char* jpg_data = njGetImage();
    int jpg_size = njGetImageSize();
    pJpeg->width = njGetWidth();
    pJpeg->height = njGetHeight();
    pJpeg->channels = njIsColor() ? 3 : 1;

    if(pJpeg->pData != NULL)
      free(pJpeg->pData);

    // init memory for backdrop
    pJpeg->pData = malloc(jpg_size);

    //check alloc was ok..
    if(pJpeg->pData == NULL)
      return false;

    //copy to backdrop buffer
    memcpy(pJpeg->pData,jpg_data,jpg_size);

    pJpeg->pBackdrop = pJpeg->pData + pJpeg->width * pJpeg->channels *  ICON_HEIGHT;
    
    return true;    
}


void BootVideoClearScreen(JPEG *pJpeg, int nStartLine, int nEndLine)
{
	VIDEO_CURSOR_POSX=vmode.xmargin;
	VIDEO_CURSOR_POSY=vmode.ymargin;

	if(nEndLine>=vmode.height) nEndLine=vmode.height-1;

	{
		if(pJpeg->pData!=NULL) 
    {
			volatile unsigned int *pdw=((unsigned int*)FB_START)+vmode.width*nStartLine;
			int n1=pJpeg->channels * pJpeg->width * nStartLine;
			unsigned char*pbJpegBitmapAdjustedDatum=pJpeg->pBackdrop;

			while(nStartLine++<nEndLine) {
				int n;
				for(n=0;n<vmode.width;n++) 
        {
					pdw[n]=0xff000000|
						((pbJpegBitmapAdjustedDatum[n1+2]))|
						((pbJpegBitmapAdjustedDatum[n1+1])<<8)|
						((pbJpegBitmapAdjustedDatum[n1])<<16);

					n1+=pJpeg->channels;
				}

				n1 += pJpeg->channels * (pJpeg->width - vmode.width);
				pdw += vmode.width; // adding u32 footprints
			}
		}
	}
}

int VideoDumpAddressAndData(unsigned int dwAds, const unsigned char * baData, unsigned int dwCountBytesUsable) { // returns bytes used
    int nCountUsed=0;
    while(dwCountBytesUsable)
    {
        unsigned int dw=(dwAds & 0xfffffff0);
        char szAscii[17];
        char sz[256];
        int n=sprintf(sz, "%08X: ", dw);
        int nBytes=0;

        szAscii[16]='\0';
        while(nBytes<16)
        {
            if((dw<dwAds) || (dwCountBytesUsable==0))
            {
                n+=sprintf(&sz[n], "   ");
                szAscii[nBytes]=' ';
            }
            else
            {
                unsigned char b=*baData++;
                n+=sprintf(&sz[n], "%02X ", b);
                if((b<32) || (b>126)) szAscii[nBytes]='.'; else szAscii[nBytes]=b;
                nCountUsed++;
                dwCountBytesUsable--;
            }
            nBytes++;
            if(nBytes==8) n+=sprintf(&sz[n], ": ");
            dw++;
        }
        n+=sprintf(&sz[n], "   ");
        n+=sprintf(&sz[n], "%s", szAscii);
        sz[n++]='\n';
        sz[n++]='\0';

        printk(sz, n);

        dwAds=dw;
    }
    return 1;
}
void BootVideoChunkedPrint(const char * szBuffer)
{
    int n=0;
    int nDone=0;
    unsigned char fDouble = 0;

    while (szBuffer[n] != 0)
    {
        if(szBuffer[n] == '\2')
        {
            fDouble = 1;
        }

        if(szBuffer[n]=='\n')
        {
            BootVideoOverlayString(
                (unsigned int *)((FB_START) + VIDEO_CURSOR_POSY * (vmode.width*4) + VIDEO_CURSOR_POSX),
                vmode.width*4, VIDEO_ATTR, &szBuffer[nDone]
            );
            nDone=n+1;
            VIDEO_CURSOR_POSY += (16 << fDouble);
            fDouble = 0;
            VIDEO_CURSOR_POSX = (vmode.xmargin << 2) + UndefinedHorizontalOffsetInPixels;
        }
        n++;
    }
    if (n != nDone)
    {
        VIDEO_CURSOR_POSX += BootVideoOverlayString(
                                (unsigned int *)((FB_START) + VIDEO_CURSOR_POSY * (vmode.width*4) + VIDEO_CURSOR_POSX),
                                vmode.width*4, VIDEO_ATTR, &szBuffer[nDone]) << 2;

        if (VIDEO_CURSOR_POSX > (vmode.width - vmode.xmargin) << 2)
        {
            VIDEO_CURSOR_POSY += (16 << fDouble);
            VIDEO_CURSOR_POSX = (vmode.xmargin << 2) + UndefinedHorizontalOffsetInPixels;
        }
    }
}

int console_putchar(int c)
{
    char buf[2];
    buf[0] = (char)c;
    buf[1] = 0;
    BootVideoChunkedPrint(buf);
    return (int)buf[0];    
}

//Fix for BSD
#ifdef putchar
#undef putchar
#endif
int putchar(int c)
{
    return console_putchar(c);
}
