// compare_recordings: compares two recordings of the knob_sequencer test (e.g. Traktor vs
// VirtualDJ) knob position by knob position.
//
// Windows only: decodes WAV / FLAC / MP3 etc. through Media Foundation, at each file's own rate.
//
// Usage:
//   compare_recordings <recording A> <recording B> [--log knob_sequence_log.csv] [--bpm 126]
//                      [--source barberpole_test_126bpm.wav] [--offset-a <s>] [--offset-b <s>]
//                      [--timeline]
//
// How the recordings are lined up with the timing log:
//   1. Each recording is followed through the source track (the test file both programs
//      played) by cross-correlation. That finds gaps, e.g. VirtualDJ's recorder skips silence.
//   2. The sequence start is found by fitting the whole knob pattern: right turns cut the
//      lows, left turns cut the highs. (--offset-a/-b override this: the recording time, in
//      seconds, where the sequence starts.)
//   3. The sequence's opening 8 s at centre is the "untouched" reference for that recording,
//      which cancels each program's own master level and EQ.
//
// Per knob position it reports, relative to that reference:
//   level    full-band level change (dB)
//   -6 dB    where the DJ filter's pass band starts (right) or ends (left)
//   sweep    notch/peak sweep speed in beats per octave (+ = rising, - = falling)
//   ripple   how deep the moving notches/peaks are (dB)
//   L/R      stereo correlation of the two channels (1 = mono)
// plus a table of the level change in octave bands.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static const double kPi = 3.14159265358979323846;

typedef std::complex<double> Complex;

// ---------------------------------------------------------------------------
// Decoding

struct Audio {
    std::string name;
    int sampleRate = 0;
    std::vector<float> left, right;   // mono files get the same samples in both
};

// Decodes any format Media Foundation can read into float samples at the file's own rate.
static bool decode(const std::string &path, Audio &audio) {
    audio.name = path;
    const int wideLength = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    std::wstring widePath(wideLength, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &widePath[0], wideLength);

    IMFSourceReader *reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(widePath.c_str(), nullptr, &reader))) {
        std::fprintf(stderr, "Could not open %s\n", path.c_str());
        return false;
    }
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    IMFMediaType *requestedType = nullptr;
    MFCreateMediaType(&requestedType);
    requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    requestedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    HRESULT hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, requestedType);
    requestedType->Release();
    if (FAILED(hr)) { std::fprintf(stderr, "Could not decode %s to float\n", path.c_str()); reader->Release(); return false; }
    IMFMediaType *actualType = nullptr;
    reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actualType);
    UINT32 channels = 0, sampleRate = 0;
    actualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    actualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    actualType->Release();
    if (channels < 1 || sampleRate < 8000) { std::fprintf(stderr, "Bad format in %s\n", path.c_str()); reader->Release(); return false; }
    audio.sampleRate = (int)sampleRate;
    for (;;) {
        DWORD flags = 0;
        IMFSample *sample = nullptr;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) break;
        if (sample) {
            IMFMediaBuffer *buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                BYTE *bytes = nullptr;
                DWORD byteCount = 0;
                buffer->Lock(&bytes, nullptr, &byteCount);
                const float *samples = (const float *)bytes;
                const size_t frames = byteCount / sizeof(float) / channels;
                for (size_t i = 0; i < frames; ++i) {
                    audio.left.push_back(samples[i * channels]);
                    audio.right.push_back(samples[i * channels + (channels > 1 ? 1 : 0)]);
                }
                buffer->Unlock();
                buffer->Release();
            }
            sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    reader->Release();
    return !audio.left.empty();
}

// ---------------------------------------------------------------------------
// FFT (in-place radix-2; the size must be a power of two)

static void fft(std::vector<Complex> &data) {
    const size_t n = data.size();
    // bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    // butterflies
    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * kPi / (double)length;
        const Complex rootStep(std::cos(angle), std::sin(angle));
        for (size_t start = 0; start < n; start += length) {
            Complex twiddle(1.0);
            for (size_t k = 0; k < length / 2; ++k) {
                const Complex even = data[start + k], odd = data[start + k + length / 2] * twiddle;
                data[start + k] = even + odd;
                data[start + k + length / 2] = even - odd;
                twiddle *= rootStep;
            }
        }
    }
}

// Inverse FFT through the forward one: ifft(x) = conj(fft(conj(x))) / n
static void ifft(std::vector<Complex> &data) {
    for (auto &value : data) value = std::conj(value);
    fft(data);
    for (auto &value : data) value = std::conj(value) / (double)data.size();
}

// ---------------------------------------------------------------------------
// Spectral analysis: the recording is cut into overlapping frames, and each frame's spectrum
// is summarised two ways: energy in 1/3-octave bands (for levels and filter edges) and a
// fine log-frequency grid in dB (for finding the moving notches).

static const int kFftSize = 8192;      // samples per frame (~170 ms at 48 kHz)
static const int kHopSize = 1024;      // frame step (frames overlap 8x)
static const int kBandCount = 30;      // 1/3-octave bands, 31 Hz .. 16 kHz
static const int kGridCount = 160;     // log-frequency grid, 1/24 octave, 100 Hz .. 10 kHz
static double bandCentreHz(int band) { return 1000.0 * std::pow(2.0, (band - 15) / 3.0); }
static double gridHz(int point) { return 100.0 * std::pow(2.0, point / 24.0); }

