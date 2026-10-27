// The boot jingle's synthesizer (spec/oxos-internal/17-boot-jingle.md): integer arithmetic only, no kernel
// dependencies, so the same file builds into the kernel and into the host tool that renders it to a WAV file
// (tools/jingle).
#pragma once

namespace jingle {

constexpr unsigned RATE = 48000; // frames per second, 16-bit stereo

struct State;

unsigned StateSize();                 // bytes of working memory State needs (4-byte aligned)
unsigned Frames();                    // the jingle's length in frames
unsigned ClimaxMs();                  // when the build-up lands on the final chord (the splash is fully faded in)
void Init(State *s);                  // before the first Render
void Mute(State *s, unsigned mask);  // host tool: leave instruments out (bit 1 cello and bass, 2 drums, 3 reverb)
void Render(State *s, short *out, unsigned frames); // the next frames, interleaved left/right

} // namespace jingle
