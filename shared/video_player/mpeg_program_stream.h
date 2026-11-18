// MPEG program streams, the .mpg files of the boot video (spec/oxos_internal/38 §3): packs of PES packets, MPEG-1 or MPEG-2, split into one video and one
// sound stream for the decoders.
#pragma once

#include <stdint.h>

#include <vector>

namespace oxos::video_player {

// Splits a program stream in place: on return File holds its first video stream (0xE0) alone, and Sound its first MPEG audio stream (0xC0). The video is
// moved towards the start of File as it is found, so no second copy of a file the size of the video is needed. A file that is a video elementary stream (it
// starts with a sequence header, 00 00 01 B3) is left as it is, without sound.
void SplitProgramStream(std::vector<uint8_t> &File, std::vector<uint8_t> &Sound);

} // namespace oxos::video_player