struct Spectra {
    int sampleRate = 0;
    double hopSeconds = 0;
    // per frame: band energies (linear) per channel, and the grid spectrum (dB) per channel
    std::vector<std::vector<double>> bandsLeft, bandsRight;
    std::vector<std::vector<float>> gridLeft, gridRight;
    // centre time of frame i, in seconds
    double timeOf(size_t frame) const { return (frame * (double)kHopSize + kFftSize / 2) / sampleRate; }
    // frame whose centre is at (or just before) a time
    size_t frameAt(double seconds) const {
        const double frame = (seconds * sampleRate - kFftSize / 2) / kHopSize;
        return frame < 0 ? 0 : (size_t)frame;
    }
};

// Power spectrum of one windowed frame starting at sample `start`.
static void powerSpectrum(const std::vector<float> &signal, size_t start, const std::vector<double> &window,
                          std::vector<double> &power) {
    std::vector<Complex> frame(kFftSize);
    for (int i = 0; i < kFftSize; ++i) frame[i] = (start + i < signal.size() ? signal[start + i] : 0.0f) * window[i];
    fft(frame);
    power.resize(kFftSize / 2 + 1);
    for (int bin = 0; bin <= kFftSize / 2; ++bin) power[bin] = std::norm(frame[bin]);
}

static void analyse(const Audio &audio, Spectra &spectra) {
    spectra.sampleRate = audio.sampleRate;
    spectra.hopSeconds = (double)kHopSize / audio.sampleRate;
    std::vector<double> hann(kFftSize);
    for (int i = 0; i < kFftSize; ++i) hann[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kFftSize);
    const double binHz = (double)audio.sampleRate / kFftSize;
    std::vector<double> power;
    for (size_t start = 0; start + kFftSize <= audio.left.size(); start += kHopSize) {
        for (int ch = 0; ch < 2; ++ch) {
            powerSpectrum(ch == 0 ? audio.left : audio.right, start, hann, power);
            std::vector<double> bands(kBandCount, 0.0);
            for (int band = 0; band < kBandCount; ++band) {
                const double lowEdge = bandCentreHz(band) * std::pow(2.0, -1.0 / 6.0), highEdge = bandCentreHz(band) * std::pow(2.0, 1.0 / 6.0);
                for (int bin = (int)std::ceil(lowEdge / binHz); bin <= kFftSize / 2 && bin * binHz < highEdge; ++bin) bands[band] += power[bin];
                bands[band] += 1e-20;   // never exactly zero, so dB stays finite
            }
            // grid: mean power over +-1/48 octave around each point, in dB
            std::vector<float> grid(kGridCount);
            for (int point = 0; point < kGridCount; ++point) {
                const double centreHz = gridHz(point);
                const double lowEdge = centreHz * std::pow(2.0, -1.0 / 24.0), highEdge = centreHz * std::pow(2.0, 1.0 / 24.0);
                double sum = 0.0;
                int count = 0;
                for (int bin = (int)std::ceil(lowEdge / binHz); bin <= kFftSize / 2 && bin * binHz < highEdge; ++bin) { sum += power[bin]; ++count; }
                if (count == 0) {   // narrower than one FFT bin: interpolate between bins
                    const double exactBin = centreHz / binHz;
                    const int lowerBin = (int)exactBin;
                    const double fraction = exactBin - lowerBin;
                    sum = power[lowerBin] * (1.0 - fraction) + power[lowerBin + 1] * fraction;
                    count = 1;
                }
                grid[point] = (float)(10.0 * std::log10(sum / count + 1e-20));
            }
            if (ch == 0) { spectra.bandsLeft.push_back(bands); spectra.gridLeft.push_back(grid); }
            else         { spectra.bandsRight.push_back(bands); spectra.gridRight.push_back(grid); }
        }
    }
}

static double toDb(double energy) { return 10.0 * std::log10(energy + 1e-30); }

// Mean band energies (left + right) over frames [firstFrame, endFrame).
static std::vector<double> meanBands(const Spectra &spectra, size_t firstFrame, size_t endFrame) {
    std::vector<double> mean(kBandCount, 0.0);
    endFrame = std::min(endFrame, spectra.bandsLeft.size());
    if (endFrame <= firstFrame) return mean;
    for (size_t frame = firstFrame; frame < endFrame; ++frame)
        for (int band = 0; band < kBandCount; ++band) mean[band] += spectra.bandsLeft[frame][band] + spectra.bandsRight[frame][band];
    for (int band = 0; band < kBandCount; ++band) mean[band] /= (double)(endFrame - firstFrame);
    return mean;
}

// Full-band energy, 40 Hz .. 12.5 kHz (leaves out the outermost bands, which are mostly noise).
static double totalEnergy(const std::vector<double> &bands) {
    double sum = 0.0;
    for (int band = 1; band < kBandCount - 1; ++band) sum += bands[band];
    return sum;
}

// ---------------------------------------------------------------------------
// One recording and how it lines up with the source track and the knob sequence.
//
// Three time bases: recording time (seconds into this file), source time (seconds into the
// test track that was played) and sequence time (seconds since the knob sequence started).

