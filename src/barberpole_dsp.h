// Barber Pole - dual-pole filter + Shepard-tone sweep (Traktor-style Mixer FX).
//
// Plain C++11, no VirtualDJ dependencies, so it can be unit-tested on any platform.
//
// One bipolar knob ("Color", 0..1):
//   centre (with a small dead zone) : no effect at all - output is bit-identical to input
//   turn right : high-pass filter rises  +  resonances climb forever  (build-up)
//   turn left  : low-pass filter falls   +  resonances fall forever  (break-down)
//   the further you turn, the stronger the filter and the more intense the sweep.
//
// How the endless sweep works (Shepard / Risset illusion applied as a filter):
//   kVoices notches (band-pass subtracted from the signal) are spaced exactly one octave
//   apart and all glide upward (or downward) together, like a phaser. Each notch's depth
//   follows a raised-cosine window over the log-frequency range, so notches fade in at
//   the bottom and fade out at the top. When one leaves the top, another is already
//   rising from the bottom, so the motion never ends and never jumps.

#ifndef BARBERPOLE_DSP_H
#define BARBERPOLE_DSP_H

#include <cmath>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define BARBERPOLE_X86 1
#endif

namespace barberpole {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// TPT state-variable filter (Zavalishin / Cytomic). Stable under fast modulation.
struct Svf {
    double ic1 = 0.0, ic2 = 0.0;
    void reset() { ic1 = ic2 = 0.0; }
    // values this small are inaudible; clearing them keeps them out of the slow denormal range
    void flush() {
        if (std::fabs(ic1) < 1e-20) ic1 = 0.0;
        if (std::fabs(ic2) < 1e-20) ic2 = 0.0;
    }
    // g = tan(pi*fc/fs), k = 1/Q.  Outputs low, band, high.
    inline void process(double x, double g, double k, double &lp, double &bp, double &hp) {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + g * a1 * v3;      // band
        const double v2 = ic2 + g * v1;                // low
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
};

// ---------------------------------------------------------------------------
// While alive, the CPU treats denormal (extremely small) numbers as zero. On x86 they are
// up to ~100x slower, and filter states decay into that range when the music stops.
// ARM (Apple Silicon) handles them at full speed, so nothing is needed there.
struct DenormalGuard {
#ifdef BARBERPOLE_X86
    unsigned int saved;
    DenormalGuard() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040); }   // FTZ | DAZ
    ~DenormalGuard() { _mm_setcsr(saved); }
#endif
};

// ---------------------------------------------------------------------------
class BarberPole {
public:
    static constexpr int    kVoices     = 8;       // one per octave
    static constexpr double kLowHz      = 45.0;    // bottom of the Shepard range
    static constexpr double kDeadZone   = 0.06;    // +-3% of knob travel around centre = off
    // DJ filter: 24 dB/oct. Range measured from Traktor's Barber Pole: even at full turn it
    // keeps the mids, so the sweep stays audible (no full cut at either end).
    static constexpr double kHpfMinHz   = 28.0;    // high-pass just past the dead zone
    static constexpr double kHpfMaxHz   = 640.0;   // high-pass at full right
    static constexpr double kLpfMaxHz   = 12000.0; // low-pass just past the dead zone
    static constexpr double kLpfMinHz   = 900.0;   // low-pass at full left
    // Sweep speed relative to the Speed slider, also measured from Traktor: it climbs a
    // little slower than it falls (2.8 vs 1.8 beats per octave at the default Speed).
    static constexpr double kRiseFactor = 1.4;
    static constexpr double kFallFactor = 0.9;
    static constexpr double kMakeup     = 0.0;     // make-up gain at full effect
    static constexpr double kPeak       = 1.5;     // peak height (1.5 = +8 dB at full effect)
    // Level trim in dB, a + b * t^2, so the level keeps falling as the knob turns further
    // (matched to Traktor: about -5 dB at full right, -3.5 dB at full left).
    static constexpr double kTrimR0 = -0.3, kTrimR2 = -0.5;
    static constexpr double kTrimL0 = -0.2, kTrimL2 = -0.5;

    BarberPole() { prepare(44100); }

    void prepare(int sampleRate) {
        sr_ = sampleRate > 0 ? sampleRate : 44100;
        kSmooth_ = 1.0 - std::exp(-1.0 / (0.025 * sr_));   // 25 ms parameter smoothing
        reset();
        setParams(0.5f, speed_, reso_, 0.0);
        amt_ = amtTarget_; fmix_ = fmixTarget_; t_ = tTarget_;
    }

    void reset() {
        for (int ch = 0; ch < 2; ++ch) {
            filt_[ch].reset();
            filt2_[ch].reset();
            for (int v = 0; v < kVoices; ++v) { voice_[ch][v].reset(); voice2_[ch][v].reset(); voice3_[ch][v].reset(); }
        }
        phase_ = 0.0;
        coefCount_ = 0;
        dir_ = wantDir_;
    }

