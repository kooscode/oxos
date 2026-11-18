# The build rules every OXOS console app shares (apps/*, tools/*, tests/*_title, kernel/extensions/*/app; nxdk and SDL3 with the text screen in
# tools/sdl_screen). An app's Makefile sets XBE_TITLE, APP_SOURCES (its source files, in its folder), GEN_XISO and nxdk's options (NXDK_NET, ...) when it
# needs them, then includes this file.
#
# The app builds in build/<its folder> at the repository root, e.g. build/apps/oxos_ftp: bin/default.xbe, the .iso, main.exe, the objects and libSDL3.lib.
# Its own folder keeps only sources: `make` there, or `make -C` it, runs the build in the build folder. nxdk and SDL3 come from third_party/ (`make -C kernel
# nxdk`).
NXDK_APP_DIR := $(abspath $(dir $(firstword $(MAKEFILE_LIST))))
OXOS_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
NXDK_APP_BUILD := $(OXOS_ROOT)/build/$(patsubst $(OXOS_ROOT)/%,%,$(NXDK_APP_DIR))

ifneq ($(abspath $(CURDIR)),$(NXDK_APP_BUILD))

# Started in the app's folder: the same make in the build folder.
$(or $(MAKECMDGOALS),all): nxdk_app_in_build_folder
	@:
nxdk_app_in_build_folder:
	@mkdir -p $(NXDK_APP_BUILD)
	$(MAKE) -C $(NXDK_APP_BUILD) -f $(NXDK_APP_DIR)/Makefile $(MAKECMDGOALS)
.PHONY: nxdk_app_in_build_folder

else

# In the build folder: nxdk's rules, with the sources found in the app's folder and tools/sdl_screen.
SRCS = $(APP_SOURCES) sdl_screen.cpp
vpath %.cpp $(NXDK_APP_DIR) $(OXOS_ROOT)/tools/sdl_screen
vpath %.c $(NXDK_APP_DIR)
NXDK_CXX = y
NXDK_DIR ?= $(OXOS_ROOT)/third_party/nxdk
include $(NXDK_DIR)/Makefile
include $(OXOS_ROOT)/tools/sdl_screen/sdl_screen.mk

endif