struct Recording {
    Audio audio;
    Spectra spectra;
    double playbackStart = 0;      // recording time where audio starts
    double firstKnobMove = -1;     // recording time where the knob first visibly moves (-1 = not found)
    double sequenceStart = 0;      // recording time where the knob sequence starts
    size_t referenceFirstFrame = 0, referenceEndFrame = 0;
    std::vector<double> reference;          // untouched band energies
    double referenceRippleVariance = 0;     // grid variance of untouched audio (noise floor of "ripple")

    // Source time = recording time + sourceOffset[k], from recording time mapTime[k] on.
    // A jump in the offset is a gap in the recording. Empty = no source given (offset 0).
    std::vector<double> mapTime, sourceOffset;
    double sequenceStartInSource = 0;

    double offsetAt(double recordingTime) const {
        if (mapTime.empty()) return 0.0;
        const size_t k = std::upper_bound(mapTime.begin(), mapTime.end(), recordingTime) - mapTime.begin();
        return sourceOffset[k == 0 ? 0 : k - 1];
    }
    double toSource(double recordingTime) const { return recordingTime + offsetAt(recordingTime); }
    double toRecording(double sourceTime) const {
        if (mapTime.empty()) return sourceTime;
        size_t k = 0;
        while (k + 1 < mapTime.size() && mapTime[k + 1] + sourceOffset[k + 1] <= sourceTime) ++k;
        // inside a gap, return the point where the recording resumes
        return std::min(sourceTime - sourceOffset[k], k + 1 < mapTime.size() ? mapTime[k + 1] : 1e9);
    }
    // recording time of a sequence time
    double atSequenceTime(double sequenceTime) const { return toRecording(sequenceStartInSource + sequenceTime); }
};

// Follows the recording through the source track: every second, cross-correlates ~1/3 s of
// the recording's left channel with the source around the expected position, and records
// the offset. The offset may only move forward (recorders skip, they don't repeat).
static void alignToSource(Recording &rec, const std::vector<float> &sourceIn, int sourceRate) {
    const Audio &audio = rec.audio;
    std::vector<float> source;   // source resampled to the recording's rate (linear interpolation)
    if (sourceRate == audio.sampleRate) source = sourceIn;
    else {
        source.resize((size_t)((double)sourceIn.size() * audio.sampleRate / sourceRate));
        for (size_t i = 0; i < source.size(); ++i) {
            const double exact = i * (double)sourceRate / audio.sampleRate;
            const size_t lower = (size_t)exact;
            const double fraction = exact - lower;
            source[i] = lower + 1 < sourceIn.size() ? (float)(sourceIn[lower] * (1.0 - fraction) + sourceIn[lower + 1] * fraction) : 0.0f;
        }
    }
    const int kChunk = 16384;   // samples of recording matched each time
    double offset = 0;
    bool found = false;
    size_t firstMatch = 0;
    for (double t = 0; (size_t)(t * audio.sampleRate) + kChunk < audio.left.size(); t += 1.0) {
        const size_t chunkStart = (size_t)(t * audio.sampleRate);
        double chunkEnergy = 0;
        for (int i = 0; i < kChunk; ++i) chunkEnergy += (double)audio.left[chunkStart + i] * audio.left[chunkStart + i];
        if (chunkEnergy > 1e-6 * kChunk) {   // skip silence: nothing to match
            // search offsets [offset - 1 s, offset + ~10 s] (before the first match: -30 s .. +17 s)
            const size_t searchSize = found ? (1u << 19) : (1u << 21);
            const double searchFrom = found ? offset - 1.0 : -30.0;
            const long long searchStart = std::max(0LL, (long long)chunkStart + (long long)(searchFrom * audio.sampleRate));
            if ((size_t)searchStart < source.size()) {
                // cross-correlation through the FFT
                std::vector<Complex> sourceSpectrum(searchSize), chunkSpectrum(searchSize);
                for (size_t i = 0; i < searchSize && searchStart + i < source.size(); ++i) sourceSpectrum[i] = source[searchStart + i];
                for (int i = 0; i < kChunk; ++i) chunkSpectrum[i] = audio.left[chunkStart + i];
                fft(sourceSpectrum);
                fft(chunkSpectrum);
                for (size_t i = 0; i < searchSize; ++i) sourceSpectrum[i] *= std::conj(chunkSpectrum[i]);
                ifft(sourceSpectrum);
                const std::vector<Complex> &correlation = sourceSpectrum;
                size_t bestLag = 0;
                double bestValue = -1e30;
                for (size_t lag = 0; lag + kChunk < searchSize && searchStart + lag + kChunk < source.size(); ++lag)
                    if (correlation[lag].real() > bestValue) { bestValue = correlation[lag].real(); bestLag = lag; }
                double sourceEnergy = 0;
                for (int i = 0; i < kChunk; ++i) sourceEnergy += (double)source[searchStart + bestLag + i] * source[searchStart + bestLag + i];
                const double normalised = bestValue / std::sqrt(chunkEnergy * sourceEnergy + 1e-30);
                const double candidate = (double)(searchStart + bestLag) / audio.sampleRate - t;
                if (normalised > 0.3 && (!found || candidate > offset - 0.05)) {
                    if (!found) firstMatch = rec.mapTime.size();
                    offset = candidate;
                    found = true;
                }
            }
        }
        rec.mapTime.push_back(t);
        rec.sourceOffset.push_back(offset);
    }
    if (!found) { rec.mapTime.clear(); rec.sourceOffset.clear(); return; }
    for (size_t k = 0; k < firstMatch; ++k) rec.sourceOffset[k] = rec.sourceOffset[firstMatch];
}

