// compare_recordings - compares two recordings of the knob_sequencer test (e.g. Traktor vs
// VirtualDJ) position by position.
//
// Windows only: decodes WAV / FLAC / MP3 etc. through Media Foundation, at each file's own rate.
//
// Usage:
//   compare_recordings <recording A> <recording B> [--log knob_sequence_log.csv] [--bpm 126]
//                      [--offset-a <s>] [--offset-b <s>]
//
// Each recording must contain some untouched (knob at centre) audio before the knob first
// moves. The tool finds where the knob first moves in each recording and lines the timing log
// up with it (or use --offset-a/-b: the recording time, in seconds, where the sequence starts).
//
// Per knob position it reports, relative to the recording's own untouched audio:
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

// ---------------------------------------------------------------------------
struct Audio {
    std::string name;
    int sr = 0;
    std::vector<float> L, R;
};

static bool decode(const std::string &path, Audio &a) {
    a.name = path;
    const int wn = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wn, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath[0], wn);

    IMFSourceReader *rd = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &rd))) {
        std::fprintf(stderr, "Could not open %s\n", path.c_str());
        return false;
    }
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    IMFMediaType *mt = nullptr;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    HRESULT hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt);
    mt->Release();
    if (FAILED(hr)) { std::fprintf(stderr, "Could not decode %s to float\n", path.c_str()); rd->Release(); return false; }
    IMFMediaType *cur = nullptr;
    rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
    UINT32 ch = 0, sr = 0;
    cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    cur->Release();
    if (ch < 1 || sr < 8000) { std::fprintf(stderr, "Bad format in %s\n", path.c_str()); rd->Release(); return false; }
    a.sr = (int)sr;
    for (;;) {
        DWORD flags = 0;
        IMFSample *s = nullptr;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s);
        if (FAILED(hr)) break;
        if (s) {
            IMFMediaBuffer *b = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
                BYTE *p = nullptr;
                DWORD len = 0;
                b->Lock(&p, nullptr, &len);
                const float *f = (const float *)p;
                const size_t n = len / sizeof(float) / ch;
                for (size_t i = 0; i < n; ++i) {
                    a.L.push_back(f[i * ch]);
                    a.R.push_back(f[i * ch + (ch > 1 ? 1 : 0)]);
                }
                b->Unlock();
                b->Release();
            }
            s->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    rd->Release();
    return !a.L.empty();
}

