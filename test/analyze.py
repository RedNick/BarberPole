"""Test suite for the Barber Pole DSP core (dual-pole filter + Shepard sweep).
Build first:  g++ -O2 -std=c++11 -shared -fPIC -o libbp.so capi.cpp"""
import ctypes, numpy as np, sys, os
lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'libbp.so'))
F = ctypes.POINTER(ctypes.c_float)
lib.bp_new.restype = ctypes.c_void_p; lib.bp_new.argtypes = [ctypes.c_int]
lib.bp_free.argtypes = [ctypes.c_void_p]
lib.bp_set.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_double]
lib.bp_process.argtypes = [ctypes.c_void_p, F, ctypes.c_int]
lib.bp_rate.restype = ctypes.c_double; lib.bp_rate.argtypes = [ctypes.c_void_p]

ok = True
def check(name, cond, info=''):
    global ok
    print(('PASS ' if cond else 'FAIL ') + name + (' ' + info if info else ''))
    ok &= bool(cond)

def run(x2, color, speed=0.5, reso=0.4, bps=2.0, sr=44100, block=512, schedule=None):
    h = lib.bp_new(sr); y = np.ascontiguousarray(x2.astype(np.float32).copy())
    for i in range(0, len(y), block):
        c = schedule(i / sr) if schedule else color
        lib.bp_set(h, c, speed, reso, bps)
        b = np.ascontiguousarray(y[i:i + block]); lib.bp_process(h, b.ctypes.data_as(F), len(b)); y[i:i + block] = b
    lib.bp_free(h); return y

def pink(n, sr, seed=0, level=0.25):
    rng = np.random.default_rng(seed)
    w = np.fft.rfft(rng.standard_normal(n)); f = np.fft.rfftfreq(n, 1 / sr)
    w[1:] /= np.sqrt(f[1:]); w[0] = 0; x = np.fft.irfft(w, n)
    x = x / np.sqrt(np.mean(x ** 2)) * level
    return np.stack([x, x], 1).astype(np.float32)

def rms_db(a): return 20 * np.log10(np.sqrt(np.mean(np.asarray(a, np.float64) ** 2)) + 1e-30)

sr = 44100
xp = pink(sr * 10, sr)

# ---- 1. Centre and dead zone are a bit-exact bypass (knob jitter around 12 o'clock)
for c in (0.5, 0.48, 0.52, 0.5 + 0.0299, 0.5 - 0.0299):
    y = run(xp[:sr * 2], c)
    check(f'Knob {c:.4f} = exact bypass', np.array_equal(y, xp[:sr * 2]))
y = run(xp[:sr * 4], None, schedule=lambda t: 0.9 if t < 1.5 else 0.5)
check('Returning to centre ends fully clean (bit-exact)', np.array_equal(y[-sr:], xp[sr * 3:sr * 4]))

# ---- 2. Dual-pole filter: left = low-pass, right = high-pass, scaling to a FULL cut at each end
def band_change(y, x, lo, hi):
    w = np.blackman(len(x) - sr)   # windowed, so strong sub-bass can't leak into other bands
    X = np.abs(np.fft.rfft(x[sr:, 0] * w)); Y = np.abs(np.fft.rfft(y[sr:, 0] * w)); f = np.fft.rfftfreq(len(x) - sr, 1 / sr)
    m = (f > lo) & (f < hi); return 20 * np.log10(np.sqrt(np.sum(Y[m] ** 2) / np.sum(X[m] ** 2)) + 1e-15)
seg = xp[:sr * 5]
bands = [(40, 120), (300, 1000), (2000, 5000), (8000, 14000)]
print('   knob   ' + '  '.join(f'{lo}-{hi} Hz' for lo, hi in bands))
table = {}
for c in (0.0, 0.05, 0.2, 0.35, 0.65, 0.8, 0.95, 1.0):
    y = run(seg, c); table[c] = [band_change(y, seg, lo, hi) for lo, hi in bands]
    print(f'   {c:.2f}  ' + '  '.join(f'{v:+9.1f}' for v in table[c]))
check('Full left: everything above 300 Hz cut >= 60 dB (not faded to silence)', all(v < -60 for v in table[0.0][1:]) and table[0.0][0] > -150)
check('Full right: everything below 5 kHz cut >= 60 dB (not faded to silence)', all(v < -60 for v in table[1.0][:3]) and table[1.0][3] > -150)
check('Left: highs go first, lows survive until near the end',
      table[0.35][3] < -30 and table[0.35][0] > -3 and table[0.05][2] < -50 and table[0.05][0] > -10)