// Finds where playback starts and where the knob first moves (band levels leave the first
// 2.5 s of audio). Only a fallback now: fitSequenceStart is more reliable.
static bool findFirstKnobMove(Recording &rec) {
    const Spectra &spectra = rec.spectra;
    const size_t frameCount = spectra.bandsLeft.size();
    std::vector<double> frameDb(frameCount);
    double loudest = -1e9;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        double sum = 0;
        for (int band = 0; band < kBandCount; ++band) sum += spectra.bandsLeft[frame][band] + spectra.bandsRight[frame][band];
        frameDb[frame] = toDb(sum);
        loudest = std::max(loudest, frameDb[frame]);
    }
    size_t start = 0;
    while (start < frameCount && frameDb[start] < loudest - 40.0) ++start;
    rec.playbackStart = spectra.timeOf(start);
    rec.referenceFirstFrame = spectra.frameAt(rec.playbackStart + 0.5);
    rec.referenceEndFrame = spectra.frameAt(rec.playbackStart + 3.0);
    if (rec.referenceEndFrame + 20 >= frameCount) return false;
    rec.reference = meanBands(spectra, rec.referenceFirstFrame, rec.referenceEndFrame);

    // mean |dB difference| from the reference, 63 Hz .. 10 kHz, over 0.25 s from a frame
    const int smoothFrames = std::max(1, (int)std::lround(0.25 / spectra.hopSeconds));
    auto deviation = [&](size_t frame) {
        const std::vector<double> mean = meanBands(spectra, frame, frame + smoothFrames);
        double sum = 0;
        for (int band = 3; band < kBandCount - 2; ++band) sum += std::fabs(toDb(mean[band]) - toDb(rec.reference[band]));
        return sum / (kBandCount - 5);
    };
    // threshold: well above how much untouched audio deviates from itself
    double sum = 0, sumSquares = 0;
    int count = 0;
    for (size_t frame = rec.referenceFirstFrame; frame + smoothFrames <= rec.referenceEndFrame; ++frame) {
        const double d = deviation(frame);
        sum += d;
        sumSquares += d * d;
        ++count;
    }
    const double mean = sum / count, spread = std::sqrt(std::max(0.0, sumSquares / count - mean * mean));
    const double threshold = std::max(1.5, mean + 5.0 * spread);
    const int sustainFrames = (int)std::lround(0.5 / spectra.hopSeconds);   // must stay above it for 0.5 s
    int run = 0;
    for (size_t frame = rec.referenceEndFrame; frame + smoothFrames < frameCount; ++frame) {
        if (deviation(frame) > threshold) {
            if (++run >= sustainFrames) { rec.firstKnobMove = spectra.timeOf(frame - run + 1); return true; }
        } else run = 0;
    }
    return false;
}

// Over frames [firstFrame, endFrame) of one channel's grid spectrum, measures:
//   octavesPerSecond  how fast the notch pattern moves (+ = up), from the frequency shift that
//                     best matches each frame to the frame one FFT length later
//   confidence        how well that shift matches (0..1)
//   rippleVariance    variance (dB^2) of the spectrum around its mean shape; the caller
//                     subtracts the untouched audio's variance to get the notch depth
static void measureSweep(const std::vector<std::vector<float>> &grid, size_t firstFrame, size_t endFrame, double hopSeconds,
                         double &octavesPerSecond, double &confidence, double &rippleVariance) {
    octavesPerSecond = 0; confidence = 0; rippleVariance = 0;
    endFrame = std::min(endFrame, grid.size());
    // Compare frames that don't overlap, so their noise is independent and doesn't favour
    // "no movement".
    const size_t lagFrames = kFftSize / kHopSize;
    if (endFrame <= firstFrame + lagFrames + 2) return;
    // remove the mean shape (the DJ filter's curve) so only the moving part is left
    std::vector<double> meanShape(kGridCount, 0.0);
    for (size_t frame = firstFrame; frame < endFrame; ++frame)
        for (int point = 0; point < kGridCount; ++point) meanShape[point] += grid[frame][point];
    for (int point = 0; point < kGridCount; ++point) meanShape[point] /= (double)(endFrame - firstFrame);
    auto detrended = [&](size_t frame, int point) { return grid[frame][point] - meanShape[point]; };

    double variance = 0;
    for (size_t frame = firstFrame; frame < endFrame; ++frame) {
        double frameMean = 0;
        for (int point = 0; point < kGridCount; ++point) frameMean += detrended(frame, point);
        frameMean /= kGridCount;
        for (int point = 0; point < kGridCount; ++point) { const double v = detrended(frame, point) - frameMean; variance += v * v; }
    }
    variance /= (double)(endFrame - firstFrame) * kGridCount;
    rippleVariance = variance;

    // Cross-correlate each frame with the frame lagFrames later over frequency shifts of up
    // to +-1/2 octave (the notches are an octave apart, so larger shifts would be ambiguous).
    const int maxShift = 12;   // grid points (1/24 octave each)
    std::vector<double> correlation(2 * maxShift + 1, 0.0);
    double autoCorrelation = 0;
    for (size_t frame = firstFrame; frame + lagFrames < endFrame; ++frame) {
        for (int shift = -maxShift; shift <= maxShift; ++shift) {
            double sum = 0;
            int count = 0;
            for (int point = std::max(0, -shift); point < kGridCount && point + shift < kGridCount; ++point) {
                sum += detrended(frame, point) * detrended(frame + lagFrames, point + shift);
                ++count;
            }
            correlation[shift + maxShift] += sum / count;
        }
        double sum = 0;
        for (int point = 0; point < kGridCount; ++point) sum += detrended(frame, point) * detrended(frame, point);
        autoCorrelation += sum / kGridCount;
    }
    int best = 0;
    for (int i = 1; i < 2 * maxShift + 1; ++i) if (correlation[i] > correlation[best]) best = i;
    double shift = best - maxShift;
    if (best > 0 && best < 2 * maxShift) {   // parabolic interpolation for a sub-point shift
        const double before = correlation[best - 1], peak = correlation[best], after = correlation[best + 1];
        const double curvature = before - 2 * peak + after;
        if (curvature < 0) shift += 0.5 * (before - after) / curvature;
    }
    octavesPerSecond = (shift / 24.0) / (lagFrames * hopSeconds);
    confidence = autoCorrelation > 0 ? correlation[best] / autoCorrelation : 0;
}

