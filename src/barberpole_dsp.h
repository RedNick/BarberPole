// Barber Pole DSP core: a DJ filter plus an endless rising/falling notch sweep, modelled on
// Traktor's Barber Pole Mixer FX.
//
// Plain C++11 with no VirtualDJ dependencies, so it can be tested on any platform.
//
// One bipolar knob ("Color", 0..1):
//   centre (with a small dead zone) : no effect at all; the output is bit-identical to the input
//   turn right : high-pass filter rises, notches climb forever  (build-ups)
//   turn left  : low-pass filter falls,  notches fall forever   (breakdowns)
//
// How the endless sweep works (the Shepard / Risset illusion, applied as a filter):
//   kNotchCount notches, exactly one octave apart, glide up (or down) together like a phaser.
//   Each notch's depth follows a half-sine window over the sweep's log-frequency range, so
//   notches fade in at one end and out at the other. When one leaves the top, another is
//   already rising from the bottom, so the motion never ends and never jumps. Two notch
//   stages in series double the depth, and resonant peaks half an octave between the notches
//   balance the level, as Traktor's does.
//
// The constants marked "matched to Traktor" were fitted to recordings of Traktor's effect
// made with tools/knob_sequencer.cpp and measured with tools/compare_recordings.cpp.

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
// State-variable filter, "topology-preserving transform" form (Zavalishin / Cytomic).
// Gives low-, band- and high-pass outputs at once and stays stable while its cutoff is
// being swept quickly, which the notches do all the time.
struct StateVariableFilter {
    double bandIntegrator = 0.0;   // the two integrator states
    double lowIntegrator = 0.0;

    void reset() { bandIntegrator = lowIntegrator = 0.0; }

    // Values this small are inaudible; clearing them keeps them out of the slow denormal range.
    void flush() {
        if (std::fabs(bandIntegrator) < 1e-20) bandIntegrator = 0.0;
        if (std::fabs(lowIntegrator) < 1e-20) lowIntegrator = 0.0;
    }

    // Filter settings. tuning = tan(pi * cutoff / sampleRate), damping = 1 / Q. They are
    // worked out once whenever the cutoff or Q changes, rather than every sample, because
    // they need a division.
    struct Coefficients { double tuning, damping, norm, tuningNorm; };
    static Coefficients coefficients(double tuning, double damping) {
        const double norm = 1.0 / (1.0 + tuning * (tuning + damping));
        return { tuning, damping, norm, tuning * norm };
    }

    // One sample in; low- and high-pass out. The band-pass (unity gain at the cutoff) is
    // input - low - damping * high, so it isn't returned separately.
    inline void process(double input, const Coefficients &c, double &low, double &high) {
        const double inputMinusLow = input - lowIntegrator;
        const double bandOut = c.norm * bandIntegrator + c.tuningNorm * inputMinusLow;
        const double lowOut = lowIntegrator + c.tuning * bandOut;
        bandIntegrator = 2.0 * bandOut - bandIntegrator;
        lowIntegrator = 2.0 * lowOut - lowIntegrator;
        low = lowOut;
        high = input - c.damping * bandOut - lowOut;
    }
};

// ---------------------------------------------------------------------------
// N state-variable band-passes stored side by side (structure of arrays), so the compiler
// can update several at once with SIMD. Same maths and rounding as StateVariableFilter.
template <int N>
struct BandPassBank {
    double bandIntegrator[N] = {}, lowIntegrator[N] = {};
    double norm[N] = {}, tuningNorm[N] = {}, tuning[N] = {}, weight[N] = {};

    void reset(int n) { bandIntegrator[n] = lowIntegrator[n] = 0.0; }
    void resetAll() { for (int n = 0; n < N; ++n) reset(n); }
    void flush() {
        for (int n = 0; n < N; ++n) {
            if (std::fabs(bandIntegrator[n]) < 1e-20) bandIntegrator[n] = 0.0;
            if (std::fabs(lowIntegrator[n]) < 1e-20) lowIntegrator[n] = 0.0;
        }
    }
    // weight scales this filter's band-pass output (the notch/peak fade window)
    void setCoefficients(int n, double tuningIn, double damping, double weightIn) {
        const auto c = StateVariableFilter::coefficients(tuningIn, damping);
        norm[n] = c.norm; tuningNorm[n] = c.tuningNorm; tuning[n] = c.tuning; weight[n] = weightIn;
    }
    // Feeds the same input to every filter and returns the sum of weight * band * damping.
    inline double process(double input, double damping) {
        double weightedBand[N];
        for (int n = 0; n < N; ++n) {   // independent filters: vectorisable
            const double inputMinusLow = input - lowIntegrator[n];
            const double bandOut = norm[n] * bandIntegrator[n] + tuningNorm[n] * inputMinusLow;
            const double lowOut = lowIntegrator[n] + tuning[n] * bandOut;
            bandIntegrator[n] = 2.0 * bandOut - bandIntegrator[n];
            lowIntegrator[n] = 2.0 * lowOut - lowIntegrator[n];
            weightedBand[n] = weight[n] * bandOut * damping;
        }
        double sum = 0.0;
        for (int n = 0; n < N; ++n) sum += weightedBand[n];   // summed in order, for repeatable rounding
        return sum;
    }
};

