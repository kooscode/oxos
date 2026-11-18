# oxos_boot_video.xbe, the boot video player (nxdk + SDL3, tools/nxdk_app.mk; spec/oxos_internal/38): `make -C kernel oxos_boot_video`; builds in
# build/apps/oxos_boot_video. The player is shared/video_player (video_player.mk: libmpeg2 and minimp3), the settings parser shared/ini_file; the boot
# splash it shows before the video (kernel/splash/oxos_splash.svg) becomes a scene header at build time (tools/svg_splash.py), drawn by
# shared/graphics/vector_renderer.cpp.
OXOS_BOOT_VIDEO_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
XBE_TITLE = OXOS\ boot\ video
include $(OXOS_BOOT_VIDEO_DIR)../../shared/video_player/video_player.mk
APP_SOURCES = oxos_boot_video.cpp ini_file.cpp settings_ini.cpp launch_redirect_ini.cpp vector_renderer.cpp $(VIDEO_PLAYER_SOURCES)
include $(OXOS_BOOT_VIDEO_DIR)../../tools/nxdk_app.mk
include $(OXOS_BOOT_VIDEO_DIR)../../shared/video_player/video_player.mk

ifeq ($(abspath $(CURDIR)),$(NXDK_APP_BUILD))
vpath %.cpp $(OXOS_ROOT)/shared/ini_file $(OXOS_ROOT)/shared/graphics
CXXFLAGS += -I$(CURDIR)
oxos_boot_video.obj: oxos_splash.h
oxos_splash.h: $(OXOS_ROOT)/kernel/splash/oxos_splash.svg $(OXOS_ROOT)/tools/svg_splash.py
	python3 -I $(OXOS_ROOT)/tools/svg_splash.py $< $@
endif
