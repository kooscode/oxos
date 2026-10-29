# FTP server (nxdk): the console's disks over the network (ftp_server.cpp). Built with nxdk
# (../../third_party/nxdk): `make -C kernel ftp-server`; runs from a disc (ftp-server.iso), from the disk, or sent
# with netboot (tools/netboot/netboot.py IMAGE --title tools/ftp-server/bin --title-name ftp).
XBE_TITLE = OXOS\ FTP\ server
GEN_XISO = ftp-server.iso
SRCS = $(CURDIR)/ftp_server.cpp
NXDK_CXX = y
NXDK_NET = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