// ---------------------------------------------------------------------------
static void fft(std::vector<std::complex<double>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Analysis grid
static const int kN = 8192;         // FFT size
static const int kHop = 1024;       // hop (frames overlap 8x)
static const int kBands = 30;       // 1/3-octave bands, 31 Hz .. 16 kHz
static const int kGrid = 160;       // log-frequency grid, 1/24 octave, 100 Hz .. 10 kHz
static double bandHz(int b) { return 1000.0 * std::pow(2.0, (b - 15) / 3.0); }
static double gridHz(int j) { return 100.0 * std::pow(2.0, j / 24.0); }

struct Frames {
    int sr = 0;
    double hopS = 0;
    // per frame: band energies (linear) per channel, grid spectrum (dB) for L and R
    std::vector<std::vector<double>> bandL, bandR;
    std::vector<std::vector<float>> gridL, gridR;
    double t(size_t i) const { return (i * (double)kHop + kN / 2) / sr; }
    size_t index(double sec) const {
        const double i = (sec * sr - kN / 2) / kHop;
        return i < 0 ? 0 : (size_t)i;
    }
};

static void spectrum(const std::vector<float> &x, size_t pos, const std::vector<double> &win,
                     std::vector<double> &pw) {
    std::vector<std::complex<double>> a(kN);
    for (int i = 0; i < kN; ++i) a[i] = (pos + i < x.size() ? x[pos + i] : 0.0f) * win[i];
    fft(a);
    pw.resize(kN / 2 + 1);
    for (int k = 0; k <= kN / 2; ++k) pw[k] = std::norm(a[k]);
}

static void analyse(const Audio &a, Frames &f) {
    f.sr = a.sr;
    f.hopS = (double)kHop / a.sr;
    std::vector<double> win(kN);
    for (int i = 0; i < kN; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kN);
    const double binHz = (double)a.sr / kN;
    std::vector<double> pw;
    for (size_t pos = 0; pos + kN <= a.L.size(); pos += kHop) {
        for (int ch = 0; ch < 2; ++ch) {
            spectrum(ch == 0 ? a.L : a.R, pos, win, pw);
            std::vector<double> bands(kBands, 0.0);
            for (int b = 0; b < kBands; ++b) {
                const double lo = bandHz(b) * std::pow(2.0, -1.0 / 6.0), hi = bandHz(b) * std::pow(2.0, 1.0 / 6.0);
                for (int k = (int)std::ceil(lo / binHz); k <= kN / 2 && k * binHz < hi; ++k) bands[b] += pw[k];
                bands[b] += 1e-20;
            }
            std::vector<float> grid(kGrid);
            for (int j = 0; j < kGrid; ++j) {
                const double fc = gridHz(j);
                const double lo = fc * std::pow(2.0, -1.0 / 24.0), hi = fc * std::pow(2.0, 1.0 / 24.0);
                double sum = 0.0;
                int cnt = 0;
                for (int k = (int)std::ceil(lo / binHz); k <= kN / 2 && k * binHz < hi; ++k) { sum += pw[k]; ++cnt; }
                if (cnt == 0) {   // narrower than a bin: interpolate
                    const double kk = fc / binHz;
                    const int k0 = (int)kk;
                    const double fr = kk - k0;
                    sum = pw[k0] * (1.0 - fr) + pw[k0 + 1] * fr;
                    cnt = 1;
                }
                grid[j] = (float)(10.0 * std::log10(sum / cnt + 1e-20));
            }
            if (ch == 0) { f.bandL.push_back(bands); f.gridL.push_back(grid); }
            else         { f.bandR.push_back(bands); f.gridR.push_back(grid); }
        }
    }
}

static double db(double e) { return 10.0 * std::log10(e + 1e-30); }

// mean band energies (L+R) over frames [i0, i1)
static std::vector<double> meanBands(const Frames &f, size_t i0, size_t i1) {
    std::vector<double> m(kBands, 0.0);
    i1 = std::min(i1, f.bandL.size());
    if (i1 <= i0) return m;
    for (size_t i = i0; i < i1; ++i)
        for (int b = 0; b < kBands; ++b) m[b] += f.bandL[i][b] + f.bandR[i][b];
    for (int b = 0; b < kBands; ++b) m[b] /= (double)(i1 - i0);
    return m;
}

static double total(const std::vector<double> &bands) {
    double s = 0.0;
    for (int b = 1; b < kBands - 1; ++b) s += bands[b];   // 40 Hz .. 12.5 kHz
    return s;
}

// ---------------------------------------------------------------------------
struct Rec {
    Audio audio;
    Frames fr;
    double playStart = 0, onset = -1, seqStart = 0;   // recording seconds
    size_t refI0 = 0, refI1 = 0;
    std::vector<double> ref;                          // untouched band energies
    double refRipple = 0;

    // Where in the source track each part of the recording came from: source time =
    // recording time + mapOff[k] from mapT[k] on. Some recorders (VirtualDJ's) skip silence,
    // which shows up as a jump in the offset. Empty = no source given (offset 0).
    std::vector<double> mapT, mapOff;
    double seqSrc = 0;                                // source time where the sequence starts

    double offsetAt(double recT) const {
        if (mapT.empty()) return 0.0;
        const size_t k = std::upper_bound(mapT.begin(), mapT.end(), recT) - mapT.begin();
        return mapOff[k == 0 ? 0 : k - 1];
    }
    double toSrc(double recT) const { return recT + offsetAt(recT); }
    double toRec(double srcT) const {
        if (mapT.empty()) return srcT;
        size_t k = 0;
        while (k + 1 < mapT.size() && mapT[k + 1] + mapOff[k + 1] <= srcT) ++k;
        return std::min(srcT - mapOff[k], k + 1 < mapT.size() ? mapT[k + 1] : 1e9);
    }
    // recording time of a sequence time
    double at(double seqT) const { return toRec(seqSrc + seqT); }
};

// inverse FFT through the forward one: ifft(x) = conj(fft(conj(x))) / n
static void ifft(std::vector<std::complex<double>> &a) {
    for (auto &v : a) v = std::conj(v);
    fft(a);
    for (auto &v : a) v = std::conj(v) / (double)a.size();
}

// Follows the recording through the source track: every second, cross-correlates 1/3 s of the
// recording's left channel with the source around the expected position.
static void alignToSource(Rec &r, const std::vector<float> &srcIn, int srcSr) {
    const Audio &a = r.audio;
    std::vector<float> src;   // source at the recording's rate (linear interpolation)
    if (srcSr == a.sr) src = srcIn;
    else {
        src.resize((size_t)((double)srcIn.size() * a.sr / srcSr));
        for (size_t i = 0; i < src.size(); ++i) {
            const double p = i * (double)srcSr / a.sr;
            const size_t k = (size_t)p;
            const double fr = p - k;
            src[i] = k + 1 < srcIn.size() ? (float)(srcIn[k] * (1.0 - fr) + srcIn[k + 1] * fr) : 0.0f;
        }
    }
    const int M = 16384;
    double off = 0;
    bool have = false;
    size_t firstFound = 0;
    for (double t = 0; (size_t)(t * a.sr) + M < a.L.size(); t += 1.0) {
        const size_t p = (size_t)(t * a.sr);
        double e = 0;
        for (int i = 0; i < M; ++i) e += (double)a.L[p + i] * a.L[p + i];
        if (e > 1e-6 * M) {
            // search offsets [off - 1 s, off + ~10 s] (before the first match: -30 s .. +17 s)
            const size_t W = have ? (1u << 19) : (1u << 21);
            const double lo = have ? off - 1.0 : -30.0;
            const long long s0 = std::max(0LL, (long long)p + (long long)(lo * a.sr));
            if ((size_t)s0 < src.size()) {
                std::vector<std::complex<double>> S(W), C(W);
                for (size_t i = 0; i < W && s0 + i < src.size(); ++i) S[i] = src[s0 + i];
                for (int i = 0; i < M; ++i) C[i] = a.L[p + i];
                fft(S);
                fft(C);
                for (size_t i = 0; i < W; ++i) S[i] *= std::conj(C[i]);
                ifft(S);
                size_t best = 0;
                double bv = -1e30;
                for (size_t i = 0; i + M < W && s0 + i + M < src.size(); ++i)
                    if (S[i].real() > bv) { bv = S[i].real(); best = i; }
                double es = 0;
                for (int i = 0; i < M; ++i) es += (double)src[s0 + best + i] * src[s0 + best + i];
                const double corr = bv / std::sqrt(e * es + 1e-30);
                const double o = (double)(s0 + best) / a.sr - t;
                if (corr > 0.3 && (!have || o > off - 0.05)) {
                    if (!have) firstFound = r.mapT.size();
                    off = o;
                    have = true;
                }
            }
        }
        r.mapT.push_back(t);
        r.mapOff.push_back(off);
    }
    if (!have) { r.mapT.clear(); r.mapOff.clear(); return; }
    for (size_t k = 0; k < firstFound; ++k) r.mapOff[k] = r.mapOff[firstFound];
}

// time where the knob first moves: band levels leave the untouched reference
static bool findOnset(Rec &r) {
    const Frames &f = r.fr;
    const size_t n = f.bandL.size();
    std::vector<double> tot(n);
    double mx = -1e9;
    for (size_t i = 0; i < n; ++i) {
        double s = 0;
        for (int b = 0; b < kBands; ++b) s += f.bandL[i][b] + f.bandR[i][b];
        tot[i] = db(s);
        mx = std::max(mx, tot[i]);
    }
    size_t start = 0;
    while (start < n && tot[start] < mx - 40.0) ++start;
    r.playStart = f.t(start);
    r.refI0 = f.index(r.playStart + 0.5);
    r.refI1 = f.index(r.playStart + 3.0);
    if (r.refI1 + 20 >= n) return false;
    r.ref = meanBands(f, r.refI0, r.refI1);

    const int smooth = std::max(1, (int)std::lround(0.25 / f.hopS));
    auto dev = [&](size_t i) {
        const std::vector<double> m = meanBands(f, i, i + smooth);
        double d = 0;
        for (int b = 3; b < kBands - 2; ++b) d += std::fabs(db(m[b]) - db(r.ref[b]));   // 63 Hz .. 10 kHz
        return d / (kBands - 5);
    };
    double s = 0, s2 = 0;
    int cnt = 0;
    for (size_t i = r.refI0; i + smooth <= r.refI1; ++i) { const double d = dev(i); s += d; s2 += d * d; ++cnt; }
    const double mean = s / cnt, sd = std::sqrt(std::max(0.0, s2 / cnt - mean * mean));
    const double thr = std::max(1.5, mean + 5.0 * sd);
    const int sustain = (int)std::lround(0.5 / f.hopS);
    int run = 0;
    for (size_t i = r.refI1; i + smooth < n; ++i) {
        if (dev(i) > thr) {
            if (++run >= sustain) { r.onset = f.t(i - run + 1); return true; }
        } else run = 0;
    }
    return false;
}

// detrended grid spectrum ripple + sweep rate over frames [i0, i1) of one channel
static void sweep(const std::vector<std::vector<float>> &g, size_t i0, size_t i1, double hopS,
                  double &octPerSec, double &confidence, double &rippleDb) {
    octPerSec = 0; confidence = 0; rippleDb = 0;
    i1 = std::min(i1, g.size());
    const size_t lagFrames = kN / kHop;   // frames that don't overlap, so their noise is independent
    if (i1 <= i0 + lagFrames + 2) return;
    std::vector<double> mean(kGrid, 0.0);
    for (size_t i = i0; i < i1; ++i)
        for (int j = 0; j < kGrid; ++j) mean[j] += g[i][j];
    for (int j = 0; j < kGrid; ++j) mean[j] /= (double)(i1 - i0);
    auto d = [&](size_t i, int j) { return g[i][j] - mean[j]; };

    double var = 0;
    for (size_t i = i0; i < i1; ++i) {
        double fm = 0;
        for (int j = 0; j < kGrid; ++j) fm += d(i, j);
        fm /= kGrid;
        for (int j = 0; j < kGrid; ++j) { const double v = d(i, j) - fm; var += v * v; }
    }
    var /= (double)(i1 - i0) * kGrid;
    rippleDb = var;   // caller subtracts the untouched-audio variance

    const int maxLag = 12;   // +-1/2 octave
    std::vector<double> c(2 * maxLag + 1, 0.0);
    double c0 = 0;
    for (size_t i = i0; i + lagFrames < i1; ++i) {
        for (int l = -maxLag; l <= maxLag; ++l) {
            double s = 0;
            int cnt = 0;
            for (int j = std::max(0, -l); j < kGrid && j + l < kGrid; ++j) { s += d(i, j) * d(i + lagFrames, j + l); ++cnt; }
            c[l + maxLag] += s / cnt;
        }
        double s = 0;
        for (int j = 0; j < kGrid; ++j) s += d(i, j) * d(i, j);
        c0 += s / kGrid;
    }
    int best = 0;
    for (int l = 1; l < 2 * maxLag + 1; ++l) if (c[l] > c[best]) best = l;
    double lag = best - maxLag;
    if (best > 0 && best < 2 * maxLag) {
        const double y0 = c[best - 1], y1 = c[best], y2 = c[best + 1];
        const double den = y0 - 2 * y1 + y2;
        if (den < 0) lag += 0.5 * (y0 - y2) / den;
    }
    octPerSec = (lag / 24.0) / (lagFrames * hopS);
    confidence = c0 > 0 ? c[best] / c0 : 0;
}

static double stereoCorr(const Audio &a, double t0, double t1) {
    const size_t s0 = (size_t)std::max(0.0, t0 * a.sr), s1 = std::min(a.L.size(), (size_t)(t1 * a.sr));
    double ll = 0, rr = 0, lr = 0;
    for (size_t i = s0; i < s1; ++i) { ll += (double)a.L[i] * a.L[i]; rr += (double)a.R[i] * a.R[i]; lr += (double)a.L[i] * a.R[i]; }
    return (ll > 0 && rr > 0) ? lr / std::sqrt(ll * rr) : 0.0;
}

struct Result {
    bool ok = false;
    double level = 0, cutoff = -1, bpo = 0, conf = 0, ripple = 0, corr = 0;
    bool hpf = false;
    double knob = 0.5;
    std::vector<double> atten;   // per 1/3-octave band, dB
};

static Result measure(const Rec &r, double seqT0, double seqT1, double knob, double bpm) {
    Result res;
    const double t0 = r.at(seqT0), t1 = r.at(seqT1);
    const size_t i0 = r.fr.index(t0), i1 = r.fr.index(t1);
    if (t0 < 0 || i1 >= r.fr.bandL.size() || i1 <= i0 + 2) return res;
    res.ok = true;
    res.knob = knob;
    const std::vector<double> m = meanBands(r.fr, i0, i1);
    res.level = db(total(m)) - db(total(r.ref));
    res.atten.resize(kBands);
    for (int b = 0; b < kBands; ++b) res.atten[b] = db(m[b]) - db(r.ref[b]);
    // -6 dB edge of the pass band
    res.hpf = knob > 0.5;
    if (std::fabs(knob - 0.5) > 0.03) {
        auto crossing = [&](int b, int prev) {   // interpolate between band prev (< -6) and b (>= -6)
            const double a0 = res.atten[prev], a1 = res.atten[b];
            const double fr = (a1 == a0) ? 0.5 : (-6.0 - a0) / (a1 - a0);
            return bandHz(prev) * std::pow(bandHz(b) / bandHz(prev), fr);
        };
        if (res.hpf) {
            for (int b = 1; b < kBands; ++b)
                if (res.atten[b] >= -6.0 && res.atten[b - 1] < -6.0) { res.cutoff = crossing(b, b - 1); break; }
            if (res.cutoff < 0 && res.atten[0] >= -6.0) res.cutoff = 0;   // nothing removed
        } else {
            for (int b = kBands - 2; b >= 0; --b)
                if (res.atten[b] >= -6.0 && res.atten[b + 1] < -6.0) { res.cutoff = crossing(b, b + 1); break; }
            if (res.cutoff < 0 && res.atten[kBands - 1] >= -6.0) res.cutoff = 0;
        }
    }
    double ops, conf, var;
    sweep(r.fr.gridL, i0, i1, r.fr.hopS, ops, conf, var);
    res.ripple = std::sqrt(std::max(0.0, var - r.refRipple));
    res.conf = conf;
    res.bpo = std::fabs(ops) > 1e-3 ? (bpm / 60.0) / ops : 0.0;
    res.corr = stereoCorr(r.audio, t0, t1);
    return res;
}

// ---------------------------------------------------------------------------
struct Step { int idx; std::string label; double start, end, knob; };

static bool readLog(const std::string &path, std::vector<Step> &steps) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string f[6];
        for (int i = 0; i < 6; ++i) std::getline(ss, f[i], ',');
        if (f[5].empty()) continue;
        steps.push_back({std::atoi(f[0].c_str()), f[1], std::atof(f[2].c_str()), std::atof(f[3].c_str()), std::atof(f[4].c_str())});
    }
    return !steps.empty();
}