// ---------------------------------------------------------------------------
// While alive, the CPU treats denormal (extremely small) numbers as zero. On x86 they are
// up to ~100x slower, and filter states decay into that range when the music stops.
// ARM (Apple Silicon) handles them at full speed, so nothing is needed there.
struct DenormalGuard {
#ifdef BARBERPOLE_X86
    unsigned int savedCsr;
    DenormalGuard() : savedCsr(_mm_getcsr()) { _mm_setcsr(savedCsr | 0x8040); }   // FTZ | DAZ bits
    ~DenormalGuard() { _mm_setcsr(savedCsr); }
#endif
};

// ---------------------------------------------------------------------------
class BarberPole {
public:
    // --- Sweep layout
    static constexpr int    kNotchCount   = 8;      // one per octave, so the sweep covers 8 octaves
    static constexpr double kSweepLowHz   = 45.0;   // bottom of the sweep range (top = 45 Hz * 2^8 = 11.5 kHz)
    // Knob travel around centre that counts as "off": +-3 % (0.06 of the -1..1 range), so a
    // jittery controller resting at 12 o'clock leaves the audio untouched.
    static constexpr double kDeadZone     = 0.06;

    // --- DJ filter range (24 dB/oct), matched to Traktor. Even at full turn it keeps the
    // mids, so the sweep stays audible (no full cut at either end).
    static constexpr double kHighPassStartHz = 28.0;     // just past the dead zone
    static constexpr double kHighPassEndHz   = 640.0;    // at full right
    static constexpr double kLowPassStartHz  = 12000.0;  // just past the dead zone
    static constexpr double kLowPassEndHz    = 900.0;    // at full left

    // --- Sweep speed relative to the Speed slider, matched to Traktor: at the default Speed
    // it rises at 2.8 and falls at 1.8 beats per octave (2 beats/oct times these factors).
    static constexpr double kRisingSpeedFactor  = 1.4;
    static constexpr double kFallingSpeedFactor = 0.9;

    // --- Level, matched to Traktor
    static constexpr double kPeakGain = 1.5;   // resonant peak height: 1.5 = about +8 dB at full effect
    // Level trim in dB = start + curve * turn^2, so the level keeps falling as the knob turns
    // further: about -5 dB at full right and -3.5 dB at full left overall.
    static constexpr double kRightTrimDb = -0.3, kRightTrimCurveDb = -0.5;
    static constexpr double kLeftTrimDb  = -0.2, kLeftTrimCurveDb  = -0.5;

    BarberPole() { prepare(44100); }

    // Call before processing and whenever the sample rate changes. Clears all filter state.
    void prepare(int sampleRate) {
        sampleRate_ = sampleRate > 0 ? sampleRate : 44100;
        // one-pole smoothing with a 25 ms time constant, so knob moves never click
        smoothing_ = 1.0 - std::exp(-1.0 / (0.025 * sampleRate_));
        reset();
        setParams(0.5f, speed_, resonance_, 0.0);
        sweepAmount_ = sweepAmountTarget_;
        filterMix_ = filterMixTarget_;
        turn_ = turnTarget_;
    }

    // Clears filter state and the sweep position (e.g. when the deck starts playing).
    void reset() {
        for (int ch = 0; ch < 2; ++ch) {
            djFilterStage1_[ch].reset();
            djFilterStage2_[ch].reset();
            notchStage1_[ch].resetAll();
            peakFilters_[ch].resetAll();
            notchStage2_[ch].resetAll();
        }
        sweepPhase_ = 0.0;
        coefficientCounter_ = 0;
        direction_ = targetDirection_;
        djCachedTurn_ = -1.0;   // force the DJ filter coefficients to be recomputed
    }

