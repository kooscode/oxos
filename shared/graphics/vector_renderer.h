// The vector renderer (shared/graphics/vector_renderer.cpp; the scene format is in spec/oxos_internal/00_boot_splash.md §1.1): draws the shapes
// tools/svg_splash.py makes from an SVG into a 640x480 picture, once for the start of the boot splash's animation and once for its end. Integer arithmetic only
// and no kernel dependencies, so the same file builds into the kernel (boot-only code there), the host preview tool (tools/vsplash), the unit tests and
// oxos_trainers.xbe (kernel/extensions/oxos_trainers/app).
#pragma once

namespace oxos::graphics::vector_renderer {

constexpr unsigned W = 640, H = 480;

// The picture at the start (the fade-in reaches it) and at the end (the cross-fade reaches it).
enum Phase : unsigned { START = 1, END = 2 };

// Checks a scene (the bytes after the 20-byte "XKSPLASH" header, version 2): 0 if Render may draw it, else the number of the check that failed. *ScratchBytes
// is the working memory Render needs (4-byte aligned).
unsigned Check(const unsigned char *Scene, unsigned Size, unsigned *ScratchBytes);

// Draws a checked scene's picture for Phase into Out (W x H pixels, 0x00RRGGBB; outside the scene's area black).
void Render(const unsigned char *Scene, unsigned Size, Phase Phase, unsigned *Out, void *Scratch);

} // namespace oxos::graphics::vector_renderer
