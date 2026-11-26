# The video player for an nxdk app (shared/video_player; tools/nxdk_app.mk): libmpeg2 (third_party/libmpeg2, MPEG-1 and MPEG-2 video) and minimp3
# (third_party/minimp3, MP2 and MP3 sound), both fetched by `make -C kernel video_libraries`. An app's Makefile adds $(VIDEO_PLAYER_SOURCES) (the whole
# player) or $(VIDEO_DECODER_SOURCES) (libmpeg2 alone) to APP_SOURCES: it includes this file before tools/nxdk_app.mk (for the lists) and again after
# it (for the build folder's paths and flags).
VIDEO_PLAYER_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
VIDEO_DECODER_SOURCES := alloc.c cpu_accel.c cpu_state.c decode.c header.c idct.c idct_mmx.c motion_comp.c motion_comp_mmx.c slice.c rgb.c rgb_mmx.c
VIDEO_PLAYER_SOURCES := mpeg_program_stream.cpp sound_output.cpp video_player.cpp $(VIDEO_DECODER_SOURCES)

ifeq ($(abspath $(CURDIR)),$(NXDK_APP_BUILD))
LIBMPEG2_DIR := $(OXOS_ROOT)/third_party/libmpeg2
MINIMP3_DIR := $(OXOS_ROOT)/third_party/minimp3
vpath %.c $(LIBMPEG2_DIR)/libmpeg2 $(LIBMPEG2_DIR)/libmpeg2/convert
vpath %.cpp $(VIDEO_PLAYER_DIR)
# libmpeg2's own configuration (its config.h) for the console: x86 with MMX, the CPU's abilities read at start-up. -iquote puts it before nxdk's USB
# library's config.h, which is on the -I path first. Everything at -O2: nxdk compiles without optimisation, and the decoders must keep up with the video.
CFLAGS += -O2 -iquote $(VIDEO_PLAYER_DIR)/libmpeg2_config -I$(LIBMPEG2_DIR)/include -I$(LIBMPEG2_DIR)/libmpeg2 -Wno-everything
CXXFLAGS += -O2 -std=c++23 -I$(OXOS_ROOT)/shared -I$(LIBMPEG2_DIR)/include -I$(MINIMP3_DIR)
endif