    // color 0..1 (0.5 = off), speed 0..1, resonance 0..1.
    // beatsPerSecond: song tempo (0 = unknown, which falls back to 2 beats/s = 120 BPM).
    // Cheap enough to call once per audio block; changes are smoothed inside process().
    void setParams(float color, float speed, float resonance, double beatsPerSecond) {
        speed_ = clamp01(speed);
        resonance_ = clamp01(resonance);
        const double bipolar = ((double)clamp01(color) - 0.5) * 2.0;   // -1 (left) .. +1 (right)
        const double fromCentre = std::fabs(bipolar);
        // "turn": 0 at the edge of the dead zone .. 1 at full turn
        double turn = (fromCentre - kDeadZone) / (1.0 - kDeadZone);
        turn = turn < 0.0 ? 0.0 : (turn > 1.0 ? 1.0 : turn);
        knobActive_ = turn > 0.0;
        // a new side is applied in process() once the old side has faded out
        if (knobActive_) targetDirection_ = bipolar > 0.0 ? 1.0 : -1.0;
        turnTarget_ = turn;

        // The sweep fades in over the first third of the turn (smoothstep), then stays full.
        const double sweepFade = (turn * 3.0 > 1.0) ? 1.0 : turn * 3.0;
        sweepAmountTarget_ = sweepFade * sweepFade * (3.0 - 2.0 * sweepFade);
        // The DJ filter crossfades in over the first 10 % of the turn, so there is no jump
        // from "untouched" to "filtered" as the knob leaves the dead zone.
        const double filterFade = (turn * 10.0 > 1.0) ? 1.0 : turn * 10.0;
        filterMixTarget_ = filterFade;

        // Sweep rate in octaves per second, synced to the tempo. It doesn't depend on the knob.
        const double tempo = beatsPerSecond > 0.2 && beatsPerSecond < 10.0 ? beatsPerSecond : 2.0;
        sweepRateTarget_ = tempo / beatsPerOctave(speed_, targetDirection_ > 0.0);
    }