check('Right: lows go first, highs survive until near the end',
      table[0.8][0] < -30 and table[0.8][3] > -3 and table[0.95][2] < -20 and table[0.95][3] > -3)
for side, cs, col in (('left', (0.35, 0.2, 0.05), 1), ('right', (0.65, 0.8, 0.95), 1)):
    vals = [table[c][col] for c in cs]
    check(f'{side.title()}: cut keeps deepening as the knob turns further', vals[0] > vals[1] > vals[2],
          '(' + ' > '.join(f'{v:.1f}' for v in vals) + ' dB)')

# ---- 3. Endless sweep direction: track the resonance peaks' log-frequency phase over time.
# Voices are one octave apart, so the response (in log2 f) is periodic with period 1 octave;
# the phase of that periodicity rotates at the sweep rate.
def sweep_rate(color, bps=2.0, speed=0.5):
    n = sr * 12; x = pink(n, sr, seed=5); y = run(x, color, speed=speed, bps=bps)
    N = 16384; hop = 2048; f = np.fft.rfftfreq(N, 1 / sr); w = np.hanning(N)
    # measure 3 octaves the DJ filter leaves alone: above the HPF on the right, below the LPF on the left
    band = ((f > 1000) & (f < 8000)) if color > 0.5 else ((f > 60) & (f < 480))
    lf = np.log2(f[band]); ph = []
    for s in range(sr * 2, n - N, hop):
        H = np.abs(np.fft.rfft(y[s:s + N, 0] * w)) / (np.abs(np.fft.rfft(x[s:s + N, 0] * w)) + 1e-12)
        h = np.log(H[band] + 1e-9); h -= h.mean()
        ph.append(np.angle(np.sum(h * np.exp(-2j * np.pi * lf))))
    ph = np.unwrap(ph); tt = np.arange(len(ph)) * hop / sr
    # phase of sum(h*exp(-2j*pi*log2 f)) is MINUS the peak position, so negate
    return -np.polyfit(tt, ph, 1)[0] / (2 * np.pi)   # octaves per second, + = rising
# speed 0.5 -> 2 beats per octave; at 120 BPM (2 beats/s) = 1 oct/s, times (1 + 0.5 t)
for c in (0.75, 0.25):
    r = sweep_rate(c); t = (abs(c - 0.5) * 2 - 0.06) / 0.94; exp_r = 1.0 * (1 + 0.5 * t)
    print(f'   knob {c:.2f}: resonances move {r:+.3f} oct/s (expected {"+" if c > 0.5 else "-"}{exp_r:.3f})')
    check(f'Knob {c:.2f} sweeps {"UP" if c > 0.5 else "DOWN"} at the expected rate',
          np.sign(r) == np.sign(c - 0.5) and abs(abs(r) - exp_r) / exp_r < 0.05)
r128 = sweep_rate(0.75, bps=128 / 60); r = sweep_rate(0.75)
check('Sweep follows song tempo', abs(r128 / r - (128 / 120)) < 0.04, f'(ratio {r128 / r:.3f}, expected {128 / 120:.3f})')

# ---- 4. Loudness across the knob
print('   level on pink noise (full-band; the filter removes energy toward the extremes):')
lv = {}
for c in (0.0, 0.15, 0.3, 0.42, 0.58, 0.7, 0.85, 1.0):
    y = run(seg, c); lv[c] = rms_db(y[sr:]) - rms_db(seg[sr:]); print(f'     knob {c:.2f}: {lv[c]:+.1f} dB')
# just past the dead zone the filter is barely open, so judge loudness where the music lives (200 Hz - 5 kHz)
mid = {c: band_change(run(seg, c), seg, 200, 5000) for c in (0.42, 0.58)}
check('Just past the dead zone: 200 Hz-5 kHz level within +-2.5 dB', all(abs(v) < 2.5 for v in mid.values()),
      '(' + ', '.join(f'knob {c}: {v:+.1f} dB' for c, v in mid.items()) + ')')