    // color 0..1 (0.5 = off). speed 0..1 (sweep rate). resonance 0..1.
    // beatsPerSec: song tempo (0 = unknown -> 2 beats/s, i.e. 120 BPM).
    void setParams(float color, float speed, float resonance, double beatsPerSec) {
        speed_ = clamp01(speed);
        reso_ = clamp01(resonance);
        const double c = ((double)clamp01(color) - 0.5) * 2.0;   // -1..1
        const double m = std::fabs(c);
        double t = (m - kDeadZone) / (1.0 - kDeadZone);
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        active_ = t > 0.0;
        if (active_) wantDir_ = c > 0.0 ? 1.0 : -1.0;  // applied once the old side has faded out
        tTarget_ = t;

        // Notches fade in over the first third of travel, then stay full.
        const double w = (t * 3.0 > 1.0) ? 1.0 : t * 3.0;
        amtTarget_ = w * w * (3.0 - 2.0 * w);
        // Filter crossfades in over the first ~10% of travel (so centre is truly clean).
        const double f = (t * 10.0 > 1.0) ? 1.0 : t * 10.0;
        fmixTarget_ = f;

        // Sweep rate in octaves per second. Speed slider picks beats per octave
        // (8, 4, 2, 1, 1/2 beats, times the rise/fall factor). It doesn't depend on the knob.
        const double bps = beatsPerSec > 0.2 && beatsPerSec < 10.0 ? beatsPerSec : 2.0;
        rateTarget_ = bps / beatsPerOctave(speed_, wantDir_ > 0.0);
    }

    // Interleaved stereo float buffer, nFrames frames.
    void process(float *buf, int nFrames) {
        if (!active_ && amt_ < 1e-6 && fmix_ < 1e-6) {   // fully off: untouched
            amt_ = 0.0; fmix_ = 0.0; t_ = 0.0;
            return;
        }
        DenormalGuard noDenormals;   // restores the host's setting on return
        const double invSr = 1.0 / (double)sr_;
        const double nyqGuard = 0.45 * sr_;
        const double kRange = (double)kVoices;               // octaves covered
        for (int i = 0; i < nFrames; ++i) {
            // Crossing from one side to the other: fade the old side out first,
            // then switch filter type and sweep direction, then fade the new side in.
            const bool switching = wantDir_ != dir_;
            if (switching && amt_ < 1e-3 && fmix_ < 1e-3) { dir_ = wantDir_; phase_ = 0.0; coefCount_ = 0; }
            const bool hold = wantDir_ != dir_;
            amt_  += ((hold ? 0.0 : amtTarget_)  - amt_)  * kSmooth_;
            fmix_ += ((hold ? 0.0 : fmixTarget_) - fmix_) * kSmooth_;
            t_    += ((hold ? 0.0 : tTarget_)    - t_)    * kSmooth_;
            rate_ += (rateTarget_ - rate_) * kSmooth_;

            // --- DJ filter cutoff: exponential over the knob's travel (the low-pass eases
            // out a little toward full left, like Traktor's).
            double fc;
            if (dir_ < 0) fc = kLpfMaxHz * std::pow(kLpfMinHz / kLpfMaxHz, std::pow(t_, 0.7));
            else          fc = kHpfMinHz * std::pow(kHpfMaxHz / kHpfMinHz, t_);
            if (fc > nyqGuard) fc = nyqGuard;
            const double gF = std::tan(kPi * fc * invSr);
            // 4th-order Butterworth (Q 0.54 + 1.31) with a small resonant bump mid-travel,
            // tapering off toward the ends so sub-bass isn't pumped up at full cut
            const double kF1 = 1.0 / 0.5412;
            const double kF2 = 1.0 / (1.3066 + 0.3 * std::sin(kPi * t_));

            // --- Shepard sweep phase (in octaves). It wraps over the whole kVoices-octave
            // range, NOT every octave, so each voice glides continuously and only jumps
            // when it individually wraps at the edge of the range (where it is silent).
            phase_ += dir_ * rate_ * invSr;
            phase_ -= std::floor(phase_ / kRange) * kRange;

            // voice coefficients, refreshed every 8 samples (the sweep moves < 0.01 octave
            // in that time). Shared by both channels; R is offset by half an octave for a
            // little stereo movement.
            if ((coefCount_++ & 7) == 0) {
                const double q = 1.5 + 4.0 * (double)reso_;   // notch Q (narrow = more vocal)
                kV_ = 1.0 / q;
                const double trimDb = dir_ > 0 ? kTrimR0 + kTrimR2 * t_ * t_ : kTrimL0 + kTrimL2 * t_ * t_;
                gain_ = (1.0 + kMakeup * amt_) * std::pow(10.0, trimDb * amt_ / 20.0);
                for (int ch = 0; ch < 2; ++ch) {
                    const double off = ch == 0 ? 0.0 : 0.5;
                    for (int v = 0; v < kVoices; ++v) {
                        double u = ((double)v + phase_ + off) / kRange;   // 0..1 position in range
                        u -= std::floor(u);
                        // a voice that just wrapped (top -> bottom or bottom -> top) is silent
                        // (window = 0); clear its state so no high-frequency energy follows it
                        if (std::fabs(u - lastU_[ch][v]) > 0.5) { voice_[ch][v].reset(); voice3_[ch][v].reset(); }
                        lastU_[ch][v] = u;
                        double f = kLowHz * std::exp2(u * kRange);
                        if (f > nyqGuard) f = nyqGuard;
                        gV_[ch][v] = std::tan(kPi * f * invSr);
                        wV_[ch][v] = std::sin(kPi * u);   // half-sine window: fades in/out at the range ends

                        // peak voice half an octave above each notch
                        double up = u + 0.5 / kRange;
                        up -= std::floor(up);
                        if (std::fabs(up - lastUP_[ch][v]) > 0.5) voice2_[ch][v].reset();
                        lastUP_[ch][v] = up;
                        double fp = kLowHz * std::exp2(up * kRange);
                        if (fp > nyqGuard) fp = nyqGuard;
                        gP_[ch][v] = std::tan(kPi * fp * invSr);
                        wP_[ch][v] = std::sin(kPi * up);
                    }
                }
            }
            const double kV = kV_;

            const double a = amt_;
            const double fm = fmix_;
            for (int ch = 0; ch < 2; ++ch) {
                const double x = (double)buf[2 * i + ch];
                double lp, bp, hp;
                filt_[ch].process(x, gF, kF1, lp, bp, hp);
                const double s1 = dir_ < 0 ? lp : hp;
                filt2_[ch].process(s1, gF, kF2, lp, bp, hp);
                const double filtered = dir_ < 0 ? lp : hp;
                const double y = x + fm * (filtered - x);

                // Shepard resonances on the filtered signal
                double sum = 0.0;
                for (int v = 0; v < kVoices; ++v) {
                    double vl, vb, vh;
                    voice_[ch][v].process(y, gV_[ch][v], kV, vl, vb, vh);
                    sum += wV_[ch][v] * vb * kV;     // unity-gain band-pass, window-weighted
                }
                // Subtracting the band-passes carves moving notches (barber-pole phaser);
                // a second identical stage in series doubles their depth in dB.
                const double z1 = y - a * sum;
                double sum3 = 0.0;
                for (int v = 0; v < kVoices; ++v) {
                    double vl, vb, vh;
                    voice3_[ch][v].process(z1, gV_[ch][v], kV, vl, vb, vh);
                    sum3 += wV_[ch][v] * vb * kV;
                }
                const double z = z1 - a * sum3;
                // Resonant peaks half way between the notches, like Traktor's: the dips and
                // bumps balance out, so the sweep is deep without scooping out the mids.
                double sum2 = 0.0;
                for (int v = 0; v < kVoices; ++v) {
                    double vl, vb, vh;
                    voice2_[ch][v].process(z, gP_[ch][v], kV, vl, vb, vh);
                    sum2 += wP_[ch][v] * vb * kV;
                }
                double out = (z + a * kPeak * sum2) * gain_;
                // No limiter: the effect never boosts more than a few dB, and VirtualDJ's
                // float mix bus has headroom. Only guard against pathological values.
                if (!(std::fabs(out) < 8.0)) out = 0.0;
                buf[2 * i + ch] = (float)out;
            }
        }
        // clear inaudibly small filter states (silence after a track stops), so they never
        // reach the denormal range (also on CPUs where the guard above does nothing)
        for (int ch = 0; ch < 2; ++ch) {
            filt_[ch].flush();
            filt2_[ch].flush();
            for (int v = 0; v < kVoices; ++v) { voice_[ch][v].flush(); voice2_[ch][v].flush(); voice3_[ch][v].flush(); }
        }
    }

