/*
 * VirtualRoot.c
 *
 *  Created on: Apr 27, 2018
 *      Author: cromwelldev
 */

#include "VirtualRoot.h"
#include "FatFSAccessor.h"
#include "string.h"
#include "stdio.h"
#include <limits.h>
#include "include/BootLogger.h"


typedef struct
{
    int         (*getEntryName) (unsigned char index, const char * *  out);
    FILEX       (*open)         (const char* path, FileOpenMode mode);
    int         (*read)         (FILEX handle, unsigned char* out, unsigned int size);
    int         (*write)        (FILEX handle, const unsigned char* in, unsigned int size);
    int         (*close)        (FILEX handle);

    int         (*eof)          (FILEX handle);
    FileInfo    (*stat)         (const char* path);

    int         (*rename)       (const char* path, const char* newName);
    int         (*mkdir)        (const char* path);
    int         (*remove)       (const char* path);
    int         (*chdir)        (const char* path);

    DIREX       (*opendir)      (const char* path);
    FileInfo    (*readdir)      (DIREX handle);
    int         (*closedir)     (DIREX handle);
} ChildFS_t;

/*-----------------------------------------*/

static ChildFS_t* currentAccessor;
static ChildFS_t FatFSAccess;

#define MaxPathLength 255
static char cwd[MaxPathLength + sizeof('\0')];
const static DIREX VirtualRootDirHandle = INT_MAX;

static unsigned char virtualRootCycler;

static int pathProcess_Absolute(const char* const path);
static int pathProcess_GoingForward(const char* const path);
static int pathProcess_GoingBack(void);

static void combinePath(char* out, const char* add);

/*-----------------------------------------*/

void VirtualRootInit(void)
{
    memset(cwd, '\0', sizeof(char) * (MaxPathLength + sizeof('\0')));
    cwd[0] = cPathSep;

    currentAccessor = NULL;

    virtualRootCycler = 0;

    FatFSAccess.getEntryName = getActivePartName;
    FatFSAccess.open = fatxopen;
    FatFSAccess.read = fatxread;
    FatFSAccess.write = fatxwrite;
    FatFSAccess.close = fatxclose;
    FatFSAccess.eof = fatxeof;
    FatFSAccess.stat = fatxstat;
    FatFSAccess.rename = fatxrename;
    FatFSAccess.mkdir = fatxmkdir;
    FatFSAccess.remove = fatxdelete;
    FatFSAccess.chdir = fatxchdir;
    FatFSAccess.opendir = fatxopendir;
    FatFSAccess.readdir = fatxreaddir;
    FatFSAccess.closedir = fatxclosedir;
}

FILEX vroot_open(const char* path, FileOpenMode mode)
{
    char workPath[300];
    combinePath(workPath, path);
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "path:\"%s\"  mode:%u", workPath, mode);
    if(NULL != currentAccessor)
    {
        return currentAccessor->open(workPath, mode);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return 0;
}

int vroot_read(FILEX handle, unsigned char* out, unsigned int size)
{
    if(NULL != currentAccessor)
    {
        return currentAccessor->read(handle, out, size);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return -1;
}

int vroot_write(FILEX handle, const unsigned char* in, unsigned int size)
{
    if(NULL != currentAccessor)
    {
        return currentAccessor->write(handle, in, size);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return -1;
}

void vroot_close(FILEX handle)
{
    if(NULL != currentAccessor)
    {
        currentAccessor->close(handle);
        return;
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return;
}

int vroot_eof(FILEX handle)
{
    if(NULL != currentAccessor)
    {
        return currentAccessor->eof(handle);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return -1;
}

FileInfo vroot_stat(const char* path)
{
    FileInfo returnStruct;
    char workPath[300];
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "path:\"%s\"", path);

    if(NULL != currentAccessor)
    {
        combinePath(workPath, path);
        return currentAccessor->stat(workPath);
    }

    returnStruct.name[0] = '\0';
    returnStruct.nameLength = 0;
    returnStruct.attributes = FileAttr_Directory | FileAttr_ReadOnly | FileAttr_SysFile;
    returnStruct.size = 0;
    returnStruct.modDate = 0;
    returnStruct.modTime = 0;


    strcpy(returnStruct.name, path);
    returnStruct.nameLength = strlen(returnStruct.name);
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "found:\"%s\"", returnStruct.name);

    return returnStruct;
}

int vroot_rename(const char* path, const char* newName)
{
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "old:\"%s\" -> \"%s\"", path, newName);
    if(NULL != currentAccessor)
    {
        //TODO: Make sure newName has proper path */
        return currentAccessor->rename(path, newName);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return -1;
}

int vroot_mkdir(const char* path)
{
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "path:\"%s\"", path);
    if(NULL != currentAccessor)
    {
        return currentAccessor->mkdir(path);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return -1;
}

int vroot_remove(const char* path)
{
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "path:\"%s\"", path);
    if(NULL != currentAccessor)
    {
        return currentAccessor->remove(path);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!!  No file op in vroot.");

    return -1;
}

int vroot_cd(const char* path)
{
    int result;
    char* sepPos;
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "path:\"%s\"", path);
    if(MaxPathLength < strlen(path))
    {
        BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "!!!Error, too long.");
        return -1;
    }

    if(cPathSep == *path)
    {
        /* Is an absolute path */
        return pathProcess_Absolute(path);
    }


    if(0 == strcmp(path, ".."))
    {
        return pathProcess_GoingBack();
    }
    else
    {
        /* Going forward */
        return pathProcess_GoingForward(path);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!! Don't know how to process.");

    return -1;
}

const char* vroot_getcwd(void)
{
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "%s", cwd);
    return cwd;
}

