/*
 * PartitionTable.c
 *
 *  Created on: Mar 27, 2018
 *      Author: cromwelldev
 */

//#include "ff.h"
#include "FatFSAccessor.h"
#ifdef _PCSIM
#include <string.h>
#include <stdio.h>
#include "../../../pc_tools/fatfs_test/src/FatFSTestHelper.h"
#else
#include "string.h"
#include "stdio.h"
#include "BootIde.h"
#include "include/BootLogger.h"
#endif
#include <stdarg.h>
#include <limits.h>

/*---------------------------------------------------------------*/
/* Private static variables */
/*---------------------------------------------------------------*/
static FATFS FatXFs[NbDrivesSupported][NbFATXPartPerHDD];      /* File system object for logical drive */

#if _USE_FASTSEEK
static DWORD SeekTbl[16];          /* Link map table for fast seek feature */
#endif

#define MaxOpenFileCount (FF_FS_LOCK / 2)
static FIL FileHandleArray[MaxOpenFileCount];

#define MaxOpenDirCount (FF_FS_LOCK / 2)
static DIR DirectoryHandleArray[MaxOpenDirCount];

const char* VolumeStr[FF_VOLUMES] = { _VOLUME_STRS };   /* User defied volume ID */

/*---------------------------------------------------------------*/

const char* const * const PartitionNameStrings[NbDrivesSupported] =
{
 (const char* const*)&VolumeStr[0],
 (const char* const*)&VolumeStr[FF_VOLUMES / 2]
};

PARTITION VolToPart[FF_VOLUMES] =
{
 {0, 0}, /* XBOX SHELL, E:\ */
 {0, 1}, /* XBOX DATA, C:\ */
 {0, 2}, /* XBOX GAME SWAP 1, X:\ */
 {0, 3}, /* XBOX GAME SWAP 2, Y:\ */
 {0, 4}, /* XBOX GAME SWAP 3, Z:\ */
 {0, 5}, /* XBOX F, F:\ */
 {0, 6}, /* XBOX G, G:\ */
 {1, 0}, /* XBOX SHELL, E:\ */
 {1, 1}, /* XBOX DATA, C:\ */
 {1, 2}, /* XBOX GAME SWAP 1, X:\ */
 {1, 3}, /* XBOX GAME SWAP 2, Y:\ */
 {1, 4}, /* XBOX GAME SWAP 3, Z:\ */
 {1, 5}, /* XBOX F, F:\ */
 {1, 6}  /* XBOX G, G:\ */
};

#define FILE_VALID(x) if((MaxOpenFileCount <= x) || (0 == FileHandleArray[x].obj.fs)) return -1;
#define DIRE_VALID(x, y) memset(&y, 0x00, sizeof(FileInfo)); \
                        if((MaxOpenDirCount <= x) || (0 == DirectoryHandleArray[x].obj.fs)) return y;

#define FILE_HANDLE_VALID(x) if(0 == x || (MaxOpenFileCount < x)) return -1; x -= 1;
#define DIRE_HANDLE_VALID(x) if(0 == x || (MaxOpenDirCount < x)) return -1; x -= 1;

extern
int get_ldnumber (      /* Returns logical drive number (-1:invalid drive) */
    const TCHAR** path  /* Pointer to pointer to the path name */

);

static void convertToFileInfo(FileInfo* out, const FILINFO* in)
{
    out->nameLength = in->namelength;
    memcpy(out->name, in->fnamex, FATX_FILENAME_MAX < out->nameLength ? FATX_FILENAME_MAX : out->nameLength);
    out->attributes = in->fattrib;
    out->size = in->fsize;
    out->modDate= in->fdate;
    out->modTime = in->ftime;
}

void FatFS_init(void)
{
    unsigned char i;
    memset(FatXFs, 0x00, sizeof(FATFS) * NbFATXPartPerHDD * NbDrivesSupported);
#if _USE_FASTSEEK
    memset(SeekTbl, 0x00, sizeof(DWORD) * 16);
#endif
    memset(FileHandleArray, 0x00, sizeof(FIL) * MaxOpenFileCount);
    memset(DirectoryHandleArray, 0x00, sizeof(DIR) * MaxOpenDirCount);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "init internal FatFS.");
    fatx_init();

    for(i = 0; i < NbDrivesSupported; i++)
    {
        if(BootIdeDeviceConnected(i) && 0 == BootIdeDeviceIsATAPI(i))
        {
            mountAll(i);
        }
    }
}

