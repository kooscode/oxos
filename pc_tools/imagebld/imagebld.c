#include <errno.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <string.h>

#include <stdarg.h>
#include <stdlib.h>
#include "sha1.h"
#include "md5.h"

#include "../../include/config.h"
#include "../../include/BiosIdentifier.h"
#include "xbe-header.h"


#define debug

struct Checksumstruct {
    uint8_t SHAresult[20];
    uint32_t Size_ramcopy;
}Checksumstruct;

typedef struct
{
    uint8_t SHAresult[20];
    uint32_t compressed_image_size;  // 20
}CromwellChecksumStruct;

void showUsage();

void shax(uint8_t *result, uint8_t *data, uint32_t len)
{
    struct SHA1Context context;
    SHA1Reset(&context);
    SHA1Input(&context, (uint8_t *)&len, 4);
    SHA1Input(&context, data, len);
    SHA1Result(&context,result);    
}


void writeBiosIdentifier(uint8_t *cromimage, int biosSize) {
    struct BiosIdentifier *BiosHeader = (struct BiosIdentifier *)&cromimage[256*1024-sizeof(struct BiosIdentifier)];
    MD5_CTX hashcontext;
    uint32_t md5Size = biosSize;
    uint8_t digest[16];
    char temp[33];
    memcpy(BiosHeader->Magic, MagicBiosHeaderValue,4);
    BiosHeader->HeaderVersion = CurrentHeaderVersion;
    BiosHeader->BiosSize = biosSize;
    sprintf(BiosHeader->Name,"%s %s",PROG_NAME, VERSION);
    printf("\n");
                                       
    BiosHeader->XboxVersion =       BiosID_Version10 |
                                    BiosID_Version11 |
                                    BiosID_Version12 |
                                    BiosID_Version13 |
                                    BiosID_Version14 |
                                    BiosID_Version15 |
                                    BiosID_Version16 |
                                    BiosID_Version17 ;
                                     
    BiosHeader->VideoEncoder =      BiosID_VideoEncoder_Conexant |
                                    BiosID_VideoEncoder_Focus |
                                    BiosID_VideoEncoder_Xcalibur;

    BiosHeader->Option1 =           Option1_SaveSettingsLocationBit;

    if(BiosHeader->HeaderVersion == HeaderVersionV2)
    {
        md5Size = BiosHeader->BiosSize;
    }
    else if(BiosHeader->HeaderVersion == HeaderVersionV1)
    {
        md5Size = 256 * 1024 - 0x1000;
    }
    else
    {
        printf("Invalid BiosIdentifier Header version. Aborting.\n");
        return;
    }



    MD5Init(&hashcontext);
    MD5Update(&hashcontext, cromimage, md5Size);
    MD5Final(BiosHeader->MD5Hash, &hashcontext);

    memcpy(temp, BiosHeader->Magic, 4);
    temp[4] = '\0';
    printf("BiosIdentifier content\n");
    printf("Magic:          %s\n", temp);
    printf("HeaderVersion:  %u\n", BiosHeader->HeaderVersion);
    printf("XboxVersion:    %u\n", BiosHeader->XboxVersion);
    printf("VideoEncoder:   %u\n", BiosHeader->VideoEncoder);
    printf("Option1:        %u\n", BiosHeader->Option1);
    printf("Option2:        %u\n", BiosHeader->Option2);
    printf("Option3:        %u\n", BiosHeader->Option3);
    printf("BiosSize:       %u\n", BiosHeader->BiosSize);
    memcpy(temp, BiosHeader->Name, 32);
    temp[32] = '\0';
    printf("Name:           %s\n", BiosHeader->Name);
    printf("MD5Hash:        %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", BiosHeader->MD5Hash[0], BiosHeader->MD5Hash[1], BiosHeader->MD5Hash[2], BiosHeader->MD5Hash[3], BiosHeader->MD5Hash[4], BiosHeader->MD5Hash[5], BiosHeader->MD5Hash[6], BiosHeader->MD5Hash[7], BiosHeader->MD5Hash[8], BiosHeader->MD5Hash[9], BiosHeader->MD5Hash[10], BiosHeader->MD5Hash[11], BiosHeader->MD5Hash[12], BiosHeader->MD5Hash[13], BiosHeader->MD5Hash[14], BiosHeader->MD5Hash[15]);

    printf("\nMD5 hash calculated on buffer size : 0x%X bytes\n\n", md5Size);
}