// Correlation of left and right over [startSeconds, endSeconds): 1 = mono, 0 = unrelated.
static double stereoCorrelation(const Audio &audio, double startSeconds, double endSeconds) {
    const size_t first = (size_t)std::max(0.0, startSeconds * audio.sampleRate);
    const size_t end = std::min(audio.left.size(), (size_t)(endSeconds * audio.sampleRate));
    double leftEnergy = 0, rightEnergy = 0, cross = 0;
    for (size_t i = first; i < end; ++i) {
        leftEnergy += (double)audio.left[i] * audio.left[i];
        rightEnergy += (double)audio.right[i] * audio.right[i];
        cross += (double)audio.left[i] * audio.right[i];
    }
    return (leftEnergy > 0 && rightEnergy > 0) ? cross / std::sqrt(leftEnergy * rightEnergy) : 0.0;
}

// ---------------------------------------------------------------------------
// Measuring one knob position

struct Measurement {
    bool valid = false;            // false = the window is outside the recording
    double levelDb = 0;            // full-band level change
    double edgeHz = -1;            // -6 dB filter edge: -1 = everything cut, 0 = nothing cut
    double beatsPerOctave = 0;     // sweep speed (+ rising, - falling), 0 = none found
    double sweepConfidence = 0;
    double rippleDb = 0;           // notch depth
    double stereo = 0;             // L/R correlation
    bool highPass = false;
    double knob = 0.5;
    std::vector<double> bandChangeDb;   // per 1/3-octave band
};

static Measurement measure(const Recording &rec, double sequenceFrom, double sequenceTo, double knob, double bpm) {
    Measurement result;
    const double from = rec.atSequenceTime(sequenceFrom), to = rec.atSequenceTime(sequenceTo);
    const size_t firstFrame = rec.spectra.frameAt(from), endFrame = rec.spectra.frameAt(to);
    if (from < 0 || endFrame >= rec.spectra.bandsLeft.size() || endFrame <= firstFrame + 2) return result;
    result.valid = true;
    result.knob = knob;
    const std::vector<double> mean = meanBands(rec.spectra, firstFrame, endFrame);
    result.levelDb = toDb(totalEnergy(mean)) - toDb(totalEnergy(rec.reference));
    result.bandChangeDb.resize(kBandCount);
    for (int band = 0; band < kBandCount; ++band) result.bandChangeDb[band] = toDb(mean[band]) - toDb(rec.reference[band]);

    // -6 dB edge of the pass band, interpolated in log frequency between the bands either side
    result.highPass = knob > 0.5;
    if (std::fabs(knob - 0.5) > 0.03) {
        auto crossing = [&](int passBand, int cutBand) {
            const double cutDb = result.bandChangeDb[cutBand], passDb = result.bandChangeDb[passBand];
            const double fraction = (passDb == cutDb) ? 0.5 : (-6.0 - cutDb) / (passDb - cutDb);
            return bandCentreHz(cutBand) * std::pow(bandCentreHz(passBand) / bandCentreHz(cutBand), fraction);
        };
        if (result.highPass) {   // first band from the bottom that is no longer cut
            for (int band = 1; band < kBandCount; ++band)
                if (result.bandChangeDb[band] >= -6.0 && result.bandChangeDb[band - 1] < -6.0) { result.edgeHz = crossing(band, band - 1); break; }
            if (result.edgeHz < 0 && result.bandChangeDb[0] >= -6.0) result.edgeHz = 0;   // nothing removed
        } else {                 // first band from the top that is no longer cut
            for (int band = kBandCount - 2; band >= 0; --band)
                if (result.bandChangeDb[band] >= -6.0 && result.bandChangeDb[band + 1] < -6.0) { result.edgeHz = crossing(band, band + 1); break; }
            if (result.edgeHz < 0 && result.bandChangeDb[kBandCount - 1] >= -6.0) result.edgeHz = 0;
        }
    }
    double octavesPerSecond, confidence, variance;
    measureSweep(rec.spectra.gridLeft, firstFrame, endFrame, rec.spectra.hopSeconds, octavesPerSecond, confidence, variance);
    result.rippleDb = std::sqrt(std::max(0.0, variance - rec.referenceRippleVariance));
    result.sweepConfidence = confidence;
    result.beatsPerOctave = std::fabs(octavesPerSecond) > 1e-3 ? (bpm / 60.0) / octavesPerSecond : 0.0;
    result.stereo = stereoCorrelation(rec.audio, from, to);
    return result;
}