int isFATXFormattedDrive(unsigned char driveNumber)
{
    return FR_OK == fatx_getbrfr(driveNumber) ? 1 : 0;
}

/* Will mount C, E, F, G, X, Y, Z if available*/
int mountAll(unsigned char driveNumber)
{
    unsigned char i;
    XboxPartitionTable tempTable;

    if(driveNumber >= NbDrivesSupported)
    {
        return -1;
    }
    BootLogger(DEBUG_FATX_FS, DBG_LVL_INFO, "Attempting to mount base partitions for drive %u", driveNumber);

    if(FR_OK == fatx_getbrfr(driveNumber))
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "got BRFR");
        if(FR_OK == fatx_getmbr(driveNumber, &tempTable))
        {
            BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "got mbr");
            //TODO: constant for number of standard partitions.
            for(i = 0; i < NbFATXPartPerHDD; i++)
            {
                BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Drive: %u, PartIndex: %u", driveNumber, i);
                if(0 == FatXFs[driveNumber][i].fs_typex)
                {
                    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "PartFlag: 0x%08X", tempTable.TableEntries[i].Flags);
                    if(tempTable.TableEntries[i].Flags & FATX_PE_PARTFLAGS_IN_USE)
                    {
                        fatxmount(driveNumber, i);
                    }
                }
            }
        }
        else
        {
            BootLogger(DEBUG_FATX_FS, DBG_LVL_WARN, "Error! No MBR.");
        }
    }
    else
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_WARN, "Error! No BRFR.");
    }

    return 0;
}

int fatxmount(unsigned char driveNumber, unsigned char partitionNumber)
{
    int result;
    char mountStr[20];

    sprintf(mountStr, PathSep"%s"PathSep, PartitionNameStrings[driveNumber][partitionNumber]);

    BootLogger(DEBUG_FATX_FS, DBG_LVL_INFO, "Mounting \"%s\" partition", PartitionNameStrings[driveNumber][partitionNumber]);
    //TODO: Constant for mount immediately flag.
    result = f_mount(&FatXFs[driveNumber][partitionNumber], mountStr, 1);
    if(FR_OK == result)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_INFO, "Mount \"%s\" partition success!", PartitionNameStrings[driveNumber][partitionNumber]);
    }
    else
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_ERROR, "Error! Mount \"%s\" partition. Code: %u!", PartitionNameStrings[driveNumber][partitionNumber], result);
    }

    return result;
}

int isMounted(unsigned char driveNumber, unsigned char partitionNumber)
{
    if(NbDrivesSupported <= driveNumber)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_ERROR, "Invalid drive number : %u", driveNumber);
        return -1;
    }

    if(NbFATXPartPerHDD <= partitionNumber)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_ERROR, "Invalid partition number : %u", partitionNumber);
        return -1;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Drive/Part %u/%u = %u", driveNumber, partitionNumber, FatXFs[driveNumber][partitionNumber].fs_typex);

    return FatXFs[driveNumber][partitionNumber].fs_typex;
}