int xberepair (    uint8_t * xbeimage,
        uint8_t * cromimage
        )
{
    FILE *f;
    uint32_t xbesize;
    uint32_t a;
    uint8_t sha_Message_Digest[SHA1HashSize];
	uint8_t *crom;
	uint8_t *xbe;
	uint32_t romsize=0;
	struct stat fileinfo;
	int unused;
    
    XBE_HEADER *header;
    XBE_SECTION *sechdr;

    crom = malloc(1024*1024);
    xbe = malloc(1024*1024+0x3000);

    printf("XBE Mode\n");

    f = fopen(cromimage, "r");
	if (f==NULL)
	{
        fprintf(stderr,"Unable to open cromwell image file %s : %s \nAborting\n",xbeimage,strerror(errno)); 
        return 1;
    }
  
    fstat(fileno(f), &fileinfo);
    romsize = fileinfo.st_size;
    if (romsize>0x100000)
    {
		printf("Romsize too big, increase the static variables everywhere");
		return 1;
	}
    
    
    fseek(f, 0, SEEK_SET);
    unused = fread(crom, 1, romsize, f);
    fclose(f);

    f = fopen(xbeimage, "r");
	if (f==NULL) {
        fprintf(stderr,"Unable to open xbe destination file %s : %s \nAborting\n",xbeimage,strerror(errno)); 
        return 1;
    }
 
    fstat(fileno(f), &fileinfo);
    xbesize = fileinfo.st_size;


    fseek(f, 0, SEEK_SET);

    memset(xbe,0x00,sizeof(xbe));
    unused = fread(xbe, 1, xbesize, f);
    fclose(f);

    // We copy the ROM image now into the Thing
    memcpy(&xbe[0x3000],crom,romsize);
    memcpy(&xbe[0x1084],&romsize,4);    // We dump the ROM Size into the Image

    romsize = (romsize & 0xfffffff0) + 32;    // We fill it up with "spaces"

    header = (XBE_HEADER*) xbe;
    // This selects the First Section, we only have one
    sechdr = (XBE_SECTION *)(((char *)xbe) + (uint32_t)header->Sections - (uint32_t)header->BaseAddress);
            
    // Correcting overall size now
    xbesize = 0x3000+romsize;
    header->ImageSize = xbesize;
    
    //printf("%08x",sechdr->FileSize);                    
    sechdr->FileSize = 0x2000+romsize;
    sechdr->VirtualSize = 0x2000+romsize;
        
 //       printf("Sections: %d\n",header->NumSections);

    shax(&sha_Message_Digest[0], ((uint8_t *)xbe)+(int32_t)sechdr->FileAddress ,sechdr->FileSize);
    memcpy(&sechdr->ShaHash[0],&sha_Message_Digest[0],20);
      
#ifdef debug
    printf("Size of all headers:     : 0x%08X\n", (uint32_t)header->HeaderSize);
        
    printf("Size of entire image     : 0x%08X\n", (uint32_t)header->ImageSize);
    printf("Virtual address          : 0x%08X\n", (uint32_t)sechdr->VirtualAddress);
    printf("Virtual size             : 0x%08X\n", (uint32_t)sechdr->VirtualSize);
    printf("File address             : 0x%08X\n", (uint32_t)sechdr->FileAddress);
    printf("File size                : 0x%08X\n", (uint32_t)sechdr->FileSize);

    printf("Section 0 Hash XBE       : ");

    for(a=0; a<SHA1HashSize; a++)
    {
        printf("%02x",sha_Message_Digest[a]);
    }

    printf("\n");
#endif
          
    // Write back the Image to Disk
    f = fopen(xbeimage, "w");
    if (f==NULL)
    {
        fprintf(stderr,"Unable to open xbe destination file %s : %s \nAborting\n",xbeimage,strerror(errno)); 
        return 1;
    }

    fwrite(xbe, 1, xbesize, f);
    fclose(f);
        
    printf("XRomwell File Created    : %s\n\n",xbeimage);
        
    return 0;    
}


