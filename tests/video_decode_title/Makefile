# Video decode benchmark title (nxdk + SDL3, tools/nxdk_app.mk; spec/oxos_internal/38 §4.1): how fast the console decodes the boot video format, with the
# player's .mpg reader and libmpeg2 (shared/video_player/video_player.mk). Builds in build/tests/video_decode_title; `make -C kernel video_decode_title`.
VIDEO_DECODE_TITLE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
XBE_TITLE = OXOS\ video\ decode\ test
include $(VIDEO_DECODE_TITLE_DIR)../../shared/video_player/video_player.mk
APP_SOURCES = video_decode_title.cpp mpeg_program_stream.cpp $(VIDEO_DECODER_SOURCES)
include $(VIDEO_DECODE_TITLE_DIR)../../tools/nxdk_app.mk
include $(VIDEO_DECODE_TITLE_DIR)../../shared/video_player/video_player.mk