int fdisk(unsigned char driveNumber, XboxDiskLayout xboxDiskLayout)
{
    XboxPartitionTable workingMbr;
    unsigned long long diskSizeLba;
    unsigned int fDriveLbaSize;
    //XXX: Assume 512Bytes sectors for now.

    if(NbDrivesSupported <= driveNumber)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_ERROR, "Error, out of bound disk: %u", driveNumber);
        return -1;
    }

    /* Get drive size */
    diskSizeLba = BootIdeGetSectorCount(driveNumber);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Disk LBA: %llu\n", diskSizeLba);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "fdisk selected layout: %u", xboxDiskLayout);

    /* If drive is too small even for stock partition scheme or stock partition scheme is not selected and drive size if smaller or equal than ~8GB*/
    if(((XBOX_EXTEND_STARTLBA - 1) > diskSizeLba) || (XBOX_EXTEND_STARTLBA >= diskSizeLba && XboxDiskLayout_Base != xboxDiskLayout))
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_ERROR, "Drive is not ok for selected layout");
        return -1;
    }

    /* If cannot get partition table (backup part table generated when none is found on drive) */
    if(fatx_getmbr(driveNumber, &workingMbr))
    {
        return -1;
    }

    switch(xboxDiskLayout)
    {
    case XboxDiskLayout_Base:
        memcpy(&workingMbr, &BackupPartTable, sizeof(XboxPartitionTable));
        break;
    case XboxDiskLayout_FOnly:
        if(diskSizeLba - XBOX_EXTEND_STARTLBA < FATX_MIN_PART_SIZE_LBA)
        {
            BootLogger(DEBUG_FATX_FS, DBG_LVL_INFO, "Disk too small for F volume");
            return -1;
        }

        workingMbr.TableEntries[5].LBAStart = XBOX_EXTEND_STARTLBA;
        workingMbr.TableEntries[5].LBASize = diskSizeLba - XBOX_EXTEND_STARTLBA;
        workingMbr.TableEntries[5].Flags = 0;
        /* Do not set Part in use flag here, in case mkfs doesn't go through. mkfs is supposed to set it. */
        workingMbr.TableEntries[6].LBAStart = 0;
        workingMbr.TableEntries[6].LBASize = 0;
        workingMbr.TableEntries[6].Flags = 0;
        break;
    case XboxDiskLayout_F120GRest:
        workingMbr.TableEntries[5].LBAStart = XBOX_EXTEND_STARTLBA;
        workingMbr.TableEntries[5].LBASize = LBASIZE_137GB;
        workingMbr.TableEntries[5].Flags = 0;
        if((LBA28_BOUNDARY + SYSTEM_LBASIZE) <= diskSizeLba)
        {
            /* Part must be at least 500MB */
            workingMbr.TableEntries[6].LBAStart = LBA28_BOUNDARY;
            workingMbr.TableEntries[6].LBASize = diskSizeLba - LBA28_BOUNDARY;
        }
        else
        {
            workingMbr.TableEntries[6].LBAStart = 0;
            workingMbr.TableEntries[6].LBASize = 0;
        }
        workingMbr.TableEntries[6].Flags = 0;
        break;
    case XboxDiskLayout_FGSplit:
        /* Calculate optimal FATX partition size for F drive depending on cluster size and volume size. */
        /* Get size of drive minus used space by stock partition scheme */
        diskSizeLba -= XBOX_EXTEND_STARTLBA;
        /* Round to 4096 bytes boundary */
        diskSizeLba &=  ~((unsigned long long)(FATX_CHAINTABLE_BLOCKSIZE / 8 - 1));
        fDriveLbaSize = diskSizeLba / 2;

        if(LBASIZE_1024GB <= fDriveLbaSize)
        {
            /* Max size for a FATX partition is 1TB */
            fDriveLbaSize = LBASIZE_1024GB;
        }
        else
        {
            /* Round to selected cluster size boundary */
            if(LBASIZE_1024GB >= fDriveLbaSize)
            {
                /* Check with 64KB clusters */
                fDriveLbaSize &= ~((unsigned long long)(FATX_MAX_CLUSTERSIZE_INSECTORS - 1));
            }

            if(LBASIZE_512GB >= fDriveLbaSize)
            {
                /* Check with 32KB clusters */
                fDriveLbaSize &= ~((unsigned long long)(FATX_MID_CLUSTERSIZE_INSECTORS - 1));
            }

            if(LBASIZE_256GB >= fDriveLbaSize)
            {
                /* Check with 16KB clusters */
                fDriveLbaSize &= ~((unsigned long long)(FATX_MIN_CLUSTERSIZE_INSECTORS - 1));
            }
        }
        workingMbr.TableEntries[5].LBAStart = XBOX_EXTEND_STARTLBA;
        workingMbr.TableEntries[5].LBASize = fDriveLbaSize;
        workingMbr.TableEntries[5].Flags = 0;
        workingMbr.TableEntries[6].LBAStart = XBOX_EXTEND_STARTLBA + fDriveLbaSize;
        workingMbr.TableEntries[6].LBASize = diskSizeLba - (XBOX_EXTEND_STARTLBA + fDriveLbaSize);
        workingMbr.TableEntries[6].Flags = 0;
        break;
    case XboxDiskLayout_FMaxGRest:
        workingMbr.TableEntries[5].LBAStart = XBOX_EXTEND_STARTLBA;
        workingMbr.TableEntries[5].LBASize = LBASIZE_1024GB;
        workingMbr.TableEntries[5].Flags = 0;
        if((XBOX_EXTEND_STARTLBA + LBASIZE_1024GB + SYSTEM_LBASIZE) <= diskSizeLba)
        {
            /* Part must be at least 500MB */
            workingMbr.TableEntries[6].LBAStart = XBOX_EXTEND_STARTLBA + LBASIZE_1024GB;
            workingMbr.TableEntries[6].LBASize = diskSizeLba - (XBOX_EXTEND_STARTLBA + LBASIZE_1024GB);
        }
        else
        {
            workingMbr.TableEntries[6].LBAStart = 0;
            workingMbr.TableEntries[6].LBASize = 0;
        }
        workingMbr.TableEntries[6].Flags = 0;
        break;
    default:
        return -1;
        break;
    }
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "F volume StartLBA: %u  sizeLBA%u", workingMbr.TableEntries[5].LBAStart, workingMbr.TableEntries[5].LBASize);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "G volume StartLBA: %u  sizeLBA%u", workingMbr.TableEntries[6].LBAStart, workingMbr.TableEntries[6].LBASize);

    return fatx_fdisk(driveNumber, &workingMbr);
}

