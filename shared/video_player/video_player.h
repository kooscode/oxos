// The video player (spec/oxos_internal/38 §4): plays an .mpg file (MPEG-1 or MPEG-2 video, MP2 or MP3 sound) full screen at 640x480 with its sound, for the
// boot video (apps/oxos_boot_video) and later its preview in OXOS Setup (apps/oxos_settings). Apps only (nxdk): the decoders use the FPU, MMX and SSE.
//   - The picture: the middle 4:3 of a wider one (a 16:9 video loses its sides), scaled to 640x480 when it is not 480 lines high.
//   - Its time: each frame is shown at its time from the start, just after the screen's vertical blank; a late frame is shown at once and the next ones catch
//     up (the slowest frames, pictures without reference to others, take two to three frame times).
//   - The sound: through the AC'97 controller, polled (sound_output.h); sound other than 48 kHz stereo is converted to it.
#pragma once

#include <stdint.h>

namespace oxos::video_player {

// The largest file the player reads: it is read whole (the video part stays in memory while it plays).
constexpr uint32_t MAX_FILE_BYTES = 40 * 1024 * 1024;

// How a video ended.
enum class PLAY_RESULT {
    ENDED,      // played to its end
    STOPPED,    // ShouldStop asked for it
    UNREADABLE, // the file is missing, empty, larger than MAX_FILE_BYTES or cannot be read
    NO_VIDEO,   // the file holds no MPEG video the decoder understands
};

const char *DescribePlayResult(PLAY_RESULT Result);

// The screen's size, and of a still picture.
constexpr int SCREEN_WIDTH = 640;
constexpr int SCREEN_HEIGHT = 480;

// Sets up the 640x480 screen (the first call) and shows Picture on it (SCREEN_WIDTH x SCREEN_HEIGHT pixels, 0x00RRGGBB; null: black), e.g. a still picture
// while the app gets ready and reads the video. PlayVideoFile keeps it until its first frame.
void ShowStillPicture(const uint32_t *Picture);

// Plays the file at Path (a path the C library opens, e.g. "C:\\OXOS\\boot_videos\\intro.mpg") on the 640x480 screen (set up black unless
// ShowStillPicture did). The first picture and the sound start no earlier than LeadInMs after the screen was switched on, so a TV or an HDMI adapter that
// takes a moment to lock onto the new picture misses none of the video. ShouldStop is called about every millisecond while the player waits (also before
// the first picture); true stops the video (it may read the controllers). The screen is left black.
PLAY_RESULT PlayVideoFile(const char *Path, uint32_t LeadInMs, bool (*ShouldStop)(void *Context), void *Context);

} // namespace oxos::video_player
