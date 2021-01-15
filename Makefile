CC	= gcc
LD  = ld
OBJCOPY = objcopy

INCLUDE = -I$(TOPDIR)/grub -I$(TOPDIR)/include -I$(TOPDIR)/ -I./ -I$(TOPDIR)/fs/cdrom \
	-I$(TOPDIR)/fs/fatx/source -I$(TOPDIR)/fs/grub -I$(TOPDIR)/lib/eeprom -I$(TOPDIR)/lib/crypt \
	-I$(TOPDIR)/drivers/video -I$(TOPDIR)/drivers/ide -I$(TOPDIR)/lib/misc \
	-I$(TOPDIR)/boot_xbe/ -I$(TOPDIR)/fs/grub -I$(TOPDIR)/lib/cromwell/font \
	-I$(TOPDIR)/startuploader -I$(TOPDIR)/drivers/cpu -I$(TOPDIR)/menu \
  -I$(TOPDIR)/menu/actions -I$(TOPDIR)/menu/textmenu

CROM_CFLAGS += $(INCLUDE) -fno-zero-initialized-in-bss
CFLAGS = -Os -march=pentium -m32 -Werror -Wstrict-prototypes -Wreturn-type -pipe -fomit-frame-pointer -DIPv4 -fpack-struct -ffreestanding
CFLAGS += -fno-stack-protector -U_FORTIFY_SOURCE -fno-PIC
2BL_CFLAGS= -O2 -march=pentium -m32 -Werror -Wstrict-prototypes -Wreturn-type -pipe -fomit-frame-pointer -fpack-struct -ffreestanding -fno-PIC

export CC

TOPDIR  := $(shell /bin/pwd)
SUBDIRS	= fs drivers lib boot menu

LDFLAGS-ROM     = -s -S -T $(TOPDIR)/scripts/ldscript-crom.ld
LDFLAGS-ROMBOOT = -s -S -T $(TOPDIR)/boot_rom/bootrom.ld
LDFLAGS-VMLBOOT = -s -S -T $(TOPDIR)/boot_vml/vml_start.ld

OBJECTS-ROMBOOT = $(TOPDIR)/obj/2bBootStartup.o
OBJECTS-ROMBOOT += $(TOPDIR)/obj/2bBootStartBios.o
OBJECTS-ROMBOOT += $(TOPDIR)/obj/2bPicResponseAction.o
OBJECTS-ROMBOOT += $(TOPDIR)/obj/sha1.o
OBJECTS-ROMBOOT += $(TOPDIR)/obj/2bBootLibrary.o
OBJECTS-ROMBOOT += $(TOPDIR)/obj/misc.o
                                             