int fatxmkfs(unsigned char driveNumber, unsigned char partNumber)
{
    XboxPartitionTable workingMbr;
    unsigned char workBuf[FATX_CHAINTABLE_BLOCKSIZE];
    char partName[22];
    sprintf(partName, PathSep"%s"PathSep, PartitionNameStrings[driveNumber][partNumber]);

    if(FR_OK != fatx_getmbr(driveNumber, &workingMbr))
    {
        return -1;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_INFO, "%s", partName);
    if(FR_OK != f_mkfs(partName, FM_FATXANY, FATX_MIN_CLUSTERSIZE_INSECTORS, workBuf, FATX_CHAINTABLE_BLOCKSIZE))
    {
        return -1;
    }

    return 0;
}
char text[] = "Toto";
int getActivePartName(unsigned char index, const char * *  out)
{
    while((NbDrivesSupported * NbFATXPartPerHDD) > index)
    {
        if(0 < isMounted(index / NbFATXPartPerHDD, index % NbFATXPartPerHDD))
        {
            BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "11111");

            *out = PartitionNameStrings[index / NbFATXPartPerHDD][index % NbFATXPartPerHDD];
            index++;
            return index;
        }
        index++;
    }

    return -1;
}


FILEX fatxopen(const char* path, FileOpenMode mode)
{
    unsigned char i;
    FRESULT result;

    /* Find unused File descriptor in array */
    for (i = 0; i < MaxOpenFileCount; i++)
    {
        if(0 == FileHandleArray[i].obj.fs)
        {
            BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "found unused handle slot %u", i + 1);
            break;
        }
    }

    if(MaxOpenFileCount == i)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_WARN, "no unused handle slot");
        return 0;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "%s, mode:%u", path, mode);
    result = f_open(&FileHandleArray[i], path, mode);
    if(FR_OK != result)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_WARN, "Open Fail...  result:%u", result);
        return 0;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Open Success!  Handle=%u", i + 1);
    return i + 1;
}

int fatxclose(FILEX handle)
{
    FRESULT result;
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Closing handle %u", handle);
    FILE_HANDLE_VALID(handle)

    if(0 != FileHandleArray[handle].obj.fs)
    {
        result = f_close(&FileHandleArray[handle]);
        BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "result:%u", result);
        return result;
    }
    BootLogger(DEBUG_FATX_FS, DBG_LVL_INFO, "Invalid Handle:%u", handle);
    return -1;
}

int fatxread(FILEX handle, unsigned char* out, unsigned int size)
{
    UINT bytesRead;
    FRESULT result;

    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "file %u, size:%u", handle, size);
    result = f_read(&FileHandleArray[handle], out, size, &bytesRead);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "result:%u, bytesRead:%u", result, bytesRead);
    if(FR_OK == result)
    {
        return bytesRead;
    }

    return -1;
}

int fatxwrite(FILEX handle, const unsigned char* in, unsigned int size)
{
    UINT bytesWrote;

    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "file %u, size:%u", handle, size);
    if(FR_OK == f_write(&FileHandleArray[handle], in, size, &bytesWrote))
    {
        return bytesWrote;
    }

    return -1;
}

int fatxseek(FILEX handle, unsigned int offset)
{
    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    if(FR_OK != f_lseek(&FileHandleArray[handle], offset))
    {
        return -1;
    }

    return 0;
}

