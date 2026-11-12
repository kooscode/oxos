# FTP server (nxdk + SDL3): the console's disks over the network (ftp_server.cpp). Built with nxdk and SDL3
# (../../third_party/nxdk and nxdk-sdl3, fetched by `make -C kernel nxdk`; the screen is ../sdl_screen):
# `make -C kernel ftp_server`; runs from a disc (ftp_server.iso), from the disk, or sent with netboot
# (tools/netboot/netboot.py IMAGE --title tools/ftp_server/bin --title-name ftp).
XBE_TITLE = OXOS\ FTP\ server
GEN_XISO = ftp_server.iso
SRCS = $(CURDIR)/ftp_server.cpp $(CURDIR)/../sdl_screen/sdl_screen.cpp
NXDK_CXX = y
NXDK_NET = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
include $(CURDIR)/../sdl_screen/sdl_screen.mk
