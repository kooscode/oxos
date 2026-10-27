// The boot jingle (spec/oxos-internal/17-boot-jingle.md): an original six-second cue for a cello section, bass and
// drums, synthesized at 48 kHz in integer arithmetic. The instruments come in one after another: a swoosh builds, a
// cello joins, the section and the bass climb in power chords, and last the drums drive a crescendo into the final
// chord. It ends on a two-note coin chime. The instruments:
//   - cello: three slightly detuned sawtooth waves with a bow envelope and delayed vibrato, plus bow noise that
//     scratches once a cycle (the rasp), through a resonant low-pass (a state-variable filter) and a clipper;
//   - bass: a sawtooth and a sine an octave below, through a low-pass;
//   - drums: sine bodies whose pitch falls (kick, taiko, toms), filtered noise (snare), noise through a band-pass
//     sweeping upwards (the swoosh), six square waves at unrelated pitches through a high-pass (cymbals);
//   - the chime: a square wave, two notes a fourth apart;
//   - a stereo reverb (four damped comb filters and two all-pass filters per side).
#include "jingle.h"

#ifdef _WIN32 // the kernel: boot-only code, freed before the first title starts (spec/oxos-internal/18)
#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")
#endif

namespace jingle {
namespace {

using u32 = unsigned;
using i32 = int;
using i64 = long long;

constexpr u32 MS = RATE / 1000;
constexpr u32 LENGTH_MS = 5900, FADE_MS = 1200; // the last FADE_MS fade out

// --- Basics

// Phase increments (2^32 per cycle at RATE) of MIDI notes 120-131, rounded; lower octaves shift right.
constexpr u32 TOP_OCTAVE[12] = {749115498,  793660223,  840853716,  890853480,  943826385,  999949222,
                                1059409297, 1122405052, 1189146729, 1259857073, 1334772074, 1414141751};
u32 NoteInc(u32 note) { return TOP_OCTAVE[note % 12] >> (10 - note / 12); }
u32 HzInc(u32 hz) { return hz * 89478u; } // 2^32 / 48000 = 89478.5

i32 Q16(i64 a, i32 b) { return i32((a * b) >> 16); }

// sin(2π phase / 2^32) in Q15: a parabola, then one correction step (error below 0.1%).
i32 Sine(u32 phase) {
    i32 a = i32(phase) >> 16; // [-32768, 32767] for [-π, π)
    i32 abs = a < 0 ? -a : a;
    i32 y = i32((i64(a) * (32768 - abs)) >> 13);
    i32 ya = y < 0 ? -y : y;
    y += i32((i64((i32((i64(y) * ya) >> 15)) - y) * 7373) >> 15);
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

i32 Saw(u32 phase) { return i32(phase) >> 16; }
i32 Square(u32 phase) { return i32(phase) < 0 ? -32767 : 32767; }

struct Rng {
    u32 s;
    i32 Next() { // [-32768, 32767]
        s = s * 1664525u + 1013904223u;
        return i32(s) >> 16;
    }
};

// One-pole low-pass coefficient (Q16) for a cut-off in Hz: about 2π fc / RATE. Also the state-variable filter's
// frequency coefficient (2 sin(π fc / RATE)), to the same approximation.
i32 Coef(u32 hz) {
    u32 c = (hz * 8785u) >> 10;
    return c > 65535 ? 65535 : i32(c);
}

struct LowPass {
    i32 y = 0;
    i32 Run(i32 x, i32 a) { return y += Q16(x - y, a); }
};

struct HighPass {
    LowPass lp;
    i32 Run(i32 x, i32 a) { return x - lp.Run(x, a); }
};

// Chamberlin state-variable filter: f from Coef, q = 1/Q (Q16); low-pass and band-pass outputs.
struct Svf {
    i32 low = 0, band = 0;
    void Run(i32 x, i32 f, i32 q) {
        low += Q16(band, f);
        i32 high = x - low - Q16(band, q);
        band += Q16(high, f);
    }
};

// Exponential decay: multiply by Decay(ms) each sample (Q30) to fall to 1/e in ms milliseconds.
constexpr u32 ONE = 1u << 30;
u32 Decay(u32 ms) { return ONE - ONE / (ms * MS); }
u32 Step(u32 env, u32 k) { return u32((i64(env) * k) >> 30); }
i32 Env(i32 x, u32 env) { return i32((i64(x) * env) >> 30); } // env Q30

// Cubic soft clip, Q15: 1.5x - 0.5x^3 inside ±1, the limit outside.
i32 SoftClip(i32 x) {
    if (x >= 32768)
        return 32767;
    if (x <= -32768)
        return -32767;
    i32 x3 = i32((((i64(x) * x) >> 15) * x) >> 15);
    return (3 * x - x3) >> 1;
}

// (t / len)^2 in Q30, 1 from len on: rising envelopes.
u32 Rise(u32 t, u32 len) {
    u32 r = t >= len ? ONE : u32((i64(t) << 15) / len) << 15;
    return u32((i64(r) * r) >> 30);
}

// --- Cello and bass

struct Bow {
    bool on, bass;
    u32 ph1, ph2, ph3, inc, vib, t;
    i32 env, attack, release; // env Q16; per-sample steps
    u32 offAt;
    bool releasing;
    i32 amp;
    Svf f;

    void Start(u32 note, u32 len, i32 vel, u32 attackMs, bool isBass, u32 now) {
        if (!on)
            env = 0; // else from where the last note left it: no click
        on = true;
        bass = isBass;
        inc = NoteInc(note);
        t = 0;
        attack = i32(65536 / (attackMs * MS)) + 1;
        release = i32(65536 / ((isBass ? 200 : 350) * MS)) + 1;
        offAt = now + len * MS;
        releasing = false;
        amp = vel * (isBass ? 190 : 120);
    }
    i32 Next(Rng &rng, u32 now) {
        if (!on)
            return 0;
        if (now >= offAt)
            releasing = true;
        if (releasing) {
            env -= release;
            if (env <= 0) {
                env = 0;
                on = false;
                return 0;
            }
        } else if (env < 65536) {
            env += attack;
            if (env > 65536)
                env = 65536;
        }
        t++;
        if (bass) { // a sawtooth and a sine an octave below, warm
            ph1 += inc;
            ph2 += inc >> 1;
            i32 x = Saw(ph1) / 2 + Sine(ph2);
            f.Run(x, Coef(520), 52000);
            return Q16(Q16(f.low, amp), env);
        }
        // Vibrato: 5.5 Hz, coming in over the first 0.4 s, up to ±0.45%.
        vib += HzInc(5) + HzInc(1) / 2;
        i32 depth = t < 400 * MS ? i32(t * 295 / (400 * MS)) : 295;
        i32 dev = i32((i64(inc) * Sine(vib)) >> 15);
        u32 i = inc + u32(Q16(dev, depth));
        ph1 += i;
        ph2 += i + i / 280;
        ph3 += i - i / 230;
        i32 saw = (Saw(ph1) + Saw(ph2) + Saw(ph3)) / 3;
        // The rasp: bow noise that scratches once a cycle, as the string slips under the hair (loudest as the
        // sawtooth jumps), on top of a steady hiss.
        i32 slip = Saw(ph1) + 32768; // 0 .. 65535 over the cycle
        i32 scratch = Q16(rng.Next(), (slip * slip) >> 16) / 3 + rng.Next() / 24;
        // Resonant low-pass at 3.2 kHz, Q 2.5; the band-pass output adds body; clipping adds grit.
        f.Run(saw + scratch, Coef(3200), 26000);
        i32 y = SoftClip((f.low + f.band / 2) * 5 / 2);
        return Q16(Q16(y, amp), env);
    }
};

// --- Drums and effects

enum Inst : unsigned char {
    CELLO, CELLO_SLOW, CELLO_SWELL, CELLO_BOW, CELLO_SHORT, BASS, COIN, KICK, TAIKO, TOM_HI, TOM_MID, SNARE, CRASH, HAT, SWOOSH
};

struct DrumKind {
    u32 hzStart, hzEnd, glideMs, toneMs; // the body's pitch fall and decay (hzStart 0: no body)
    i32 tone;                            // body level (Q16)
    u32 noiseMs, noiseHz;                // noise decay and filter (low-pass, or high-pass when highPass)
    i32 noise;                           // noise level (Q16)
    bool highPass;
    u32 metalMs;                         // cymbal partials' decay (0: none)
    i32 metal;
    i32 pan;                             // Q16 left share
    i32 send;                            // reverb send (Q16)
};

const DrumKind KINDS[] = {
    // KICK
    {150, 45, 35, 230, 64000, 4, 3000, 22000, false, 0, 0, 32768, 3000},
    // TAIKO
    {100, 58, 90, 560, 64000, 45, 700, 30000, false, 0, 0, 32768, 16000},
    // TOM_HI
    {205, 140, 60, 260, 48000, 25, 1600, 14000, false, 0, 0, 42000, 14000},
    // TOM_MID
    {150, 98, 70, 320, 52000, 30, 1200, 15000, false, 0, 0, 24000, 14000},
    // SNARE
    {230, 185, 30, 70, 26000, 150, 1500, 34000, true, 0, 0, 32768, 14000},
    // CRASH
    {0, 0, 0, 0, 0, 900, 5000, 9000, true, 1100, 15000, 41000, 12000},
    // HAT
    {0, 0, 0, 0, 0, 30, 7000, 6000, true, 45, 9000, 22000, 4000},
    // SWOOSH: noise through a band-pass sweeping from 250 Hz to 7 kHz as it rises (envelope (t/len)^2), shimmer on top
    {0, 0, 0, 0, 0, 0, 0, 40000, false, 0, 4000, 32768, 22000},
};

// Partials of the cymbals: six square waves at unrelated pitches (Hz).
constexpr u32 METAL_HZ[6] = {349, 517, 628, 888, 918, 1360};

struct Hit {
    bool on;
    const DrumKind *k;
    bool swoosh;
    u32 t, len; // len: the swoosh's rise (samples)
    u32 ph, metal[6];
    u32 glide, toneEnv, noiseEnv, metalEnv; // Q30
    u32 glideK, toneK, noiseK, metalK;
    i32 vel;                                 // Q16
    LowPass nf, mf;
    Svf sweep;

    void Start(const DrumKind *kind, i32 velocity, u32 lenMs, bool isSwoosh) {
        on = true;
        k = kind;
        swoosh = isSwoosh;
        t = 0;
        len = lenMs * MS;
        ph = 0;
        glide = toneEnv = noiseEnv = metalEnv = ONE;
        glideK = k->glideMs ? Decay(k->glideMs) : ONE;
        toneK = k->toneMs ? Decay(k->toneMs) : 0;
        noiseK = k->noiseMs ? Decay(k->noiseMs) : 0;
        metalK = k->metalMs ? Decay(k->metalMs) : 0;
        vel = velocity * 516; // 127 -> 65532
        nf.y = mf.y = 0;
        sweep = Svf();
    }
    i32 Next(Rng &rng) {
        if (!on)
            return 0;
        i32 out = 0;
        if (swoosh) {
            u32 e = Rise(t, len);
            // The band-pass's centre rises with the envelope's square root (t / len): 250 Hz to 7 kHz.
            u32 fc = 250 + u32((i64(t >= len ? len : t) * 6750) / len);
            sweep.Run(rng.Next(), Coef(fc), 26000);
            out += Q16(Env(sweep.band, e), k->noise);
            i32 m = 0;
            for (u32 i = 0; i < 6; i++)
                m += Square(metal[i] += HzInc(METAL_HZ[i]) * 2) / 6;
            m -= mf.Run(m, Coef(6500));
            out += Q16(Env(m, e), k->metal);
            if (++t >= len)
                on = false;
            return Q16(out, vel);
        }
        if (k->hzStart) {
            u32 a = HzInc(k->hzEnd), b = HzInc(k->hzStart);
            ph += a + u32(((i64)(b - a) * glide) >> 30);
            out += Q16(Env(Sine(ph), toneEnv), k->tone);
            glide = Step(glide, glideK);
            toneEnv = Step(toneEnv, toneK);
        }
        if (k->noise) {
            i32 n = rng.Next();
            n = k->highPass ? n - nf.Run(n, Coef(k->noiseHz)) : nf.Run(n, Coef(k->noiseHz));
            out += Q16(Env(n, noiseEnv), k->noise);
            noiseEnv = Step(noiseEnv, noiseK);
        }
        if (k->metal) {
            i32 m = 0;
            for (u32 i = 0; i < 6; i++)
                m += Square(metal[i] += HzInc(METAL_HZ[i]) * 2) / 6;
            m -= mf.Run(m, Coef(6500));
            out += Q16(Env(m, metalEnv), k->metal);
            metalEnv = Step(metalEnv, metalK);
        }
        t++;
        if ((!k->hzStart || toneEnv < 2000) && (!k->noise || noiseEnv < 2000) && (!k->metal || metalEnv < 2000))
            on = false;
        return Q16(out, vel);
    }
};

// The coin chime: a square wave with a quick decay.
struct Chime {
    bool on;
    u32 ph, inc, env, k;
    void Start(u32 note, u32 ms) {
        on = true;
        inc = NoteInc(note);
        env = ONE;
        k = Decay(ms);
    }
    i32 Next() {
        if (!on)
            return 0;
        ph += inc;
        env = Step(env, k);
        if (env < 1000)
            on = false;
        return Env(Square(ph), env);
    }
};

// --- Reverb (Schroeder/Moorer: comb filters with damping in the feedback, then all-pass filters)

constexpr u32 COMB[4] = {1695, 1760, 1623, 1548}, ALLPASS[2] = {605, 480}, SPREAD = 25;
constexpr u32 COMB_ROOM = 1760 + SPREAD, ALLPASS_ROOM = 605 + SPREAD;

struct Comb {
    i32 buf[COMB_ROOM];
    u32 len, pos;
    i32 filt;
    i32 Run(i32 in) {
        i32 y = buf[pos];
        filt = y + Q16(filt - y, 19000); // damping 0.29
        buf[pos] = in + Q16(filt, 54000); // feedback 0.82
        if (++pos == len)
            pos = 0;
        return y;
    }
};

struct AllPass {
    i32 buf[ALLPASS_ROOM];
    u32 len, pos;
    i32 Run(i32 in) {
        i32 b = buf[pos];
        buf[pos] = in + (b >> 1);
        if (++pos == len)
            pos = 0;
        return b - in;
    }
};

struct Reverb {
    Comb comb[2][4];
    AllPass all[2][2];
    void Init() {
        for (u32 c = 0; c < 2; c++) {
            for (u32 i = 0; i < 4; i++)
                comb[c][i].len = COMB[i] + c * SPREAD;
            for (u32 i = 0; i < 2; i++)
                all[c][i].len = ALLPASS[i] + c * SPREAD;
        }
    }
    void Run(i32 in, i32 *l, i32 *r) {
        in >>= 2; // four combs add up
        for (u32 c = 0; c < 2; c++) {
            i32 s = 0;
            for (auto &cb : comb[c])
                s += cb.Run(in);
            for (auto &ap : all[c])
                s = ap.Run(s);
            *(c ? r : l) = s;
        }
    }
};

// --- The score: D minor, 120 beats a minute (an eighth is 250 ms).

struct Event {
    unsigned short ms;
    Inst inst;
    unsigned char note; // MIDI note
    unsigned short len; // ms
    unsigned char vel;
};

constexpr unsigned char D2 = 38, A2 = 45, D3 = 50;
constexpr unsigned short FINALE = 2600;

// A chord for the cello section, as a power chord: root, fifth and octave, the bass an octave below.
#define CHORD(at, root, len, vel)                                                                                    \
    {at, CELLO, root, len, vel}, {at, CELLO, (unsigned char)(root + 7), len, (unsigned char)(vel - 12)},             \
        {at, CELLO, (unsigned char)(root + 12), len, (unsigned char)(vel - 8)},                                      \
        {at, BASS, (unsigned char)(root - 12), len, (unsigned char)(vel - 10)}

// One lead-in and one climax, no beat: the swoosh rises while the cellos swell in one after another (each fading in
// over 1.7 s), into a single full chord with one hit that rings out to the end.
const Event SCORE[] = {
    // 1. The lead-in: the swoosh rises to the climax; the low D, its fifth and its octave swell in under it.
    {0, SWOOSH, 0, FINALE, 100},
    {150, CELLO_SWELL, D2, FINALE + 750, 100},
    {720, CELLO_SWELL, A2, FINALE - 620, 92},
    {1230, CELLO_SWELL, D3, FINALE - 1230, 96},
    // 2. The climax: the whole section and the bass on D, one taiko, kick and crash, ringing out.
    CHORD(FINALE, D2, LENGTH_MS - FINALE, 127),
    {FINALE, CELLO, D3 + 12, LENGTH_MS - FINALE, 100},
    {FINALE, KICK, 0, 0, 127},
    {FINALE, TAIKO, 0, 0, 127},
    {FINALE, CRASH, 0, 0, 127},
};
constexpr u32 SCORE_EVENTS = sizeof(SCORE) / sizeof(SCORE[0]);
static_assert(SCORE_EVENTS < 256, "State::order holds event numbers in bytes");

} // namespace

struct State {
    Bow cello[10];
    Hit hits[16];
    Chime chime;
    Reverb reverb;
    Rng rng;
    u32 t, next;                       // the sample, and the next score event (in order[])
    unsigned char order[SCORE_EVENTS]; // the score's events by time
    u32 mute;                          // host tool: instruments left out (bit 1 cello and bass, 2 drums, 3 reverb)
};

unsigned StateSize() { return (sizeof(State) + 3) & ~3u; }
unsigned Frames() { return LENGTH_MS * MS; }
unsigned ClimaxMs() { return FINALE; }
void Mute(State *s, unsigned mask) { s->mute = mask; }

void Init(State *s) {
    auto *p = reinterpret_cast<unsigned char *>(s);
    for (u32 i = 0; i < sizeof(State); i++)
        p[i] = 0;
    s->rng.s = 0xC0FFEE;
    s->reverb.Init();
    for (u32 i = 0; i < SCORE_EVENTS; i++) { // the score is written by part: sort it by time (stable)
        u32 j = i;
        for (; j > 0 && SCORE[s->order[j - 1]].ms > SCORE[i].ms; j--)
            s->order[j] = s->order[j - 1];
        s->order[j] = (unsigned char)i;
    }
}

namespace {

void Dispatch(State *s, const Event &e) {
    switch (e.inst) {
    case CELLO:
    case CELLO_SLOW:
    case CELLO_SWELL:
    case CELLO_BOW:
    case CELLO_SHORT:
    case BASS: {
        Bow *b = &s->cello[0];
        for (auto &c : s->cello) // a free voice, else the one closest to its end
            if (!c.on) {
                b = &c;
                break;
            } else if (c.offAt < b->offAt) {
                b = &c;
            }
        u32 attack = e.inst == CELLO_SWELL ? 1700 : e.inst == CELLO_SLOW ? 1100 : e.inst == CELLO_BOW ? 220 : e.inst == CELLO_SHORT ? 12 : e.inst == BASS ? 8 : 50;
        b->Start(e.note, e.len, e.vel, attack, e.inst == BASS, s->t);
        break;
    }
    case COIN:
        s->chime.Start(e.note, e.len);
        break;
    default: {
        Hit *h = &s->hits[0];
        for (auto &x : s->hits)
            if (!x.on) {
                h = &x;
                break;
            } else if (x.t > h->t) {
                h = &x; // else the oldest
            }
        h->Start(&KINDS[e.inst - KICK], e.vel, e.len, e.inst == SWOOSH);
        break;
    }
    }
}

} // namespace

void Render(State *s, short *out, unsigned frames) {
    const u32 total = Frames(), fadeFrom = total - FADE_MS * MS;
    for (u32 f = 0; f < frames; f++, s->t++) {
        while (s->next < SCORE_EVENTS && SCORE[s->order[s->next]].ms * MS <= s->t)
            Dispatch(s, SCORE[s->order[s->next++]]);
        i32 l = 0, r = 0, send = 0;
        for (u32 i = 0; i < 10; i++) {
            Bow &b = s->cello[i];
            i32 v = b.Next(s->rng, s->t);
            if (s->mute & 2)
                v = 0;
            i32 pan = b.bass ? 32768 : i % 3 == 0 ? 46000 : i % 3 == 1 ? 21000 : 33000;
            l += Q16(v, pan);
            r += Q16(v, 65536 - pan);
            send += b.bass ? v >> 4 : v >> 2;
        }
        for (auto &h : s->hits) {
            if (!h.on)
                continue;
            i32 v = h.Next(s->rng);
            if (s->mute & 4)
                v = 0;
            l += Q16(v, h.k->pan);
            r += Q16(v, 65536 - h.k->pan);
            send += Q16(v, h.k->send);
        }
        {
            i32 v = Q16(s->chime.Next(), 5200);
            l += v;
            r += v;
            send += v >> 1;
        }
        i32 wl, wr;
        s->reverb.Run(send, &wl, &wr);
        if (s->mute & 8)
            wl = wr = 0;
        l += Q16(wl, 26000);
        r += Q16(wr, 26000);
        // Master: level, a soft limit, and a fade over the last FADE_MS (squared: it eases out).
        l = SoftClip(Q16(l, 21000));
        r = SoftClip(Q16(r, 21000));
        if (s->t >= total) {
            l = r = 0;
        } else if (s->t >= fadeFrom) {
            i32 g = i32(((total - s->t) << 16) / (FADE_MS * MS));
            g = Q16(g, g);
            l = Q16(l, g);
            r = Q16(r, g);
        }
        out[2 * f] = short(l);
        out[2 * f + 1] = short(r);
    }
}

} // namespace jingle
