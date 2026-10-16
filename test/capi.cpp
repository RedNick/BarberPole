// C API over the DSP core so test/analyze.py can drive it through ctypes.
// analyze.py calls these by name, so keep the names and signatures.
#include "../src/barberpole_dsp.h"
using namespace barberpole;
extern "C" {
// create an effect prepared for a sample rate
void *bp_new(int sampleRate) { auto *effect = new BarberPole(); effect->prepare(sampleRate); return effect; }
void bp_free(void *effect) { delete (BarberPole *)effect; }
// color/speed/resonance 0..1, beatsPerSecond = tempo (0 = unknown)
void bp_set(void *effect, float color, float speed, float resonance, double beatsPerSecond) { ((BarberPole *)effect)->setParams(color, speed, resonance, beatsPerSecond); }
// process an interleaved stereo buffer of frameCount frames in place
void bp_process(void *effect, float *buffer, int frameCount) { ((BarberPole *)effect)->process(buffer, frameCount); }
// target sweep rate in octaves per second
double bp_rate(void *effect) { return ((BarberPole *)effect)->octavesPerSecond(); }
}
