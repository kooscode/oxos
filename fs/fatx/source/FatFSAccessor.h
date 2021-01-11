/*
 * PartTable.h
 *
 *  Created on: Mar 27, 2018
 *      Author: cromwelldev
 */

#ifndef FS_FF12B_SRC_FATFSACCESSOR_H_
#define FS_FF12B_SRC_FATFSACCESSOR_H_

#define NbDrivesSupported 2
#define HDD_Master  0
#define HDD_Slave   0

#define NbFATXPartPerHDD 7
#define Part_C    1
#define Part_E    0
#define Part_F    5
#define Part_G    6
#define Part_X    2
#define Part_Y    3
#define Part_Z    4

#define cPathSep   '/'
#define PathSep    "/"

extern const char* const * const  PartitionNameStrings[NbDrivesSupported];

typedef enum
{
    XboxDiskLayout_Base,
    XboxDiskLayout_FOnly,
    XboxDiskLayout_F120GRest,
    XboxDiskLayout_FGSplit,
    XboxDiskLayout_FMaxGRest
}XboxDiskLayout;

typedef int FILEX;

#include "ff.h"

typedef enum
{
    FileOpenMode_Read = FA_READ,
    FileOpenMode_Write = FA_WRITE,
    FileOpenMode_OpenExistingOnly = FA_OPEN_EXISTING,
    FileOpenMode_CreateNewOnly = FA_CREATE_NEW,
    FileOpenMode_CreateAlways = FA_CREATE_ALWAYS,
    FileOpenMode_OpenAlways = FA_OPEN_ALWAYS,
    FileOpenMode_OpenAppend = FA_OPEN_APPEND
}FileOpenMode;

typedef enum
{
    FileAttr_ReadOnly = AM_RDO,
    FileAttr_Hidden = AM_HID,
    FileAttr_SysFile = AM_SYS,
    FileAttr_Directory = AM_DIR,
    FileAttr_Archive =  AM_ARC
}FileAttr;

typedef int DIREX;

typedef struct
{
    char name[42];
    unsigned char nameLength;
    unsigned int size;
    unsigned char attributes;
    unsigned short modDate;
    unsigned short modTime;
}FileInfo;

typedef struct
{
    unsigned char seconds;
    unsigned char minutes;
    unsigned char hours;
    unsigned char mday;
    unsigned char month;
    unsigned short year;
}FileDate;


void FatFS_init(void);
int isFATXFormattedDrive(unsigned char driveNumber);
int mountAll(unsigned char driveNumber);
int fatxmount(unsigned char driveNumber, unsigned char partitionNumber);
int isMounted(unsigned char driveNumber, unsigned char partitionNumber);
int fdisk(unsigned char driveNumber, XboxDiskLayout xboxDiskLayout);
int fatxmkfs(unsigned char driveNumber, unsigned char partNumber);

int getActivePartName(unsigned char index, const char * * out);

FILEX fatxopen(const char* path, FileOpenMode mode);
int fatxclose(FILEX handle);
int fatxread(FILEX handle, unsigned char* out, unsigned int size);
int fatxwrite(FILEX handle, const unsigned char* in, unsigned int size);
int fatxseek(FILEX handle, unsigned int offset);
int fatxsync(FILEX handle);
FileInfo fatxstat(const char* path);

int fatxmkdir(const char* path);
DIREX fatxopendir(const char* path);
FileInfo fatxreaddir(DIREX handle);
DIREX fatxfindfirst(FileInfo* out, const char* path, const char* pattern);
int fatxfindnext(DIREX handle, FileInfo* out);
int fatxrewinddir(DIREX handle);
int fatxclosedir(DIREX handle);

int fatxdelete(const char* path);
int fatxrename(const char* path, const char* newName);

int fatxchdir(const char* path);
int fatxchdrive(const char* path);

int fatxgetfree(const char* path);
int fatxgetclustersize(unsigned char driveNumber, unsigned char partNumber);

int fatxputc(FILEX handle, char c);
int fatxputs(const char* sz, FILEX handle);
int fatxprintf(FILEX handle, const char* sz, ...);
const char* fatxgets(char* out, unsigned int len, FILEX handle);
long long int fatxtell(FILEX handle);
int fatxeof(FILEX handle);
long long int fatxsize(FILEX handle);

FileDate convertToTime(unsigned short date, unsigned short time);

#endif /* FS_FF12B_SRC_FATFSACCESSOR_H_ */
