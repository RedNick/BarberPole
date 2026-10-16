// C API over the DSP core so Python (ctypes) can drive it for analysis.
#include "../src/barberpole_dsp.h"
using namespace barberpole;
extern "C" {
void *bp_new(int sr) { auto *p = new BarberPole(); p->prepare(sr); return p; }
void bp_free(void *p) { delete (BarberPole *)p; }
void bp_set(void *p, float color, float speed, float reso, double bps) { ((BarberPole *)p)->setParams(color, speed, reso, bps); }
void bp_process(void *p, float *buf, int n) { ((BarberPole *)p)->process(buf, n); }
double bp_rate(void *p) { return ((BarberPole *)p)->octavesPerSecond(); }
}