OBJECTS-CROM = $(TOPDIR)/obj/BootStartup.o
OBJECTS-CROM += $(TOPDIR)/obj/BootResetAction.o
OBJECTS-CROM += $(TOPDIR)/obj/i2c.o
OBJECTS-CROM += $(TOPDIR)/obj/pci.o
OBJECTS-CROM += $(TOPDIR)/obj/BootVgaInitialization.o
OBJECTS-CROM += $(TOPDIR)/obj/VideoInitialization.o
OBJECTS-CROM += $(TOPDIR)/obj/conexant.o
OBJECTS-CROM += $(TOPDIR)/obj/focus.o
OBJECTS-CROM += $(TOPDIR)/obj/xcalibur.o
OBJECTS-CROM += $(TOPDIR)/obj/BootIde.o
OBJECTS-CROM += $(TOPDIR)/obj/BootHddKey.o
OBJECTS-CROM += $(TOPDIR)/obj/rc4.o
OBJECTS-CROM += $(TOPDIR)/obj/sha1.o
OBJECTS-CROM += $(TOPDIR)/obj/BootVideoHelpers.o
OBJECTS-CROM += $(TOPDIR)/obj/cromString.o
OBJECTS-CROM += $(TOPDIR)/obj/string.o
OBJECTS-CROM += $(TOPDIR)/obj/sortHelpers.o
OBJECTS-CROM += $(TOPDIR)/obj/rand.o
OBJECTS-CROM += $(TOPDIR)/obj/vsprintf.o
OBJECTS-CROM += $(TOPDIR)/obj/timeManagement.o
OBJECTS-CROM += $(TOPDIR)/obj/Gentoox.o
OBJECTS-CROM += $(TOPDIR)/obj/LED.o
OBJECTS-CROM += $(TOPDIR)/obj/TextMenu.o
OBJECTS-CROM += $(TOPDIR)/obj/TextMenuInit.o
OBJECTS-CROM += $(TOPDIR)/obj/ResetMenuInit.o
OBJECTS-CROM += $(TOPDIR)/obj/MenuActions.o
OBJECTS-CROM += $(TOPDIR)/obj/ResetMenuActions.o
OBJECTS-CROM += $(TOPDIR)/obj/OnScreenKeyboard.o
OBJECTS-CROM += $(TOPDIR)/obj/setup.o
OBJECTS-CROM += $(TOPDIR)/obj/iso9660.o
OBJECTS-CROM += $(TOPDIR)/obj/BootLibrary.o
OBJECTS-CROM += $(TOPDIR)/obj/cputools.o
OBJECTS-CROM += $(TOPDIR)/obj/microcode.o
OBJECTS-CROM += $(TOPDIR)/obj/ioapic.o
OBJECTS-CROM += $(TOPDIR)/obj/BootInterrupts.o
OBJECTS-CROM += $(TOPDIR)/obj/BootEEPROM.o
OBJECTS-CROM += $(TOPDIR)/obj/ff.o
OBJECTS-CROM += $(TOPDIR)/obj/diskio.o
OBJECTS-CROM += $(TOPDIR)/obj/FatFSAccessor.o
OBJECTS-CROM += $(TOPDIR)/obj/ProgressBar.o
OBJECTS-CROM += $(TOPDIR)/obj/ConfirmDialog.o
OBJECTS-CROM += $(TOPDIR)/obj/md5.o
OBJECTS-CROM += $(TOPDIR)/obj/crc32.o
OBJECTS-CROM += $(TOPDIR)/obj/strtol.o
OBJECTS-CROM += $(TOPDIR)/obj/strnstr.o
OBJECTS-CROM += $(TOPDIR)/obj/strcasecmp.o
OBJECTS-CROM += $(TOPDIR)/obj/memrchr.o
OBJECTS-CROM += $(TOPDIR)/obj/strrchr.o
OBJECTS-CROM += $(TOPDIR)/obj/cromSystem.o
OBJECTS-CROM += $(TOPDIR)/obj/CallbackTimer.o

#USB
OBJECTS-CROM += $(TOPDIR)/obj/config.o 
OBJECTS-CROM += $(TOPDIR)/obj/hcd-pci.o
OBJECTS-CROM += $(TOPDIR)/obj/hcd.o
OBJECTS-CROM += $(TOPDIR)/obj/hub.o
OBJECTS-CROM += $(TOPDIR)/obj/message.o
OBJECTS-CROM += $(TOPDIR)/obj/ohci-hcd.o
OBJECTS-CROM += $(TOPDIR)/obj/buffer_simple.o
OBJECTS-CROM += $(TOPDIR)/obj/urb.o
OBJECTS-CROM += $(TOPDIR)/obj/usb-debug.o
OBJECTS-CROM += $(TOPDIR)/obj/usb.o
OBJECTS-CROM += $(TOPDIR)/obj/BootUSB.o
OBJECTS-CROM += $(TOPDIR)/obj/usbwrapper.o
OBJECTS-CROM += $(TOPDIR)/obj/linuxwrapper.o
OBJECTS-CROM += $(TOPDIR)/obj/xpad.o
OBJECTS-CROM += $(TOPDIR)/obj/risefall.o

RESOURCES = $(TOPDIR)/obj/backdrop.elf

export INCLUDE
export TOPDIR

.PHONY: all clean

all: 
	@$(MAKE) --no-print-directory resources cromsubdirs obj/image-crom.bin cromwell.bin imagecompress 256KBBinGen

