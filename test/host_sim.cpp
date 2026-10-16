// Loads the built plugin like VirtualDJ does (dlopen + DllGetClassObject), declares
// parameters through a fake host, and runs audio through it.
#include "vdjDsp8.h"
#include <dlfcn.h>
#include <cstdio>
#include <cmath>
#include <vector>
struct Host : IVdjCallbacks8 {
  std::vector<float*> params;
#ifdef REAL_SDK
  HRESULT SendCommand(const char *) override { return S_OK; }
  HRESULT GetInfo(const char *, double *r) override { *r = 0; return S_OK; }
  HRESULT GetStringInfo(const char *, void *, int) override { return S_OK; }
  HRESULT GetSongBuffer(int, int, short **) override { return E_NOTIMPL; }
#endif
  HRESULT DeclareParameter(void *p, int type, int id, const char *n, const char *, float d) override {
    printf("  param %d '%s' type %d default %.2f\n", id, n, type, d); *(float*)p = d; params.push_back((float*)p); return S_OK; }
};
int main(int argc, char **argv) {
  void *h = dlopen(argv[1], RTLD_NOW); if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  auto get = (HRESULT(*)(const GUID&, const GUID&, void**))dlsym(h, "DllGetClassObject");
  if (!get) { printf("DllGetClassObject not exported\n"); return 1; }
  void *obj = nullptr;
  if (get(CLSID_VdjPlugin8, IID_IVdjPluginDsp8, &obj) != NO_ERROR || !obj) { printf("refused DSP IID\n"); return 1; }
  void *bad = nullptr; GUID wrong = IID_IVdjPluginDsp8; wrong.Data1 ^= 1;
  if (get(CLSID_VdjPlugin8, wrong, &bad) != CLASS_E_CLASSNOTAVAILABLE) { printf("accepted wrong IID\n"); return 1; }
  auto *p = (IVdjPluginDsp8*)obj; Host host; p->cb = &host; p->SampleRate = 48000; p->SongBpm = 48000 * 60 / 126;
  p->OnLoad(); TVdjPluginInfo8 info{}; p->OnGetPluginInfo(&info); printf("  name '%s' v%s\n", info.PluginName, info.Version);
  p->OnStart();
  char s[64]; float vals[] = {0.5f, 0.51f, 0.6f, 1.0f, 0.3f, 0.0f};
  for (float v : vals) { *host.params[0] = v; p->OnParameter(0); p->OnGetParameterString(0, s, 64); printf("  color %.2f -> %s\n", v, s); }
  p->OnGetParameterString(1, s, 64); printf("  speed -> %s\n", s);
  p->OnGetParameterString(2, s, 64); printf("  resonance -> %s\n", s);
  *host.params[0] = 0.85f; std::vector<float> buf(2*512); double peak = 0, rmsIn = 0, rmsOut = 0; unsigned seed = 1;
  for (int b = 0; b < 400; ++b) {
    for (auto &x : buf) { seed = seed*1664525u+1013904223u; x = ((seed>>9)/8388608.0f - 1.0f)*0.3f; rmsIn += x*x; }
    p->OnProcessSamples(buf.data(), 512);
    for (auto x : buf) { if (!std::isfinite(x)) { printf("NaN!\n"); return 1; } peak = std::fmax(peak, std::fabs(x)); rmsOut += x*x; }
  }
  printf("  processed 400 blocks: peak %.3f, level change %+.2f dB\n", peak, 10*std::log10(rmsOut/rmsIn));
  p->OnStop(); p->Release(); dlclose(h); printf("HOST SIM OK\n"); return 0;
}