DIREX vroot_opendir(const char* path)
{
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "path:\"%s\"", path);
    if(NULL != currentAccessor)
    {
        return currentAccessor->opendir(path);
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "Returning vroot handle.");

    return VirtualRootDirHandle;
}

FileInfo vroot_readdir(DIREX handle)
{
    FileInfo returnStruct;

    if(VirtualRootDirHandle != handle && NULL != currentAccessor)
    {
        return currentAccessor->readdir(handle);
    }

    returnStruct.name[0] = '\0';
    returnStruct.nameLength = 0;
    returnStruct.attributes = FileAttr_Directory | FileAttr_ReadOnly | FileAttr_SysFile;
    returnStruct.size = 0;
    returnStruct.modDate = 0;
    returnStruct.modTime = 0;

    if(VirtualRootDirHandle == handle)
    {
        BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "readdir in vroot.");
        /* For now only FatFS is populating Virtual Root */
        const char*  string = NULL;
        virtualRootCycler = FatFSAccess.getEntryName(virtualRootCycler, &string);
        if(NULL != string)
        {
            sprintf(returnStruct.name, "%s", string);
            BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "found:\"%s\"", returnStruct.name);
            returnStruct.nameLength = strlen(returnStruct.name);
        }
    }

    return returnStruct;
}

void vroot_closedir(DIREX handle)
{
    if(NULL != currentAccessor)
    {
        currentAccessor->closedir(handle);
        return;
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "Close vroot handle.");

    virtualRootCycler = 0;

    return;
}


/*-----------------------------------------------*/

static int pathProcess_Absolute(const char* const path)
{
    int result;
    const char* sepPos = path;
    if('\0' == path[1])
    {
        BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "Back to vroot.");
        strcpy(cwd, PathSep);
        currentAccessor = NULL;

        return 0;
    }

    result = FatFSAccess.chdir(sepPos);
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "chdir:\"%s\"   result:%u.", sepPos, result);
    if(0 == result)
    {
        currentAccessor = &FatFSAccess;
        strcpy(cwd, path);
        return 0;
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!! Invalid path.");

    return -1;
}

static int pathProcess_GoingForward(const char* const path)
{
    char workPath[300];

    if(cPathSep != *path)
    {
        combinePath(workPath, path);
        BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "New path:\"%s\".", workPath);

        if(0 == FatFSAccess.chdir(workPath))
        {
            currentAccessor = &FatFSAccess;
            strcpy(cwd, workPath);

            return 0;
        }
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!! Invalid path.");

    return -1;

}

static int pathProcess_GoingBack(void)
{
    char workPath[300];
    int pathLen, result;
    char* sepPos;

    strcpy(workPath, cwd);
    pathLen = strlen(workPath);
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "workPath:\"%s\"  len:%u", workPath, pathLen);

    sepPos = strrchr(workPath + sizeof(cPathSep), cPathSep);
    if(NULL == sepPos)
    {
        BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "!!!Error, requested root. Already at root.");
        /* Already at root */
        return -1;
    }

    *sepPos = '\0';
    sepPos = strrchr(workPath + sizeof(cPathSep), cPathSep);

    if(NULL == sepPos)
    {
        BootLogger(DEBUG_VROOT, DBG_LVL_WARN, "Invalid path. Returning to root.");
        currentAccessor = NULL;
        strcpy(cwd, PathSep);
        return 0;
    }

    sepPos[1] = '\0';
    BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "new path:\"%s\"", workPath);

    if(NULL != currentAccessor)
    {
        result = currentAccessor->chdir(workPath);

        if(0 == result)
        {
            BootLogger(DEBUG_VROOT, DBG_LVL_DEBUG, "Sucess. Path exist in FatFS");
            strcpy(cwd, workPath);
        }
        else
        {
            currentAccessor = NULL;
            strcpy(cwd, PathSep);
        }
    }
    BootLogger(DEBUG_VROOT, DBG_LVL_ERROR, "Error!!! Invalid path.");

    return -1;
}

static void combinePath(char* out, const char* add)
{
    int cwdLen = strlen(cwd);
    if(cwdLen)
    {
        if(cPathSep == cwd[cwdLen - 1])
        {
            sprintf(out, "%s%s", cwd, add);
            return;
        }
    }
    sprintf(out, "%sPathSep%s", cwd, add);
}