int vmlbuild(uint8_t * vmlimage, uint8_t * cromimage)
{
    FILE *f;
    uint32_t vmlsize;
    int32_t a;

    uint8_t *crom;
    uint8_t *vml;
    uint32_t romsize = 0;
    struct stat fileinfo;
    int unused;
    
    crom = malloc(1024*1024);
    vml = malloc(1024*1024+0x3000);
         
    printf("VML Mode\n");
    
    f = fopen(cromimage, "r");
    if (f==NULL)
    {
        fprintf(stderr,"Unable to open cromwell image file %s : %s \nAborting\n",cromimage,strerror(errno)); 
        return 1;
    }
    
    fstat(fileno(f),&fileinfo);
    romsize = fileinfo.st_size;
        
    if (romsize>0x100000)
    {
        printf("Romsize too big, increase the static variables everywhere");
        return 1;
    }

    fseek(f, 0, SEEK_SET);
    unused = fread(crom, 1, romsize, f);
    fclose(f);
    
    f = fopen(vmlimage, "r");
    if (f==NULL)
    {
        fprintf(stderr,"Unable to open vml image file %s : %s \nAborting\n",vmlimage,strerror(errno)); 
        return 1;
    }
    
    fstat(fileno(f),&fileinfo);
    vmlsize = fileinfo.st_size;
    
    fseek(f, 0, SEEK_SET);
    unused = fread(vml, 1, vmlsize, f);
    fclose(f);
                
    memcpy(&vml[0x1700], crom, romsize);
    memset(&vml[0x1700 + romsize], 0x0, 100);
    vmlsize += romsize;
    vmlsize += 100;
        
    // Write back the Image to Disk
    f = fopen(vmlimage, "w");
    if (f==NULL)
    {
        fprintf(stderr,"Unable to open vml image file %s : %s \nAborting\n",vmlimage,strerror(errno)); 
        return 1;
    }
    fwrite(vml, 1, vmlsize, f);
    fclose(f);
    
    printf("VML File Created         : %s\n\n",vmlimage);
    
    return 0;
}

