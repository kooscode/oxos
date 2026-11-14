# FTP server (nxdk + SDL3, tools/nxdk_app.mk): the console's disks over the network (ftp_server.cpp).
# `make -C kernel ftp_server`; builds in build/tools/ftp_server; runs from a disc (ftp_server.iso), from the disk, or
# sent with netboot (tools/netboot/netboot.py IMAGE --title build/tools/ftp_server/bin --title-name ftp).
XBE_TITLE = OXOS\ FTP\ server
GEN_XISO = ftp_server.iso
APP_SOURCES = ftp_server.cpp
NXDK_NET = y
include $(dir $(lastword $(MAKEFILE_LIST)))../../tools/nxdk_app.mk