int fatxsync(FILEX handle)
{
    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "file %u", handle);
    if(FR_OK != f_sync(&FileHandleArray[handle]))
    {
        return -1;
    }

    return 0;
}

FileInfo fatxstat(const char* path)
{
    FileInfo returnStruct;
    FILINFO getter;
    FRESULT result;
    memset(&returnStruct, 0x00, sizeof(FileInfo));
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "path:\"%s\"", path);
    result = f_stat(path, &getter);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "result:%u", result);
    if(FR_OK == result)
    {
        convertToFileInfo(&returnStruct, &getter);
    }
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "name:%s (%u)", returnStruct.name, returnStruct.nameLength);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "flags:%u size:%u", returnStruct.attributes, returnStruct.size);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "date:%u time:%u", returnStruct.modDate, returnStruct.modTime);

    return returnStruct;
}


int fatxmkdir(const char* path)
{
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "dir %s", path);
    if(FR_OK != f_mkdir(path))
    {
        return -1;
    }

    return 0;
}

DIREX fatxopendir(const char* path)
{
    FRESULT result;
    unsigned char i;

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Open dir:\"%s\"", path);

    /* Find unused Directory descriptor in array */
    for (i = 0; i < MaxOpenDirCount; i++)
    {
        if(0 == DirectoryHandleArray[i].obj.fs)
        {
            BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "found unused handle slot %u", i + 1);
            break;
        }
    }

    if(MaxOpenDirCount == i)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_WARN, "no unused handle slot");
        return 0;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "dir %s", path);
    result = f_opendir(&DirectoryHandleArray[i], path);
    if(FR_OK != result)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_WARN, "Open Fail...  result:%u", result);
        return 0;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Open Success!  Handle:%u", i + 1);
    return i + 1;
}

FileInfo fatxreaddir(DIREX handle)
{
    FileInfo returnStruct;
    FILINFO getter;

    if(0 == handle || (MaxOpenDirCount < handle))
    {
        /* Invalidate handle */
        handle = 255;
    }
    else
    {
        handle -= 1;
    }

    DIRE_VALID(handle, returnStruct)

    if(FR_OK == f_readdir(&DirectoryHandleArray[handle], &getter))
    {
        convertToFileInfo(&returnStruct, &getter);
        BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "dir %s(%u) size:%u attr:%u", returnStruct.name, returnStruct.nameLength, returnStruct.size, returnStruct.attributes);
    }

    return returnStruct;
}

DIREX fatxfindfirst(FileInfo* out, const char* path, const char* pattern)
{
    unsigned char i;
    FILINFO getter;
    memset(out, 0x00, sizeof(FileInfo));

    /* Find unused Directory descriptor in array */
    for (i = 0; i < MaxOpenDirCount; i++)
    {
        if(0 == DirectoryHandleArray[i].obj.fs)
        {
            break;
        }
    }

    if(MaxOpenDirCount == i)
    {
        return 0;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "file %s", path);
    if(FR_OK == f_findfirst(&DirectoryHandleArray[i], &getter, path, pattern))
    {
        convertToFileInfo(out, &getter);
        BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "dir %s(%u) size:%u attr:%u", out->name, out->nameLength, out->size, out->attributes);

    }

    return i + 1;
}

int fatxfindnext(DIREX handle, FileInfo* out)
{
    FILINFO getter;

    DIRE_HANDLE_VALID(handle)
    memset(out, 0x00, sizeof(FileInfo));

    if(0 == DirectoryHandleArray[handle].obj.fs)
    {
        return -1;
    }

    if(FR_OK == f_findnext(&DirectoryHandleArray[handle], &getter))
    {
        convertToFileInfo(out, &getter);
        BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "dir %s(%u) size:%u attr:%u", out->name, out->nameLength, out->size, out->attributes);

        return 0;
    }

    return -1;
}

int fatxrewinddir(DIREX handle)
{
    DIRE_HANDLE_VALID(handle)
    if(0 == DirectoryHandleArray[handle].obj.fs)
    {
        return -1;
    }

    if(FR_OK != f_rewinddir(&DirectoryHandleArray[handle]))
    {
        return -1;
    }

    return 0;
}

