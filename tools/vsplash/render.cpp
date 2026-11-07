// Draws a vector splash scene (tools/svg_splash.py) with the kernel's renderer and writes the start picture, the end
// picture and the half-way point of the cross-fade as PPM files; with --frames, every frame of the boot animation
// instead (OUT-0000.ppm ...: 25 per second, as kernel/src/hal/splash.cpp shows them, to 8 s), for a preview video
// (the jingle starts 2 s in): ffmpeg -framerate 25 -i OUT-%04d.ppm -itsoffset 2 -i jingle.wav ... (tools/jingle)
#include "../../kernel/src/hal/vsplash.h"

#include <cstdio>
#include <string>
#include <vector>

static bool WritePpm(const std::string &path, const std::vector<unsigned> &px) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%u %u\n255\n", vsplash::W, vsplash::H);
    for (unsigned p : px) {
        unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}

int main(int argc, char **argv) {
    bool frames = argc == 4 && std::string(argv[3]) == "--frames";
    if (argc != 3 && !frames) {
        fprintf(stderr, "usage: vsplash SCENE.bin OUT [--frames]  (writes OUT-start.ppm, OUT-end.ppm, OUT-mid.ppm,\n"
                        "       or with --frames the animation's frames OUT-0000.ppm ...)\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    std::vector<unsigned char> scene;
    for (int c; (c = fgetc(f)) != EOF;)
        scene.push_back((unsigned char)c);
    fclose(f);
    unsigned scratchBytes = 0, status = vsplash::Check(scene.data(), (unsigned)scene.size(), &scratchBytes);
    if (status) {
        fprintf(stderr, "%s: check %u failed\n", argv[1], status);
        return 1;
    }
    std::vector<unsigned> scratch(scratchBytes / 4 + 1), start(vsplash::W * vsplash::H), end(start.size()),
        mid(start.size());
    vsplash::Render(scene.data(), (unsigned)scene.size(), vsplash::START, start.data(), scratch.data());
    vsplash::Render(scene.data(), (unsigned)scene.size(), vsplash::END, end.data(), scratch.data());
    for (size_t i = 0; i < mid.size(); i++) {
        unsigned v = 0;
        for (unsigned sh = 0; sh < 24; sh += 8)
            v |= (((start[i] >> sh & 0xFF) + (end[i] >> sh & 0xFF)) / 2) << sh;
        mid[i] = v;
    }
    std::string out = argv[2];
    if (frames) {
        // The splash thread's timeline (kernel/src/hal/splash.cpp): 0.5 s from black to the start picture, which
        // stays until 1 s; the jingle starts and the picture cross-fades (smoothstep) to the end picture, there at the
        // jingle's climax 2.6 s later; held 1.4 s, as the jingle's chord dies away; then black (the title).
        const unsigned introMs = 500, logoMs = 1000, climaxMs = 2600, holdMs = 1400;
        std::vector<unsigned> frame(start.size());
        unsigned n = 0;
        for (unsigned ms = 0; ms < 8000; ms += 40, n++) {
            const unsigned shownMs = logoMs + climaxMs + holdMs;
            unsigned level = ms >= introMs ? 256 : ms * 256 / introMs, mix = 0;
            if (ms >= logoMs + climaxMs) {
                mix = 256;
            } else if (ms > logoMs) {
                unsigned u = (ms - logoMs) * 256 / climaxMs;
                mix = u * u * (768 - 2 * u) / 65536;
            }
            for (size_t i = 0; i < frame.size(); i++) {
                unsigned v = 0;
                for (unsigned sh = 0; sh < 24 && ms < shownMs; sh += 8) {
                    unsigned c = ((start[i] >> sh & 0xFF) * (256 - mix) + (end[i] >> sh & 0xFF) * mix) >> 8;
                    v |= (c * level >> 8) << sh;
                }
                frame[i] = v;
            }
            char name[32];
            snprintf(name, sizeof(name), "-%04u.ppm", n);
            if (!WritePpm(out + name, frame)) {
                perror(out.c_str());
                return 1;
            }
        }
        printf("%u frames: %s-0000.ppm ...\n", n, argv[2]);
        return 0;
    }
    if (!WritePpm(out + "-start.ppm", start) || !WritePpm(out + "-end.ppm", end) || !WritePpm(out + "-mid.ppm", mid)) {
        perror(out.c_str());
        return 1;
    }
    printf("scene %zu bytes, scratch %u bytes: %s-start.ppm, %s-end.ppm, %s-mid.ppm\n", scene.size(), scratchBytes,
           argv[2], argv[2], argv[2]);
    return 0;
}
