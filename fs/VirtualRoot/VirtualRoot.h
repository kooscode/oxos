/*
 * VirtualRoot.h
 *
 *  Created on: Apr 27, 2018
 *      Author: cromwelldev
 */

#ifndef FS_VIRTUALROOT_VIRTUALROOT_H_
#define FS_VIRTUALROOT_VIRTUALROOT_H_

#include "FatFSAccessor.h"

void VirtualRootInit(void);

FILEX vroot_open(const char* path, FileOpenMode mode);
int vroot_read(FILEX handle, unsigned char* out, unsigned int size);
int vroot_write(FILEX handle, const unsigned char* in, unsigned int size);
void vroot_close(FILEX handle);

int vroot_eof(FILEX handle);
FileInfo vroot_stat(const char* path);

int vroot_rename(const char* path, const char* newName);
int vroot_mkdir(const char* path);
int vroot_remove(const char* path);
int vroot_cd(const char* path);
const char* vroot_getcwd(void);

DIREX vroot_opendir(const char* path);
FileInfo vroot_readdir(DIREX handle);
void vroot_closedir(DIREX handle);


#endif /* FS_VIRTUALROOT_VIRTUALROOT_H_ */
