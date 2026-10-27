// Renders the boot jingle (kernel/src/hal/jingle.cpp, the kernel's own synthesizer) to a WAV file and prints its
// peak and loudness per half second: make -C tools/jingle, then tools/jingle/bin/jingle OUT.wav
#include "../../kernel/src/hal/jingle.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: jingle OUT.wav [MUTE]  (MUTE: bit 1 cello and bass, 2 drums, 3 reverb)\n");
        return 2;
    }
    std::vector<unsigned char> work(jingle::StateSize());
    auto *state = reinterpret_cast<jingle::State *>(work.data());
    std::vector<short> pcm(2 * jingle::Frames());
    auto start = std::chrono::steady_clock::now();
    jingle::Init(state);
    if (argc == 3)
        jingle::Mute(state, unsigned(std::strtoul(argv[2], nullptr, 0)));
    for (unsigned at = 0; at < jingle::Frames(); at += 2048) {
        unsigned n = jingle::Frames() - at < 2048 ? jingle::Frames() - at : 2048;
        jingle::Render(state, pcm.data() + 2 * at, n);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    std::FILE *f = std::fopen(argv[1], "wb");
    if (!f)
        return 1;
    auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::uint32_t bytes = std::uint32_t(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f), u32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(2), u32(jingle::RATE), u32(jingle::RATE * 4), u16(4), u16(16);
    std::fwrite("data", 1, 4, f), u32(bytes);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);

    std::printf("%u frames (%.2f s), state %u bytes, rendered in %.1f ms\n", jingle::Frames(),
                double(jingle::Frames()) / jingle::RATE, jingle::StateSize(), ms);
    for (unsigned at = 0; at < jingle::Frames(); at += jingle::RATE / 2) {
        int peak = 0;
        double sum = 0;
        unsigned n = 0;
        for (unsigned i = 2 * at; i < 2 * (at + jingle::RATE / 2) && i < pcm.size(); i++, n++) {
            peak = std::max(peak, std::abs(int(pcm[i])));
            sum += double(pcm[i]) * pcm[i];
        }
        std::printf("  %4.1f s  peak %6.1f dBFS  rms %6.1f dBFS\n", double(at) / jingle::RATE,
                    20 * std::log10(std::max(peak, 1) / 32768.0), 10 * std::log10(std::max(sum / n, 1.0) / (32768.0 * 32768.0)));
    }
    return 0;
}