int romcopy (uint8_t * blbinname, uint8_t * cromimage, uint8_t * binname256)
{
    
    static uint8_t SHA1_result[SHA1HashSize];
    struct SHA1Context context;
    FILE *f;
    uint8_t *loaderimage;
    uint8_t *flash256;
    uint8_t *crom;
    uint32_t freeflashspace = 252*1024;     //Reserve last 4KB of flash space for persistent settings.
    uint32_t romsize=0;
    uint32_t a=0;
    struct Checksumstruct bootloaderstruct ;
    uint32_t bootloaderpos;
    uint32_t temp;
    struct stat fileinfo;
    int unused;
    const unsigned int compressed_image_start = 0x3018;
    
    loaderimage = malloc(256*1024);
    flash256 = malloc(256*1024);
    crom = malloc(256*1024);
           
    memset(flash256,0x00,256*1024);
    memset(crom,0x00,256*1024);
    memset(loaderimage,0x00,256*1024);

   printf("ROM Mode\n");
          
    f = fopen(blbinname, "r"); // 2bl
    if (f==NULL)
    {
        fprintf(stderr,"Unable to open blimage file %s : %s \nAborting\n",blbinname,strerror(errno)); 
        return 1;
    }

    unused = fread(loaderimage, 1, 256*1024, f); // 2bl into loaderimage buff;
    fclose(f);


    f = fopen(cromimage, "r"); //Cromwell gzip
    if (f==NULL)
    {
        fprintf(stderr,"Unable to open cromwell image file %s : %s \nAborting\n",cromimage,strerror(errno)); 
        return 1;
    }
       
    fstat(fileno(f), &fileinfo);
    romsize = fileinfo.st_size;
    
    fseek(f, 0, SEEK_SET);
    unused = fread(crom, 1, romsize, f); //Cromwell gzip
    fclose(f);

    // Ok, we have loaded both images, we can continue
    
    // this is very nasty, but simple , we Dump a GDT to the TOP rom
    // XXX: necessary? MCPX supposedly replace top 512 bytes with its own bootsect. Might free up some space.
    memset(&loaderimage[0x3fe00],0x90,512);
    memset(&loaderimage[0x3ffd0],0x00,32);
    loaderimage[0x3ffcf] = 0xfc;
    loaderimage[0x3ffd0] = 0xea;
    loaderimage[0x3ffd2] = 0x10;
    loaderimage[0x3ffd3] = 0xfc;
    loaderimage[0x3ffd4] = 0xff;
    loaderimage[0x3ffd5] = 0x08;
    loaderimage[0x3ffd7] = 0x90;

    loaderimage[0x3ffe0] = 0xff;
    loaderimage[0x3ffe1] = 0xff;
    loaderimage[0x3ffe5] = 0x9b;
    loaderimage[0x3ffe6] = 0xcf;
    loaderimage[0x3ffe8] = 0xff;
    loaderimage[0x3ffe9] = 0xff;
    loaderimage[0x3ffed] = 0x93;
    loaderimage[0x3ffee] = 0xcf;

    loaderimage[0x3fff4] = 0x18;
    loaderimage[0x3fff5] = 0x00;
    loaderimage[0x3fff6] = 0xd8;
    loaderimage[0x3fff7] = 0xff;
    loaderimage[0x3fff8] = 0xff;        
    loaderimage[0x3fff9] = 0xff;
           
    // We have dumped the GDT now, we continue    
                
    memcpy(&bootloaderpos,&loaderimage[0x40],4);       // This can be found in the 2bBootStartup.S
    memset(&loaderimage[0x40],0x0,4);            // We do not need this helper sum anymore
    memcpy(&bootloaderstruct.Size_ramcopy,&loaderimage[bootloaderpos + 20],sizeof(uint32_t));
    
    SHA1Reset(&context);
    SHA1Input(&context,&loaderimage[bootloaderpos + sizeof(struct Checksumstruct)], bootloaderstruct.Size_ramcopy - sizeof(struct Checksumstruct));
    SHA1Result(&context,SHA1_result);
    memcpy(&bootloaderstruct.SHAresult,SHA1_result,20);
    memcpy(&loaderimage[bootloaderpos], &bootloaderstruct, sizeof(struct Checksumstruct));

    memcpy(flash256,loaderimage,256*1024);
    //memcpy(flash1024,loaderimage,256*1024);
    
    // We make now sure, there are some "space" bits and we start oranized with 16
#if 0
    temp = bootloaderpos + bootloaderstruct.Size_ramcopy;
    temp = temp & 0xfffffff0;
    temp = temp + 0x10;
#endif

    if(bootloaderpos + bootloaderstruct.Size_ramcopy > 0x3000)
    {
        printf("!!!!!!!!WARNING!!!!!!!!\n");
        printf("2bl image cannot fit in specified maximum file size.\n");
        printf("Image generation was aborted.\n");
        printf("Total size required            : %08x (%d Byte)\n",bootloaderpos + bootloaderstruct.Size_ramcopy, bootloaderpos + bootloaderstruct.Size_ramcopy);
        printf("Maximum size allowed           : %08x (%d Byte)\n\n",0x3000, 0x3000);

        return 0;
    }
    temp = 0x3000;
    
    CromwellChecksumStruct cromwellChecksumStruct;

    // We add additional 0x100 byts for some space
    //temp = temp + 0x100;      //Why?
    
    cromwellChecksumStruct.compressed_image_size =  romsize;
    //freeflashspace = freeflashspace - 512; // We decrement the TOP ROM
    // We have no TOP ROM anymore
    freeflashspace = freeflashspace - compressed_image_start;
    //compressed crom image too big for available flash space
    //This condition is valid for 256KB image of XBlast OS as the
    //last 4 KB of flash space is reserved for settings, CRC and header information
    //Specifically for XBlast OS, it is forbidden to generate an image of
    //total size bigger than 252KB + 4KB.
    //Fail bin generation if size condition cannot be met.
    if(freeflashspace < romsize)
    {
        printf("!!!!!!!!WARNING!!!!!!!!\n");
        printf("Compressed image cannot fit in specified maximum file size.\n");
        printf("Image generation was aborted.\n");
        printf("Total size required            : %08x (%d Byte)\n",cromwellChecksumStruct.compressed_image_size, cromwellChecksumStruct.compressed_image_size);
        printf("Maximum size allowed           : %08x (%d Byte)\n\n",freeflashspace, freeflashspace);

        return 0;
    }
    
    memcpy(&flash256[bootloaderpos],&bootloaderstruct,sizeof(struct Checksumstruct));
    memcpy(&flash256[temp],&cromwellChecksumStruct,sizeof(CromwellChecksumStruct));
    //bootloaderstruct.Biossize_type = 1; // Means it is a 1MB Image
    //memcpy(&flash1024[bootloaderpos],&bootloaderstruct,sizeof(struct Checksumstruct));
    
#ifdef debug        
    printf("BootLoaderPos            : %08x\n",bootloaderpos);
    printf("Size2blRamcopy           : %08x\n",bootloaderstruct.Size_ramcopy);        
    printf("ROM Image Start          : %08x\n",compressed_image_start);
    printf("ROM Image Size           : %08x (%d Byte)\n",romsize,romsize); 
    printf("ROM compressed image size: %08x (%d Byte)\n",cromwellChecksumStruct.compressed_image_size,cromwellChecksumStruct.compressed_image_size);
    printf("Available space in ROM   : %08x (%d Byte)\n",freeflashspace,freeflashspace);
    printf("Percentage of ROM used   : %2.2f \n",(float)cromwellChecksumStruct.compressed_image_size/freeflashspace*100);
#endif
    
    // We have calculated the size of the kompressed image and where it can start (where the 2bl ends)

          
                
    // Ok, the 2BL loader is ready, we now go to the "Kernel"
    memset(&flash256[compressed_image_start+romsize],0xff,256*1024-(compressed_image_start+romsize)-512);
    // The first 20 bytes of the compressed image are the checksum
    memcpy(&flash256[compressed_image_start],&crom[0],romsize);
    SHA1Reset(&context);
    SHA1Input(&context,&flash256[compressed_image_start],romsize);
    SHA1Result(&context,SHA1_result);                                
    memcpy(&flash256[temp],SHA1_result,20);

                      
#ifdef debug
//    printf("ROM Hash 1MB             : ");
//    for(a=0; a<SHA1HashSize; a++) {
//        printf("%02X",SHA1_result[a]);
//    }
//          printf("\n");
#endif
     
    //Apply the SmartXX bios identifier data
    //V2 for now
    writeBiosIdentifier(flash256, compressed_image_start + cromwellChecksumStruct.compressed_image_size);
    // Write the 256 /1024 Kbyte Image Back
    f = fopen(binname256, "w");
    fwrite(flash256, 1, 256*1024, f);
    fclose(f);
              
#ifdef debug
    printf("Binary 256k File Created : %s\n\n",binname256);
#endif                  
    return 0;    
}        