cromsubdirs: $(patsubst %, _dir_%, $(SUBDIRS))
$(patsubst %, _dir_%, $(SUBDIRS)) : dummy
	$(MAKE) CFLAGS="$(CFLAGS) $(CROM_CFLAGS)" -C $(patsubst _dir_%, %, $@)
	
2blsubdirs: $(patsubst %, _dir_%, boot_rom)
$(patsubst %, _dir_%, boot_rom) : dummy
	$(MAKE) CFLAGS="$(2BL_CFLAGS) $(INCLUDE)" -C $(patsubst _dir_%, %, $@)

dummy:

resources:
	${LD} -r --oformat elf32-i386 -o $(TOPDIR)/obj/backdrop.elf -T $(TOPDIR)/scripts/backdrop.ld -b binary $(TOPDIR)/pics/backdrop.jpg	
	
clean:
	find . \( -name '*.[oas]' -o -name core -o -name '.*.flags' \) -type f -print | grep -v lxdialog/ | xargs rm -f
	rm -fr $(TOPDIR)/obj
	rm -fr $(TOPDIR)/image
	rm -f $(TOPDIR)/bin/imagebld*
	rm -f $(TOPDIR)/bin/crc*
	rm -f $(TOPDIR)/bin/*.o
	rm -f $(TOPDIR)/boot_vml/disk/vmlboot
	mkdir -p $(TOPDIR)/image
	mkdir -p $(TOPDIR)/obj 
	mkdir -p $(TOPDIR)/bin

obj/image-crom.bin: cromsubdirs resources
	${LD} -o obj/image-crom.elf ${OBJECTS-CROM} ${RESOURCES} ${LDFLAGS-ROM} -Map $(TOPDIR)/obj/image-crom.map
	${OBJCOPY} --output-target=binary --strip-all obj/image-crom.elf $@

vmlboot: vml_startup 
	${LD} -o $(TOPDIR)/obj/vmlboot.elf ${OBJECTS-VML} ${LDFLAGS-VMLBOOT}
	${OBJCOPY} --output-target=binary --strip-all $(TOPDIR)/obj/vmlboot.elf $(TOPDIR)/boot_vml/disk/$@
	
vml_startup:
	$(CC) ${CFLAGS} -c -o ${OBJECTS-VML} boot_vml/vml_Startup.S

cromwell.bin: cromsubdirs 2blsubdirs
	${LD} -o $(TOPDIR)/obj/2lbimage.elf ${OBJECTS-ROMBOOT} ${LDFLAGS-ROMBOOT} -Map $(TOPDIR)/obj/2lbimage.map
	${OBJCOPY} --output-target=binary --strip-all $(TOPDIR)/obj/2lbimage.elf $(TOPDIR)/obj/2blimage.bin

# PC Tool - Image Builder
bin/imagebld:
	gcc -Ilib/crypt -o bin/sha1.o -c lib/crypt/sha1.c
	gcc -Ilib/crypt -o bin/md5.o -c lib/crypt/md5.c
	gcc -Ilib/crypt -o bin/imagebld.o -c pc_tools/imagebld/imagebld.c
	gcc -o bin/imagebld bin/imagebld.o bin/sha1.o bin/md5.o

# PC Tool - CRC Generator
crcbin:
	gcc -o bin/crcbin.o -c pc_tools/crcbin/crcbin.c
	gcc -o bin/crc32.o -c lib/misc/crc32.c
	gcc -o bin/crcbin bin/crcbin.o bin/crc32.o

imagecompress: obj/image-crom.bin bin/imagebld
	cp obj/image-crom.bin obj/c
	gzip -f -9 obj/c
	bin/imagebld -vml boot_vml/disk/vmlboot obj/image-crom.bin f

256KBBinGen:  crcbin imagecompress cromwell.bin
	bin/imagebld -rom bin/2blimage.bin obj/c.gz image/cromwell.bin
	bin/crcbin image/cromwell.bin image/crcwell.bin