
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************
*/
// 20040924 - Updated by dmp to include more str functions, and use ASM
// where possible. ASM shamelessly stolen from linux-2.6.8.1
// include/asm-i386/string.h
#include "boot.h"
#include "string.h"
#include "stdlib.h"

inline void BootPciInterruptEnable()  {    __asm__ __volatile__  (  "sti" ); }

int tolower(int ch)
{
      if ( (unsigned int)(ch - 'A') < 26u )
            ch += 'a' - 'A';
      return ch;
}

int toupper(int ch)
{
    if(ch>='a' && ch<='z')
        return('A' + ch - 'a');
    else
        return(ch);
}

/* -------------------------------------------------------------------- */


unsigned int MemoryManagerStartAddress=0;

int MemoryManagerGetFree(void) {
    
    unsigned char *memsmall = (void*)MemoryManagerStartAddress;
    int freeblocks = 0;
    int counter;
    for (counter=0;counter<0x400;counter++) {
        if (memsmall[counter]==0x0) freeblocks++;    
    }
    return freeblocks;
        
}

void MemoryManagementInitialization(void * pvStartAddress, unsigned int dwTotalMemoryAllocLength)
{
    unsigned char *mem = pvStartAddress;
    //if (dwTotalMemoryAllocLength!=0x1000000) return 1;

    MemoryManagerStartAddress = (unsigned int)pvStartAddress;

    // Prepare the memory cluster Table to be free
    memset(mem,0x0,0x4000);
    // Set the first cluster to be "reserved"
    mem[0] = 0xff;

    return ;
}


void * t_malloc(size_t size)
{
    unsigned char *memsmall = (void*)MemoryManagerStartAddress;
    unsigned int counter;
    unsigned int dummy = 0;
    unsigned int blockcount = 0;
    unsigned int temp;

    // this is for 16 kbyes allocations (quick & fast)
    if (size<(0x4000+1)) {
        for (counter=1;counter<0x400;counter++) {
            if (memsmall[counter]==0)
            {
                memsmall[counter] = 0xAA;
                return (void*)(MemoryManagerStartAddress+counter*0x4000);
            }
        }
        // No free Memory found
        return 0;
    }

    // this is for 64 kbyte allocations (also quick)
    if (size<(0x10000+1)) {
        for (counter=1;counter<0x400;counter++) {
            if (memcmp(&memsmall[counter],&dummy,4)==0)
            {
                dummy = 0xB8BADCFE;
                memcpy(&memsmall[counter],&dummy,4);
                return (void*)(MemoryManagerStartAddress+counter*0x4000);
            }
        }
        // No free Memory found
        return 0;
    }

    if (size<(5*1024*1024+1)) {

        for (counter=1;counter<0x400;counter++) {
            unsigned int needsectory;
            unsigned int foundstart;

            temp = (size & 0xffffc000) + 0x4000;
            needsectory = temp / 0x4000;

            //printf("Need Sectors %x\n",needsectory);

            foundstart = 1;
            for (blockcount=0;blockcount<needsectory;blockcount++) {
                if (memsmall[counter+blockcount]!=0    ) {
                    foundstart = 0;
                    break;
                }
            }

            if (foundstart == 1)
            {
                // We found a free sector
                //printf("Found Sectors Start %x\n",counter);
                memset(&memsmall[counter],0xFF,needsectory);
                memsmall[counter] = 0xBB;
                memsmall[counter+1] = 0xCC;
                memsmall[counter+needsectory-2] = 0xCC;
                memsmall[counter+needsectory-1] = 0xBB;

                return (void*)(MemoryManagerStartAddress+counter*0x4000);
            }

        }
        return 0;
    }

    return 0;

}



void t_free (void *ptr)
{
    unsigned char *memsmall = (void*)MemoryManagerStartAddress;
    unsigned int temp;
    unsigned int dummy = 0;
    unsigned int point = (unsigned int)ptr;

    // this is the offset of the Free thing
    temp = point - MemoryManagerStartAddress;

    if ((temp & 0xffffc000) == temp)
    {
        // Allignement OK
        temp = temp / 0x4000;
        //printf("Free %x\n",temp);

        if (memsmall[temp] == 0xAA)
        {
            // Found Small Block, free it
            ptr = NULL;
            memsmall[temp] = 0x0;
            return;
        }

        dummy = 0xB8BADCFE;
        if (memcmp(&memsmall[temp],&dummy,4)==0)
        {
            // Found 64 K block, free it
            ptr = NULL;
            dummy = 0;
            memset(&memsmall[temp],dummy,4);
            return;
        }


        dummy = 0xFFFFCCBB;
        if (memcmp(&memsmall[temp],&dummy,4)==0)
        {
            unsigned int counter;
            // Found 64 K block, free it
            //printf("Found Big block %x\n",temp);
            ptr = NULL;
            for (counter=temp;counter<0x400;counter++)
            {
                if ((memsmall[counter]==0xCC)&&(memsmall[counter+1]==0xBB))
                {
                    // End detected
                    memsmall[counter]=0;
                    memsmall[counter+1]=0;
                    return;
                }
                memsmall[counter]=0;
            }
            return;
        }

    }

}



void * malloc(size_t size) {

    size_t temp;
    unsigned char *tempmalloc;
    unsigned int *tempmalloc1;
    unsigned int *tempmalloc2;
         __asm__ __volatile__  (  "cli" );
         
    temp = (size+0x200) & 0xffFFff00;

    tempmalloc = t_malloc(temp);
    tempmalloc2 = (unsigned int*)tempmalloc;

    tempmalloc = (unsigned char *)((unsigned int)(tempmalloc+0x100) & 0xffFFff00);
    tempmalloc1 = (unsigned int*)tempmalloc;
    tempmalloc1--;
    tempmalloc1--;
    tempmalloc1[0] = (unsigned int)tempmalloc2;
    tempmalloc1[1] = 0x1234567;
    BootPciInterruptEnable();
        
    return tempmalloc;
}

void *calloc (size_t __nmemb, size_t __size)
{
    void* ptr = malloc(__nmemb * __size);
    memset(ptr, 0x00, __nmemb * __size);

    return ptr;
}

void free(void *ptr) {

    unsigned int *tempmalloc1;
        __asm__ __volatile__  (  "cli" );
          
          if (ptr == NULL) return;
            
    tempmalloc1 = ptr;
    tempmalloc1-=2;
    ptr = (unsigned int*)tempmalloc1[0];
        if (tempmalloc1[1]!= 0x1234567) {
        	BootPciInterruptEnable();
            return ;
    }        
    t_free(ptr);
    BootPciInterruptEnable();

}