int main (int argc, const char * argv[])
{
           printf("ImageBLD Hasher by XBL Project (c) hamtitampti\n");
    
    if( argc < 3 ) {
        showUsage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (!strcmp(argv[1],"-xbe")) { 
        xberepair((uint8_t*)argv[2],(uint8_t*)argv[3]);
    }
    else if (!strcmp(argv[1],"-vml")) { 
        vmlbuild((uint8_t*)argv[2],(uint8_t*)argv[3]);
    }
    else if (!strcmp(argv[1],"-rom")) { 
        if( argc != 5  ) {
            showUsage(argv[0]);
            exit(EXIT_FAILURE);
        }
        if(romcopy((uint8_t*)argv[2],(uint8_t*)argv[3],(uint8_t*)argv[4]))
            exit(EXIT_FAILURE);
    }
    else {
        showUsage(argv[0]);
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}

void showUsage(char *progname) {
//    printf("Usage:\n",progname);
    printf("Usage:\n");
//    printf("%s -vml vmlname romname\n");
    printf("%s -vml vmlname romname\n",progname);

//    printf("%s -xbe xbename romname\n");
    printf("%s -xbe xbename romname\n",progname);
    printf("\tGenerates Xbox .xbe executable xbename from the rom image romname\n\n");

//    printf("%s -rom blname romname image256name image1024name\n");
    printf("%s -rom blname romname image256name image1024name\n",progname);
    printf("\tGenerates Xbox bios images image256name, image1024name, from the\n");
    printf("\tbootloader image blname, and the rom image romname\n\n");
}