    // beats per octave for a Speed slider value and direction (rising = true)
    static double beatsPerOctave(float speed, bool rising) {
        return 8.0 * std::pow(1.0 / 16.0, (double)speed) * (rising ? kRiseFactor : kFallFactor);
    }

    // For UI labels / tests
    bool active() const { return active_; }
    double direction() const { return wantDir_; }
    double amount() const { return tTarget_; }
    double octavesPerSecond() const { return rateTarget_; }
    int sampleRate() const { return sr_; }

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    int sr_ = 44100;
    Svf filt_[2], filt2_[2];
    Svf voice_[2][kVoices], voice2_[2][kVoices], voice3_[2][kVoices];   // notches, peaks, notches (2nd stage)
    double phase_ = 0.0;
    double gV_[2][kVoices] = {}, wV_[2][kVoices] = {}, lastU_[2][kVoices] = {}, kV_ = 0.5;
    double gP_[2][kVoices] = {}, wP_[2][kVoices] = {}, lastUP_[2][kVoices] = {};
    unsigned coefCount_ = 0;
    double gain_ = 1.0;
    double dir_ = 1.0, wantDir_ = 1.0;
    bool active_ = false;
    double kSmooth_ = 0.0;
    double amtTarget_ = 0.0, fmixTarget_ = 0.0, tTarget_ = 0.0, rateTarget_ = 0.5;
    double amt_ = 0.0, fmix_ = 0.0, t_ = 0.0, rate_ = 0.5;
    float speed_ = 0.5f, reso_ = 0.4f;
};

}  // namespace barberpole

#endif  // BARBERPOLE_DSP_H
