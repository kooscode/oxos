# oxos_ftp.xbe, the OXOS FTP server (nxdk + SDL3, tools/nxdk_app.mk): the console's disks over the network (oxos_ftp.cpp). `make -C kernel oxos_ftp`; builds
# in build/apps/oxos_ftp; runs from a disc (oxos_ftp.iso), from the disk (C:\OXOS\apps\oxos_ftp.xbe in the release zip), or sent with netboot
# (tools/netboot/netboot.py IMAGE --title build/apps/oxos_ftp/bin --title-name ftp).
XBE_TITLE = OXOS\ FTP\ server
GEN_XISO = oxos_ftp.iso
APP_SOURCES = oxos_ftp.cpp
NXDK_NET = y
include $(dir $(lastword $(MAKEFILE_LIST)))../../tools/nxdk_app.mk
