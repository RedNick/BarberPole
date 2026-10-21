// Speed benchmark for the DSP core (src/barberpole_dsp.h).
//
// Runs stereo noise through the effect in 128- or 512-frame blocks, the way VirtualDJ calls
// it, at several knob settings and sample rates, and prints the median of 7 runs. "% core"
// is the share of one CPU core one deck uses. The last column is a hash of every output
// sample: run it before and after a change, and if a hash changes, so did the sound.
// (Hashes can differ between compilers and maths libraries, so compare like with like.)
//
//   g++ -O2 -std=c++11 -I../src -o benchmark benchmark.cpp          (Linux/macOS/WSL)
//   cl /O2 /EHsc /I..\src benchmark.cpp                              (Windows, MSVC)

#include "barberpole_dsp.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using barberpole::BarberPole;

struct Case { const char *name; float color; float resonance; bool silence; bool knobMoving; };

// Returns nanoseconds per stereo frame; fills *hash with an FNV-1a hash of the output.
static double runCase(const Case &c, int sampleRate, int block, double seconds, uint64_t *hash)
{
    BarberPole fx;
    fx.prepare(sampleRate);
    const int frames = (int)(sampleRate * seconds);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    std::vector<float> buffer(2 * (size_t)frames);
    for (size_t i = 0; i < buffer.size(); ++i) buffer[i] = c.silence ? 0.0f : noise(rng);

    // 1 s of noise first, so the filters have settled (and, for the silence case,
    // the timed part is the decaying tail, where denormals used to cost CPU).
    std::vector<float> warmUp(2 * (size_t)block);
    for (int i = 0; i < sampleRate; i += block) {
        for (size_t j = 0; j < warmUp.size(); ++j) warmUp[j] = noise(rng);
        fx.setParams(c.color, 0.5f, c.resonance, 2.1);
        fx.process(warmUp.data(), block);
    }

    const auto start = std::chrono::steady_clock::now();
    int pos = 0;
    for (; pos + block <= frames; pos += block) {
        float color = c.color;
        if (c.knobMoving) color = (float)(0.5 + 0.5 * std::sin((double)pos / sampleRate * 2.0));
        fx.setParams(color, 0.5f, c.resonance, 2.1);
        fx.process(buffer.data() + 2 * (size_t)pos, block);
    }
    const auto end = std::chrono::steady_clock::now();

    if (hash) {
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < 2 * pos; ++i) {
            uint32_t bits;
            std::memcpy(&bits, &buffer[i], 4);
            h = (h ^ bits) * 1099511628211ull;
        }
        *hash = h;
    }
    return std::chrono::duration<double, std::nano>(end - start).count() / pos;
}

int main()
{
    const Case cases[] = {
        {"centre (off)",        0.50f, 0.4f, false, false},
        {"right 1/4",           0.64f, 0.4f, false, false},
        {"right full",          1.00f, 0.4f, false, false},
        {"left 1/2",            0.26f, 0.4f, false, false},
        {"left full, res 100%", 0.00f, 1.0f, false, false},
        {"right 1/2, silence",  0.75f, 0.4f, true,  false},
        {"knob moving",         0.50f, 0.4f, false, true},
    };
    const int sampleRates[] = {44100, 48000, 96000};
    const int blocks[] = {128, 512};
    const int runs = 7;
    const double seconds = 4.0;

    printf("%-22s %6s %6s %10s %8s  %s\n", "case", "rate", "block", "ns/frame", "% core", "output hash");
    for (int sampleRate : sampleRates)
        for (int block : blocks) {
            if (sampleRate != 44100 && block != 512) continue;   // 128-frame blocks at 44.1 kHz only
            for (const Case &c : cases) {
                std::vector<double> times;
                uint64_t hash = 0;
                for (int r = 0; r < runs; ++r) times.push_back(runCase(c, sampleRate, block, seconds, r == 0 ? &hash : nullptr));
                std::sort(times.begin(), times.end());
                const double median = times[runs / 2];
                printf("%-22s %6d %6d %10.1f %7.2f%%  %016llx\n", c.name, sampleRate, block, median,
                       median * sampleRate / 1e7, (unsigned long long)hash);
            }
        }
    return 0;
}