int fatxclosedir(DIREX handle)
{
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "Closing handle%u", handle);

    DIRE_HANDLE_VALID(handle)
    if(0 == DirectoryHandleArray[handle].obj.fs)
    {
        return -1;
    }

    if(FR_OK != f_closedir(&DirectoryHandleArray[handle]))
    {
        return -1;
    }

    return 0;
}


int fatxdelete(const char* path)
{
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "file %s", path);
    FRESULT result = f_unlink(path);
    if(FR_OK != result)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_ERROR, "Error!!!  result:%u", result);
        return -1;
    }

    return 0;
}

int fatxrename(const char* path, const char* newName)
{
    FRESULT result;
    const char *drive = (const char*)strchr(newName, ':');
    drive = (drive == NULL) ? newName : drive + 1;

    if(FATX_FILENAME_MAX < strlen(newName))
    {
        return -1;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "\"%s\" to \"%s\"", path, drive);
    result = f_rename(path, drive);
    if(FR_OK != result)
    {
        BootLogger(DEBUG_FATX_FS, DBG_LVL_WARN, "result:%u", result);
        return -1;
    }

    return 0;
}


int fatxchdir(const char* path)
{
    FRESULT result;

    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "path:\"%s\"", path);

    result = f_chdir(path);
    BootLogger(DEBUG_FATX_FS, DBG_LVL_DEBUG, "result:%u", result);
    if(FR_OK == result)
    {
        return 0;
    }

    BootLogger(DEBUG_FATX_FS, DBG_LVL_ERROR, "!!!Error  result:%u", result);
    return -1;
}

int fatxchdrive(const char* path)
{
    /* Not useful for the moment. */
    return -1;
}

int fatxgetfree(const char* path)
{
    DWORD clusters;

    //TODO: fix for slave drive
    int vol = get_ldnumber(&path);
    FATFS* fatfs = &FatXFs[0][vol];
    if(0 > vol)
    {
        return -1;
    }

    if(FR_OK == f_getfree(path, &clusters, &fatfs))
    {
        return clusters;
    }

    return -1;
}

int fatxgetclustersize(unsigned char driveNumber, unsigned char partNumber)
{
    if(NbDrivesSupported <= driveNumber || NbFATXPartPerHDD <= partNumber)
    {
        return -1;
    }

    return FatXFs[driveNumber][partNumber].csize;
}


int fatxputc(FILEX handle, char c)
{
    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    return f_putc(c, &FileHandleArray[handle]);
}

int fatxputs(const char* sz, FILEX handle)
{
    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    BootLogger(DEBUG_FATX_FS, DBG_LVL_TRACE, "\"%s\"", sz);

    return f_puts(sz, &FileHandleArray[handle]);
}

int fatxprintf(FILEX handle, const char* sz, ...)
{
    int wrote;
    va_list args;

    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    va_start( args, sz );
    wrote = f_printf(&FileHandleArray[handle], sz, args);
    va_end( args );


    return wrote;
}

const char* fatxgets(char* out, unsigned int len, FILEX handle)
{
    char* outValidation;
    if(0 == handle || (MaxOpenFileCount < handle))
    {
        return NULL;
    }
    handle -= 1;

    if((MaxOpenFileCount <= handle) || (0 == FileHandleArray[handle].obj.fs)) return NULL;

    outValidation = f_gets(out, len, &FileHandleArray[handle]);

    BootLogger(DEBUG_FATX_FS, DBG_LVL_TRACE, "\"%s\" len:%u  ptr:0x%08X  buflen:%u", out, strlen(out), (unsigned int)outValidation, len);

    return outValidation;
}

long long int fatxtell(FILEX handle)
{
    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    return f_tell(&FileHandleArray[handle]);
}

int fatxeof(FILEX handle)
{
    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    if(f_eof(&FileHandleArray[handle]))
    {
        /* At end of file */
        return 1;
    }

    return 0;
}

long long int fatxsize(FILEX handle)
{
    FILE_HANDLE_VALID(handle)
    FILE_VALID(handle)

    return f_size(&FileHandleArray[handle]);
}

FileDate convertToTime(unsigned short date, unsigned short time)
{
    FileDate out;

    out.year = (date >> 9) + 1980;
    out.month = (date >> 5) & 15;
    out.mday = date & 31;
    out.hours = (time >> 11);
    out.minutes = (time >> 5) & 63;
    out.seconds = (time & 31) * 2;

    return out;
}