static std::string fmtCut(const Result &r) {
    char b[32];
    if (!r.ok) return "-";
    if (r.cutoff == 0 || std::fabs(r.knob - 0.5) <= 0.03) return "none";
    if (r.cutoff < 0) return "all cut";
    if (r.cutoff >= 1000) std::snprintf(b, sizeof b, "%.1fk", r.cutoff / 1000.0);
    else std::snprintf(b, sizeof b, "%.0f", r.cutoff);
    return b;
}

static std::string fmtSweep(const Result &r) {
    char b[32];
    if (!r.ok) return "-";
    if (r.ripple < 1.0 || r.conf < 0.15 || r.bpo == 0) return "none";
    std::snprintf(b, sizeof b, "%+.2f", r.bpo);
    return b;
}

int main(int argc, char **argv) {
    std::string files[2], logPath = "knob_sequence_log.csv", sourcePath = "barberpole_test_126bpm.wav";
    double bpm = 126.0, offset[2] = {-1, -1};
    bool timeline = false;
    int nf = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--log" && i + 1 < argc) logPath = argv[++i];
        else if (a == "--bpm" && i + 1 < argc) bpm = std::atof(argv[++i]);
        else if (a == "--offset-a" && i + 1 < argc) offset[0] = std::atof(argv[++i]);
        else if (a == "--offset-b" && i + 1 < argc) offset[1] = std::atof(argv[++i]);
        else if (a == "--timeline") timeline = true;
        else if (a == "--source" && i + 1 < argc) sourcePath = argv[++i];
        else if (nf < 2) files[nf++] = a;
        else { std::fprintf(stderr, "Unexpected argument %s\n", a.c_str()); return 1; }
    }
    if (nf != 2) {
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
    static Rec rec[2];
    for (int k = 0; k < 2; ++k) {
        if (!decode(files[k], rec[k].audio)) return 1;
        analyse(rec[k].audio, rec[k].fr);
        Rec &r = rec[k];
        const bool found = findOnset(r);
        if (haveSource) alignToSource(r, source.L, source.sr);
        // knob leaves the dead zone ~0.12 s into the first glide (which starts at sequence 8.0 s)
        if (offset[k] >= 0) r.seqStart = offset[k];
        else if (found) r.seqStart = r.onset - 8.12;
        else { std::fprintf(stderr, "%s: could not find where the knob first moves; use --offset-%c\n", files[k].c_str(), 'a' + k); return 1; }
        r.seqSrc = r.toSrc(r.seqStart);
        // The sequence opens with 8 s at centre, which is untouched by definition: use that as
        // the reference (the audio before it may still have the knob off-centre from earlier).
        if (r.at(0.5) >= 0) {
            r.refI0 = r.fr.index(r.at(0.5));
            r.refI1 = r.fr.index(r.at(7.5));
            r.ref = meanBands(r.fr, r.refI0, r.refI1);
        }
        if (offset[k] < 0 && r.seqStart < r.playStart)
            std::fprintf(stderr, "Warning: %s: the knob moved less than 8 s after playback started; the untouched reference may be short\n", files[k].c_str());
        double ops, conf, var;
        sweep(r.fr.gridL, r.refI0, r.refI1, r.fr.hopS, ops, conf, var);
        r.refRipple = var;
        std::printf("%c: %s\n   %d Hz, %.1f s. Playback starts at %.2f s, knob first moves at %.2f s -> sequence starts at %.2f s\n",
                    'A' + k, files[k].c_str(), r.audio.sr, r.audio.L.size() / (double)r.audio.sr, r.playStart, r.onset, r.seqStart);
        if (haveSource && r.mapT.empty()) std::printf("   could not follow it through the source track (assuming no gaps)\n");
        for (size_t i = 1; i < r.mapT.size(); ++i)
            if (r.mapOff[i] - r.mapOff[i - 1] > 0.05)
                std::printf("   gap: the recording skips %.2f s of the track at %.0f s (sequence %.0f s)\n",
                            r.mapOff[i] - r.mapOff[i - 1], r.mapT[i], r.toSrc(r.mapT[i]) - r.seqSrc);
    }
    MFShutdown();

    if (timeline) {
        // every 0.5 s of sequence time: expected knob, then per recording the octave-band levels
        const int oct[] = {0, 3, 6, 9, 12, 15, 18, 21, 24, 27};
        std::printf("\nTimeline (sequence seconds; octave bands 31 Hz .. 16 kHz, dB vs untouched)\n");
        for (double t = -2.0; t < steps.back().end + 2.0; t += 0.5) {
            double knob = 64.0 / 127.0;
            for (size_t s = 0; s < steps.size(); ++s) {
                const Step &st = steps[s];
                if (t < st.start) break;
                const bool isSweep = st.label.rfind("sweep", 0) == 0;
                const bool afterSweep = s > 0 && steps[s - 1].label.rfind("sweep", 0) == 0;
                const double glide = s == 0 ? 0.0 : (isSweep ? 16.0 : (afterSweep ? 2.0 : 0.5));
                const double k0 = s == 0 ? st.knob : steps[s - 1].knob;
                knob = t < st.start + glide ? k0 + (st.knob - k0) * (t - st.start) / glide : st.knob;
            }
            for (int k = 0; k < 2; ++k) {
                const Rec &r = rec[k];
                const size_t i0 = r.fr.index(r.at(t)), i1 = r.fr.index(r.at(t + 0.5));
                std::printf("%6.1f %3d %c ", t, (int)std::lround(knob * 127), 'A' + k);
                if (r.at(t) < 0 || i1 >= r.fr.bandL.size() || i1 <= i0) { std::printf("  -\n"); continue; }
                const std::vector<double> m = meanBands(r.fr, i0, i1);
                std::printf("%6.1f |", db(total(m)) - db(total(r.ref)));
                for (int b : oct) std::printf("%6.0f", std::max(-99.0, db(m[b]) - db(r.ref[b])));
                std::printf("\n");
            }
        }
        return 0;
    }

    // sanity check: the centre steps should match the untouched audio in both recordings
    std::printf("\nChecks: the centre (12 o'clock) holds should sound untouched (level 0 dB, -6 dB edge \"none\").\n");

    std::printf("\nlevel = full-band change (dB)   -6dB = filter edge (Hz)   sweep = beats per octave (+ rising, - falling)\n"
                "ripple = depth of the moving notches (dB)   L/R = stereo correlation\n\n");
    std::printf("%-22s %4s | %-38s | %-38s\n", "", "", "A", "B");
    std::printf("%-22s %4s | %6s %7s %6s %6s %5s | %6s %7s %6s %6s %5s\n", "position", "MIDI",
                "level", "-6dB", "sweep", "ripple", "L/R", "level", "-6dB", "sweep", "ripple", "L/R");
    std::printf("%s\n", std::string(110, '-').c_str());

    struct Row { std::string label; int midi; Result r[2]; };
    std::vector<Row> rows;
    for (size_t s = 0; s < steps.size(); ++s) {
        const Step &st = steps[s];
        const bool isSweep = st.label.rfind("sweep", 0) == 0;
        const bool afterSweep = s > 0 && steps[s - 1].label.rfind("sweep", 0) == 0;
        if (isSweep) {
            // 16 s glide: report 2 s slices
            const double glide = 16.0;
            const double k0 = steps[s - 1].knob, k1 = st.knob;
            for (double a = 0; a + 2.0 <= glide + 1e-9; a += 2.0) {
                const double km = k0 + (k1 - k0) * (a + 1.0) / glide;
                char lab[64];
                std::snprintf(lab, sizeof lab, "  %s %2.0f-%2.0fs", k1 > k0 ? "sweep R" : "sweep L", a, a + 2.0);
                Row row{lab, (int)std::lround(km * 127)};
                for (int k = 0; k < 2; ++k) row.r[k] = measure(rec[k], st.start + a, st.start + a + 2.0, km, bpm);
                rows.push_back(row);
            }
            continue;
        }
        const double glide = s == 0 ? 0.0 : (afterSweep ? 2.0 : 0.5);
        Row row{st.label, (int)std::lround(st.knob * 127)};
        for (int k = 0; k < 2; ++k) row.r[k] = measure(rec[k], st.start + glide + 1.0, st.end - 0.3, st.knob, bpm);
        rows.push_back(row);
    }
    for (const Row &row : rows) {
        std::printf("%-22s %4d |", row.label.c_str(), row.midi);
        for (int k = 0; k < 2; ++k) {
            const Result &r = row.r[k];
            if (!r.ok) { std::printf(" %-37s |", "(outside recording)"); continue; }
            std::printf(" %+6.1f %7s %6s %6.1f %5.2f |", r.level, fmtCut(r).c_str(), fmtSweep(r).c_str(), r.ripple, r.corr);
        }
        std::printf("\n");
    }

    // octave-band level changes
    const int oct[] = {0, 3, 6, 9, 12, 15, 18, 21, 24, 27};   // 31 Hz .. 16 kHz
    std::printf("\nLevel change per octave band (dB), A over B\n%-22s ", "position");
    for (int b : oct) {
        const double hz = bandHz(b);
        if (hz >= 1000) std::printf("%6.0fk", hz / 1000.0); else std::printf("%7.0f", hz);
    }
    std::printf("\n%s\n", std::string(22 + 7 * 10 + 1, '-').c_str());
    for (const Row &row : rows) {
        if (row.label[0] == ' ' && row.label.find(" 0- 2s") == std::string::npos && row.label.find("14-16s") == std::string::npos &&
            row.label.find(" 6- 8s") == std::string::npos)
            continue;   // keep the tables short: first, middle and last sweep slice
        for (int k = 0; k < 2; ++k) {
            std::printf("%-20s %c ", k == 0 ? row.label.c_str() : "", 'A' + k);
            if (!row.r[k].ok) { std::printf("  -\n"); continue; }
            for (int b : oct) std::printf("%7.1f", std::max(-99.9, row.r[k].atten[b]));
            std::printf("\n");
        }
    }
    return 0;
}