// ---------------------------------------------------------------------------
// The timing log written by knob_sequencer: one row per knob position.

struct Step { int index; std::string label; double start, end, knob; };

static bool isSweep(const Step &step) { return step.label.rfind("sweep", 0) == 0; }

static bool readLog(const std::string &path, std::vector<Step> &steps) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        std::stringstream fields(line);
        std::string field[6];   // step, label, start_s, end_s, knob, midi_value
        for (int i = 0; i < 6; ++i) std::getline(fields, field[i], ',');
        if (field[5].empty()) continue;
        steps.push_back({std::atoi(field[0].c_str()), field[1], std::atof(field[2].c_str()), std::atof(field[3].c_str()), std::atof(field[4].c_str())});
    }
    return !steps.empty();
}

// Finds the source time where the sequence starts by fitting the whole knob pattern: right
// turns cut the lows, left turns cut the highs. For each candidate start, every hold scores
// (8-16 kHz level - 31-63 Hz level) with the sign of its side; the best candidates form a
// plateau (any start that keeps the windows inside their holds scores about the same), and
// the middle of that plateau is used. Returns false if no candidate fits in the recording.
static bool fitSequenceStart(const Recording &rec, const std::vector<Step> &steps, double &startInSource) {
    const double recordingLength = rec.audio.left.size() / (double)rec.audio.sampleRate;
    const double sequenceLength = steps.back().end;
    double bestScore = -1e30, bestStart = -1;
    std::vector<double> candidates, scores;
    for (double start = rec.toSource(0.0) - 5.0; start + sequenceLength <= rec.toSource(recordingLength); start += 0.1) {
        double score = 0;
        for (size_t s = 1; s < steps.size(); ++s) {
            const Step &step = steps[s];
            if (isSweep(step) || std::fabs(step.knob - 0.5) < 0.03) continue;   // only off-centre holds
            // 1 s clear of the glide at each end, so the plateau is centred on the true start
            const double from = rec.toRecording(start + step.start + 1.5), to = rec.toRecording(start + step.end - 1.0);
            if (from < 0) { score = -1e30; break; }
            const std::vector<double> mean = meanBands(rec.spectra, rec.spectra.frameAt(from), rec.spectra.frameAt(to));
            double lows = 0, highs = 0;
            for (int band = 0; band < 4; ++band) lows += mean[band];                         // 31 .. 63 Hz
            for (int band = kBandCount - 6; band < kBandCount; ++band) highs += mean[band];  // 8 .. 16 kHz
            score += (step.knob > 0.5 ? 1.0 : -1.0) * (toDb(highs) - toDb(lows));
        }
        candidates.push_back(start);
        scores.push_back(score);
        if (score > bestScore) { bestScore = score; bestStart = start; }
    }
    if (bestScore <= -1e20) return false;
    size_t bestIndex = 0;
    while (candidates[bestIndex] != bestStart) ++bestIndex;
    const double tolerance = 0.03 * std::fabs(bestScore);
    size_t low = bestIndex, high = bestIndex;
    while (low > 0 && scores[low - 1] >= bestScore - tolerance) --low;
    while (high + 1 < scores.size() && scores[high + 1] >= bestScore - tolerance) ++high;
    startInSource = 0.5 * (candidates[low] + candidates[high]);
    return true;
}

// Knob position (0..1) at a sequence time, following the glides between steps.
static double knobAt(const std::vector<Step> &steps, double sequenceTime) {
    double knob = 64.0 / 127.0;
    for (size_t s = 0; s < steps.size(); ++s) {
        const Step &step = steps[s];
        if (sequenceTime < step.start) break;
        const bool afterSweep = s > 0 && isSweep(steps[s - 1]);
        const double glide = s == 0 ? 0.0 : (isSweep(step) ? 16.0 : (afterSweep ? 2.0 : 0.5));
        const double from = s == 0 ? step.knob : steps[s - 1].knob;
        knob = sequenceTime < step.start + glide ? from + (step.knob - from) * (sequenceTime - step.start) / glide : step.knob;
    }
    return knob;
}

static std::string formatEdge(const Measurement &m) {
    char text[32];
    if (!m.valid) return "-";
    if (m.edgeHz == 0 || std::fabs(m.knob - 0.5) <= 0.03) return "none";
    if (m.edgeHz < 0) return "all cut";
    if (m.edgeHz >= 1000) std::snprintf(text, sizeof text, "%.1fk", m.edgeHz / 1000.0);
    else std::snprintf(text, sizeof text, "%.0f", m.edgeHz);
    return text;
}

