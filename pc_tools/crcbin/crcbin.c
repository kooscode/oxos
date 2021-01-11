/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned int crc32buf(unsigned char *buf, int len);
void showUsage(const char *progname);


int main (int argc, const char * argv[])
{
FILE *pFile;
unsigned char inBuffer[0x3F000];  //Length of data we wish to calculate CRC on.
unsigned char outBuffer[0x40000];  //Total size of output (262144 bytes)

unsigned int crcValue = 0xFFFFFFFF, nbytes;


    printf("CRC32 calculator. Embed the result in bin file at offset 0x3FDFC.\n");
    
    if( argc != 3 ) {
        showUsage(argv[0]);
        exit(1);
    }

    pFile = fopen(argv[1],"rb");
    if(pFile == NULL){
        printf("File open error.\n");
        exit(1);
    }
    fseek(pFile, 0, SEEK_END);
    if(ftell(pFile) != 262144){
        printf("File size error.\n");
        exit(1);
    }
    fseek(pFile, 0, SEEK_SET);
    fread(inBuffer,1,0x3F000,pFile);   //Copy data up to persistent settings location.
    fseek(pFile, 0, SEEK_SET);
    fread(outBuffer,1,0x40000,pFile);  //Copy input data for output file generation.
    fclose(pFile);

    crcValue = crc32buf(inBuffer, 0x3F000);     //from crc32.o

    //print CRC32 result value in output buffer.
    outBuffer[0x3FDFC] = crcValue & 0xFF;
    outBuffer[0x3FDFD] = (crcValue >> 8) & 0xFF;
    outBuffer[0x3FDFE] = (crcValue >> 16) & 0xFF;
    outBuffer[0x3FDFF] = (crcValue >> 24) & 0xFF;
    pFile = fopen(argv[2],"wb");
    if(pFile == NULL){
            printf("File creation error.\n");
            exit(1);
    }
    fwrite(outBuffer, 1, 0x40000, pFile);
    printf("Done.  CRC32 value is 0x%08X\n", crcValue);

    exit(0);
}

void showUsage(const char *progname) {
    printf("Usage:   ");
    printf("%s 'input file' 'output file'\n",progname);
}