    // Processes an interleaved stereo float buffer of frameCount frames, in place.
    void process(float *buffer, int frameCount) {
        if (!knobActive_ && sweepAmount_ < 1e-6 && filterMix_ < 1e-6) {   // fully off: untouched
            sweepAmount_ = 0.0;
            filterMix_ = 0.0;
            turn_ = 0.0;
            return;
        }
        DenormalGuard noDenormals;   // restores the host's setting on return
        const double samplePeriod = 1.0 / (double)sampleRate_;
        const double maxFilterHz = 0.45 * sampleRate_;              // keep tan() well below Nyquist
        const double sweepOctaves = (double)kNotchCount;
        for (int i = 0; i < frameCount; ++i) {
            // Crossing from one side to the other: fade the old side out first, then switch
            // filter type and sweep direction, then fade the new side in.
            const bool switching = targetDirection_ != direction_;
            if (switching && sweepAmount_ < 1e-3 && filterMix_ < 1e-3) {
                direction_ = targetDirection_;
                sweepPhase_ = 0.0;
                coefficientCounter_ = 0;
            }
            const bool fadingOut = targetDirection_ != direction_;
            sweepAmount_ += ((fadingOut ? 0.0 : sweepAmountTarget_) - sweepAmount_) * smoothing_;
            filterMix_   += ((fadingOut ? 0.0 : filterMixTarget_)   - filterMix_)   * smoothing_;
            turn_        += ((fadingOut ? 0.0 : turnTarget_)        - turn_)        * smoothing_;
            sweepRate_ += (sweepRateTarget_ - sweepRate_) * smoothing_;

            // --- DJ filter cutoff: exponential over the turn, so each bit of turn moves it by
            // the same number of octaves (the low-pass eases out a little toward full left,
            // like Traktor's).
            if (turn_ != djCachedTurn_ || direction_ != djCachedDirection_) {
            djCachedTurn_ = turn_; djCachedDirection_ = direction_;
            double cutoffHz;
            if (direction_ < 0) cutoffHz = kLowPassStartHz * std::pow(kLowPassEndHz / kLowPassStartHz, std::pow(turn_, 0.7));
            else                cutoffHz = kHighPassStartHz * std::pow(kHighPassEndHz / kHighPassStartHz, turn_);
            if (cutoffHz > maxFilterHz) cutoffHz = maxFilterHz;
            const double cutoffTuning = std::tan(kPi * cutoffHz * samplePeriod);
            // Two 2nd-order stages make a 4th-order Butterworth (Q 0.54 and 1.31). The second
            // stage gets a small resonant bump mid-travel that tapers off toward the ends, so
            // sub-bass isn't pumped up near full turn.
            const double damping1 = 1.0 / 0.5412;
            const double damping2 = 1.0 / (1.3066 + 0.3 * std::sin(kPi * turn_));
            djCoefficients1_ = StateVariableFilter::coefficients(cutoffTuning, damping1);
            djCoefficients2_ = StateVariableFilter::coefficients(cutoffTuning, damping2);
            }

            // --- Sweep phase, in octaves. It wraps over the whole range (kNotchCount octaves),
            // not every octave, so each notch glides continuously and only jumps when it
            // individually wraps at the edge of the range, where its window is zero.
            sweepPhase_ += direction_ * sweepRate_ * samplePeriod;
            sweepPhase_ -= std::floor(sweepPhase_ / sweepOctaves) * sweepOctaves;

            // Notch/peak coefficients, refreshed every 8 samples (the sweep moves < 0.01 octave
            // in that time). The right channel is offset by half an octave for a little stereo
            // movement.
            if ((coefficientCounter_++ & 7) == 0) {
                const double notchQ = 1.5 + 4.0 * (double)resonance_;   // higher = narrower, more vocal
                notchDamping_ = 1.0 / notchQ;
                const double trimDb = direction_ > 0 ? kRightTrimDb + kRightTrimCurveDb * turn_ * turn_
                                                     : kLeftTrimDb + kLeftTrimCurveDb * turn_ * turn_;
                outputGain_ = std::pow(10.0, trimDb * sweepAmount_ / 20.0);
                // Every filter sits on a half-octave grid: left notches on the whole octaves,
                // left peaks and right notches half an octave up, right peaks a whole octave
                // up. So only 2 * kNotchCount distinct frequencies are needed.
                double slotPosition[2 * kNotchCount], slotTuning[2 * kNotchCount], slotWeight[2 * kNotchCount];
                for (int k = 0; k < 2 * kNotchCount; ++k) {
                    double position = (0.5 * (double)k + sweepPhase_) / sweepOctaves;   // 0..1
                    position -= std::floor(position);
                    double hz = kSweepLowHz * std::exp2(position * sweepOctaves);
                    if (hz > maxFilterHz) hz = maxFilterHz;
                    slotPosition[k] = position;
                    slotTuning[k] = std::tan(kPi * hz * samplePeriod);
                    slotWeight[k] = std::sin(kPi * position);   // half-sine window: 0 at both ends
                }
                for (int ch = 0; ch < 2; ++ch) {
                    for (int n = 0; n < kNotchCount; ++n) {
                        const int notchSlot = 2 * n + ch;
                        const int peakSlot = (2 * n + ch + 1) % (2 * kNotchCount);
                        // A filter that just wrapped (top -> bottom or bottom -> top) is silent
                        // (window = 0): clear its state so no stale energy follows it.
                        if (std::fabs(slotPosition[notchSlot] - notchLastPosition_[ch][n]) > 0.5) {
                            notchStage1_[ch].reset(n);
                            notchStage2_[ch].reset(n);
                        }
                        notchLastPosition_[ch][n] = slotPosition[notchSlot];
                        notchStage1_[ch].setCoefficients(n, slotTuning[notchSlot], notchDamping_, slotWeight[notchSlot]);
                        notchStage2_[ch].setCoefficients(n, slotTuning[notchSlot], notchDamping_, slotWeight[notchSlot]);
                        if (std::fabs(slotPosition[peakSlot] - peakLastPosition_[ch][n]) > 0.5) peakFilters_[ch].reset(n);
                        peakLastPosition_[ch][n] = slotPosition[peakSlot];
                        peakFilters_[ch].setCoefficients(n, slotTuning[peakSlot], notchDamping_, slotWeight[peakSlot]);
                    }
                }
            }
            const double notchDamping = notchDamping_;

            const double sweepAmount = sweepAmount_;
            const double filterMix = filterMix_;
            for (int ch = 0; ch < 2; ++ch) {
                const double input = (double)buffer[2 * i + ch];

                // DJ filter: two stages, high-pass when turning right, low-pass when left
                double low, high;
                djFilterStage1_[ch].process(input, djCoefficients1_, low, high);
                const double stage1Out = direction_ < 0 ? low : high;
                djFilterStage2_[ch].process(stage1Out, djCoefficients2_, low, high);
                const double filterOut = direction_ < 0 ? low : high;
                const double filtered = input + filterMix * (filterOut - input);

                // Notches: subtracting a unity-gain band-pass at a frequency carves a notch there
                // (each weighted by its fade window). Two identical stages in series double the
                // notch depth in dB.
                const double notchSum1 = notchStage1_[ch].process(filtered, notchDamping);
                const double afterNotches1 = filtered - sweepAmount * notchSum1;
                const double notchSum2 = notchStage2_[ch].process(afterNotches1, notchDamping);
                const double afterNotches = afterNotches1 - sweepAmount * notchSum2;

                // Peaks half way between the notches, like Traktor's: adding the band-passes
                // back makes bumps that balance the dips, so the sweep is deep without
                // scooping out the mids.
                const double peakSum = peakFilters_[ch].process(afterNotches, notchDamping);
                double output = (afterNotches + sweepAmount * kPeakGain * peakSum) * outputGain_;
                // No limiter: the effect only boosts single frequencies a few dB, and VirtualDJ's
                // float mix bus has headroom. Only guard against pathological values.
                if (!(std::fabs(output) < 8.0)) output = 0.0;
                buffer[2 * i + ch] = (float)output;
            }
        }
        // Clear inaudibly small filter states (silence after a track stops), so they never
        // reach the denormal range (also on CPUs where the guard above does nothing).
        for (int ch = 0; ch < 2; ++ch) {
            djFilterStage1_[ch].flush();
            djFilterStage2_[ch].flush();
            notchStage1_[ch].flush();
            peakFilters_[ch].flush();
            notchStage2_[ch].flush();
        }
    }

