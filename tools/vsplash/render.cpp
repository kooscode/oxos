// Draws a vector splash scene (tools/svg_splash.py) with the kernel's renderer and writes its start and end pictures as PPM files (OUT-start.ppm,
// OUT-end.ppm). The boot splash shows the end picture (kernel/src/boot_splash/splash.cpp); the trainer menu's logo uses both.
#include <graphics/vector_renderer.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace oxos::graphics; // vector_renderer::, the kernel's vector renderer (shared/graphics)

static bool WritePpm(const std::string &path, const std::vector<unsigned> &px) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%u %u\n255\n", vector_renderer::W, vector_renderer::H);
    for (unsigned p : px) {
        unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: vsplash SCENE.bin OUT  (writes OUT-start.ppm and OUT-end.ppm)\n");
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
    unsigned scratchBytes = 0, status = vector_renderer::Check(scene.data(), (unsigned)scene.size(), &scratchBytes);
    if (status) {
        fprintf(stderr, "%s: check %u failed\n", argv[1], status);
        return 1;
    }
    std::vector<unsigned> scratch(scratchBytes / 4 + 1), start(vector_renderer::W * vector_renderer::H), end(start.size());
    vector_renderer::Render(scene.data(), (unsigned)scene.size(), vector_renderer::START, start.data(), scratch.data());
    vector_renderer::Render(scene.data(), (unsigned)scene.size(), vector_renderer::END, end.data(), scratch.data());
    std::string out = argv[2];
    if (!WritePpm(out + "-start.ppm", start) || !WritePpm(out + "-end.ppm", end)) {
        perror(argv[2]);
        return 1;
    }
    return 0;
}