# sustained notes must not get boosted by the sweep (soft, no screaming resonances)
tone = np.sin(2 * np.pi * 700 * np.arange(sr * 6) / sr) * 0.1; xt = np.stack([tone, tone], 1).astype(np.float32)
pk = max(20 * np.log10(np.abs(run(xt, c, reso=r)[sr:, 0]).max() / 0.1) for c in (0.8, 0.2, 1.0) for r in (0.4, 1.0))
check('A sustained note is never boosted more than +4.5 dB', pk < 4.5, f'(worst {pk:+.1f} dB)')
check('Never louder than +3 dB anywhere', max(lv.values()) < 3)

# ---- 5. Robustness
loud = np.clip(np.random.default_rng(2).standard_normal((sr * 4, 2)) * 0.9, -1, 1).astype(np.float32)
gauss = (np.random.default_rng(3).standard_normal((sr * 4, 2)) * 0.25).astype(np.float32)   # unclipped
for s_r in (44100, 48000, 96000):
    for reso in (0.0, 1.0):
        # torture input (heavily clipped white noise): must stay finite and sane. Any steep
        # high-pass, including a textbook Butterworth, turns clipped flat-tops into ~2x spikes.
        tort = [run(loud[:s_r * 3], c, reso=reso, sr=s_r, speed=1.0) for c in (0.0, 0.2, 0.8, 1.0)]
        # unclipped noise: output peaks must not exceed input peaks by more than 25 %
        pk_in = np.abs(gauss[:s_r * 3]).max()
        pk = max(np.abs(run(gauss[:s_r * 3], c, reso=reso, sr=s_r, speed=1.0)).max() for c in (0.1, 0.3, 0.7, 0.9))
        check(f'Stable @ {s_r} Hz, reso {reso}', all(np.isfinite(t).all() and np.abs(t).max() < 3 for t in tort)
              and pk <= pk_in * 1.25, f'(unclipped noise peak {pk:.2f} vs input {pk_in:.2f})')
tone = np.sin(2 * np.pi * 220 * np.arange(sr * 6) / sr) * 0.4
xt = np.stack([tone, tone], 1).astype(np.float32)
y = run(xt, None, block=256, schedule=lambda t: 0.5 + 0.5 * np.sin(2 * np.pi * 0.7 * t))
# click detector: broadband (>3 kHz) energy in any 6 ms frame, relative to that frame (pure 220 Hz has none)
N = 256; worst = -200.0; fq = np.fft.rfftfreq(N, 1 / sr)
for st in range(0, len(y) - N, N // 2):
    Y = np.abs(np.fft.rfft(y[st:st + N, 0] * np.hanning(N)))
    Xf = np.abs(np.fft.rfft(xt[st:st + N, 0] * np.hanning(N)))
    worst = max(worst, 20 * np.log10(np.sqrt(np.sum(Y[fq > 3000] ** 2)) / np.sqrt(np.sum(Xf ** 2)) + 1e-12))
check('Sweeping knob L<->R through centre: finite, no clicks', np.isfinite(y).all() and worst < -50,
      f'(worst broadband burst {worst:.0f} dB below input level)')
# No periodic ticks when sweep voices wrap: in a strongly-cut stopband, no short frame may
# jump far above the filter floor (a wrap bug once caused -15 dB bursts every sweep cycle).
xs = pink(sr * 12, sr, seed=2, level=0.2)
for c, lo, hi in ((0.92, 100, 600), (0.08, 3000, 8000)):
    yy = run(xs, c, bps=2.0); N = 512; fq2 = np.fft.rfftfreq(N, 1 / sr); mm = (fq2 > lo) & (fq2 < hi); lv = []
    for st in range(sr * 2, len(yy) - N, N // 2):
        lv.append(20 * np.log10(np.sqrt(np.sum(np.abs(np.fft.rfft(yy[st:st + N, 0] * np.hanning(N)))[mm] ** 2)) + 1e-15))
    lv = np.array(lv)
    check(f'No sweep-wrap ticks at knob {c}', lv.max() < np.median(lv) + 15,
          f'(stopband floor {np.median(lv):.0f} dB, worst frame {lv.max():.0f} dB)')
sil = np.concatenate([loud[:sr], np.zeros((sr * 3, 2), np.float32)])
y = run(sil, 0.9); check('Decays to silence', np.abs(y[-sr // 2:]).max() < 1e-5, f'(tail {np.abs(y[-sr//2:]).max():.1e})')

print('\nALL PASS' if ok else '\nSOME FAILED'); sys.exit(0 if ok else 1)