    // Beats per octave of sweep for a Speed slider value (0..1) and direction.
    // Speed 0 .. 1 maps to 8 .. 1/2 beats per octave (4x slower .. 4x faster than the
    // default 2), then the rising/falling factor is applied.
    static double beatsPerOctave(float speed, bool rising) {
        return 8.0 * std::pow(1.0 / 16.0, (double)speed) * (rising ? kRisingSpeedFactor : kFallingSpeedFactor);
    }

    // For UI labels / tests
    bool active() const { return knobActive_; }
    double direction() const { return targetDirection_; }   // +1 right, -1 left
    double amount() const { return turnTarget_; }            // 0..1 turn past the dead zone
    double octavesPerSecond() const { return sweepRateTarget_; }
    int sampleRate() const { return sampleRate_; }

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    int sampleRate_ = 44100;
    double smoothing_ = 0.0;   // per-sample one-pole smoothing coefficient

    // Filters, per channel ([2] = left, right)
    StateVariableFilter djFilterStage1_[2], djFilterStage2_[2];
    BandPassBank<kNotchCount> notchStage1_[2], notchStage2_[2], peakFilters_[2];

    // Notch and peak settings (refreshed every 8 samples)
    double notchLastPosition_[2][kNotchCount] = {}, peakLastPosition_[2][kNotchCount] = {};   // for wrap detection
    StateVariableFilter::Coefficients djCoefficients1_ = {}, djCoefficients2_ = {};
    double djCachedTurn_ = -1.0, djCachedDirection_ = 0.0;   // inputs the DJ coefficients were made from
    double notchDamping_ = 0.5;   // 1 / notch Q
    double outputGain_ = 1.0;     // level trim, linear
    unsigned coefficientCounter_ = 0;

    double sweepPhase_ = 0.0;     // octaves, 0 .. kNotchCount
    double direction_ = 1.0;      // side currently playing: +1 right (high-pass, rising), -1 left
    double targetDirection_ = 1.0;
    bool knobActive_ = false;     // knob outside the dead zone

    // Smoothed parameters (current value and target set by setParams)
    double sweepAmountTarget_ = 0.0, sweepAmount_ = 0.0;   // notch/peak depth, 0..1
    double filterMixTarget_ = 0.0, filterMix_ = 0.0;       // DJ filter crossfade, 0..1
    double turnTarget_ = 0.0, turn_ = 0.0;                 // knob turn past the dead zone, 0..1
    double sweepRateTarget_ = 0.5, sweepRate_ = 0.5;       // octaves per second
    float speed_ = 0.5f, resonance_ = 0.4f;
};

}  // namespace barberpole

#endif  // BARBERPOLE_DSP_H