static std::string formatSweep(const Measurement &m) {
    char text[32];
    if (!m.valid) return "-";
    // too shallow or too unclear to be a real sweep
    if (m.rippleDb < 1.0 || m.sweepConfidence < 0.15 || m.beatsPerOctave == 0) return "none";
    std::snprintf(text, sizeof text, "%+.2f", m.beatsPerOctave);
    return text;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    std::string files[2], logPath = "knob_sequence_log.csv", sourcePath = "barberpole_test_126bpm.wav";
    double bpm = 126.0, manualStart[2] = {-1, -1};
    bool timeline = false;
    int fileCount = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--log" && i + 1 < argc) logPath = argv[++i];
        else if (arg == "--bpm" && i + 1 < argc) bpm = std::atof(argv[++i]);
        else if (arg == "--offset-a" && i + 1 < argc) manualStart[0] = std::atof(argv[++i]);
        else if (arg == "--offset-b" && i + 1 < argc) manualStart[1] = std::atof(argv[++i]);
        else if (arg == "--timeline") timeline = true;
        else if (arg == "--source" && i + 1 < argc) sourcePath = argv[++i];
        else if (fileCount < 2) files[fileCount++] = arg;
        else { std::fprintf(stderr, "Unexpected argument %s\n", arg.c_str()); return 1; }
    }
    if (fileCount != 2) {
        std::printf("Usage: compare_recordings <recording A> <recording B> [--log knob_sequence_log.csv] [--bpm 126]\n"
                    "                          [--source barberpole_test_126bpm.wav] [--offset-a <s>] [--offset-b <s>] [--timeline]\n");
        return 1;
    }
    std::vector<Step> steps;
    if (!readLog(logPath, steps)) { std::fprintf(stderr, "Could not read timing log %s\n", logPath.c_str()); return 1; }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    static Audio source;
    const bool haveSource = GetFileAttributesA(sourcePath.c_str()) != INVALID_FILE_ATTRIBUTES && decode(sourcePath, source);
    if (!haveSource) std::printf("(no source track %s: assuming the recordings have no gaps)\n", sourcePath.c_str());
    static Recording recordings[2];
    for (int k = 0; k < 2; ++k) {
        Recording &rec = recordings[k];
        if (!decode(files[k], rec.audio)) return 1;
        analyse(rec.audio, rec.spectra);
        const bool foundKnobMove = findFirstKnobMove(rec);
        if (haveSource) alignToSource(rec, source.left, source.sampleRate);
        if (manualStart[k] >= 0) rec.sequenceStart = manualStart[k];
        else {
            double startInSource;
            if (fitSequenceStart(rec, steps, startInSource)) {
                rec.sequenceStart = rec.toRecording(startInSource);
            } else {
                if (!foundKnobMove) { std::fprintf(stderr, "%s: could not line up the knob sequence; use --offset-%c\n", files[k].c_str(), 'a' + k); return 1; }
                // the knob leaves the dead zone ~0.12 s into the first glide, which starts at 8.0 s
                rec.sequenceStart = rec.firstKnobMove - 8.12;
            }
        }
        rec.sequenceStartInSource = rec.toSource(rec.sequenceStart);
        // The sequence opens with 8 s at centre, which is untouched by definition: use that as
        // the reference (the audio before it may still have the knob off-centre from earlier).
        if (rec.atSequenceTime(0.5) >= 0) {
            rec.referenceFirstFrame = rec.spectra.frameAt(rec.atSequenceTime(0.5));
            rec.referenceEndFrame = rec.spectra.frameAt(rec.atSequenceTime(7.5));
            rec.reference = meanBands(rec.spectra, rec.referenceFirstFrame, rec.referenceEndFrame);
        }
        if (manualStart[k] < 0 && rec.sequenceStart < rec.playbackStart)
            std::fprintf(stderr, "Warning: %s: the knob moved less than 8 s after playback started; the untouched reference may be short\n", files[k].c_str());
        double unusedRate, unusedConfidence, variance;
        measureSweep(rec.spectra.gridLeft, rec.referenceFirstFrame, rec.referenceEndFrame, rec.spectra.hopSeconds, unusedRate, unusedConfidence, variance);
        rec.referenceRippleVariance = variance;
        std::printf("%c: %s\n   %d Hz, %.1f s. Playback starts at %.2f s, knob first moves at %.2f s -> sequence starts at %.2f s\n",
                    'A' + k, files[k].c_str(), rec.audio.sampleRate, rec.audio.left.size() / (double)rec.audio.sampleRate,
                    rec.playbackStart, rec.firstKnobMove, rec.sequenceStart);
        if (haveSource && rec.mapTime.empty()) std::printf("   could not follow it through the source track (assuming no gaps)\n");
        for (size_t i = 1; i < rec.mapTime.size(); ++i)
            if (rec.sourceOffset[i] - rec.sourceOffset[i - 1] > 0.05)
                std::printf("   gap: the recording skips %.2f s of the track at %.0f s (sequence %.0f s)\n",
                            rec.sourceOffset[i] - rec.sourceOffset[i - 1], rec.mapTime[i], rec.toSource(rec.mapTime[i]) - rec.sequenceStartInSource);
    }
    MFShutdown();

    const int octaveBands[] = {0, 3, 6, 9, 12, 15, 18, 21, 24, 27};   // 31 Hz .. 16 kHz

    if (timeline) {
        // every 0.5 s of sequence time: expected knob, then per recording the octave-band levels
        std::printf("\nTimeline (sequence seconds; octave bands 31 Hz .. 16 kHz, dB vs untouched)\n");
        for (double t = -2.0; t < steps.back().end + 2.0; t += 0.5) {
            const double knob = knobAt(steps, t);
            for (int k = 0; k < 2; ++k) {
                const Recording &rec = recordings[k];
                const size_t firstFrame = rec.spectra.frameAt(rec.atSequenceTime(t)), endFrame = rec.spectra.frameAt(rec.atSequenceTime(t + 0.5));
                std::printf("%6.1f %3d %c ", t, (int)std::lround(knob * 127), 'A' + k);
                if (rec.atSequenceTime(t) < 0 || endFrame >= rec.spectra.bandsLeft.size() || endFrame <= firstFrame) { std::printf("  -\n"); continue; }
                const std::vector<double> mean = meanBands(rec.spectra, firstFrame, endFrame);
                std::printf("%6.1f |", toDb(totalEnergy(mean)) - toDb(totalEnergy(rec.reference)));
                for (int band : octaveBands) std::printf("%6.0f", std::max(-99.0, toDb(mean[band]) - toDb(rec.reference[band])));
                std::printf("\n");
            }
        }
        return 0;
    }

    std::printf("\nChecks: the centre (12 o'clock) holds should sound untouched (level 0 dB, -6 dB edge \"none\").\n");

    std::printf("\nlevel = full-band change (dB)   -6dB = filter edge (Hz)   sweep = beats per octave (+ rising, - falling)\n"
                "ripple = depth of the moving notches (dB)   L/R = stereo correlation\n\n");
    std::printf("%-22s %4s | %-38s | %-38s\n", "", "", "A", "B");
    std::printf("%-22s %4s | %6s %7s %6s %6s %5s | %6s %7s %6s %6s %5s\n", "position", "MIDI",
                "level", "-6dB", "sweep", "ripple", "L/R", "level", "-6dB", "sweep", "ripple", "L/R");
    std::printf("%s\n", std::string(110, '-').c_str());

    // One row per hold; the 16 s sweeps are split into 2 s slices.
    struct Row { std::string label; int midi; Measurement result[2]; };
    std::vector<Row> rows;
    for (size_t s = 0; s < steps.size(); ++s) {
        const Step &step = steps[s];
        const bool afterSweep = s > 0 && isSweep(steps[s - 1]);
        if (isSweep(step)) {
            const double glide = 16.0;
            const double fromKnob = steps[s - 1].knob, toKnob = step.knob;
            for (double sliceStart = 0; sliceStart + 2.0 <= glide + 1e-9; sliceStart += 2.0) {
                const double midKnob = fromKnob + (toKnob - fromKnob) * (sliceStart + 1.0) / glide;
                char label[64];
                std::snprintf(label, sizeof label, "  %s %2.0f-%2.0fs", toKnob > fromKnob ? "sweep R" : "sweep L", sliceStart, sliceStart + 2.0);
                Row row{label, (int)std::lround(midKnob * 127)};
                for (int k = 0; k < 2; ++k) row.result[k] = measure(recordings[k], step.start + sliceStart, step.start + sliceStart + 2.0, midKnob, bpm);
                rows.push_back(row);
            }
            continue;
        }
        // skip the glide into the hold plus 1 s of settling, and the last 0.3 s
        const double glide = s == 0 ? 0.0 : (afterSweep ? 2.0 : 0.5);
        Row row{step.label, (int)std::lround(step.knob * 127)};
        for (int k = 0; k < 2; ++k) row.result[k] = measure(recordings[k], step.start + glide + 1.0, step.end - 0.3, step.knob, bpm);
        rows.push_back(row);
    }
    for (const Row &row : rows) {
        std::printf("%-22s %4d |", row.label.c_str(), row.midi);
        for (int k = 0; k < 2; ++k) {
            const Measurement &m = row.result[k];
            if (!m.valid) { std::printf(" %-37s |", "(outside recording)"); continue; }
            std::printf(" %+6.1f %7s %6s %6.1f %5.2f |", m.levelDb, formatEdge(m).c_str(), formatSweep(m).c_str(), m.rippleDb, m.stereo);
        }
        std::printf("\n");
    }

    // octave-band level changes
    std::printf("\nLevel change per octave band (dB), A over B\n%-22s ", "position");
    for (int band : octaveBands) {
        const double hz = bandCentreHz(band);
        if (hz >= 1000) std::printf("%6.0fk", hz / 1000.0); else std::printf("%7.0f", hz);
    }
    std::printf("\n%s\n", std::string(22 + 7 * 10 + 1, '-').c_str());
    for (const Row &row : rows) {
        if (row.label[0] == ' ' && row.label.find(" 0- 2s") == std::string::npos && row.label.find("14-16s") == std::string::npos &&
            row.label.find(" 6- 8s") == std::string::npos)
            continue;   // keep the table short: first, middle and last slice of each sweep
        for (int k = 0; k < 2; ++k) {
            std::printf("%-20s %c ", k == 0 ? row.label.c_str() : "", 'A' + k);
            if (!row.result[k].valid) { std::printf("  -\n"); continue; }
            for (int band : octaveBands) std::printf("%7.1f", std::max(-99.9, row.result[k].bandChangeDb[band]));
            std::printf("\n");
        }
    }
    return 0;
}
