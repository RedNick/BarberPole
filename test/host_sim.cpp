// host_sim: loads the built plugin the way VirtualDJ does (dlopen + DllGetClassObject),
// declares its parameters through a fake host, checks the parameter texts, and runs noise
// through it checking for NaNs.
//
// Usage: host_sim <path to the built plugin>
// Build with -DREAL_SDK against the real VirtualDJ SDK headers (the fake host then also
// implements the callbacks only the real SDK declares).
#include "vdjDsp8.h"
#include <dlfcn.h>
#include <cstdio>
#include <cmath>
#include <vector>

// Minimal stand-in for VirtualDJ: remembers where each declared parameter lives so the test
// can set it, the way the host does when you turn a knob.
struct FakeHost : IVdjCallbacks8 {
  std::vector<float*> params;   // in declaration order: Color, Speed, Resonance
#ifdef REAL_SDK
  HRESULT SendCommand(const char *) override { return S_OK; }
  HRESULT GetInfo(const char *, double *result) override { *result = 0; return S_OK; }
  HRESULT GetStringInfo(const char *, void *, int) override { return S_OK; }
  HRESULT GetSongBuffer(int, int, short **) override { return E_NOTIMPL; }
#endif
  HRESULT DeclareParameter(void *param, int type, int id, const char *name, const char *, float defaultValue) override {
    printf("  param %d '%s' type %d default %.2f\n", id, name, type, defaultValue);
    *(float*)param = defaultValue;
    params.push_back((float*)param);
    return S_OK;
  }
};

int main(int argc, char **argv) {
  void *library = dlopen(argv[1], RTLD_NOW);
  if (!library) { printf("dlopen: %s\n", dlerror()); return 1; }
  auto getClassObject = (HRESULT(*)(const GUID&, const GUID&, void**))dlsym(library, "DllGetClassObject");
  if (!getClassObject) { printf("DllGetClassObject not exported\n"); return 1; }

  // must create the DSP plugin, and refuse any other interface
  void *object = nullptr;
  if (getClassObject(CLSID_VdjPlugin8, IID_IVdjPluginDsp8, &object) != NO_ERROR || !object) { printf("refused DSP IID\n"); return 1; }
  void *unexpected = nullptr;
  GUID wrongInterface = IID_IVdjPluginDsp8;
  wrongInterface.Data1 ^= 1;
  if (getClassObject(CLSID_VdjPlugin8, wrongInterface, &unexpected) != CLASS_E_CLASSNOTAVAILABLE) { printf("accepted wrong IID\n"); return 1; }

  auto *plugin = (IVdjPluginDsp8*)object;
  FakeHost host;
  plugin->cb = &host;
  plugin->SampleRate = 48000;
  plugin->SongBpm = 48000 * 60 / 126;   // the SDK's "SongBpm" is samples per beat: 126 BPM at 48 kHz
  plugin->OnLoad();
  TVdjPluginInfo8 info{};
  plugin->OnGetPluginInfo(&info);
  printf("  name '%s' v%s\n", info.PluginName, info.Version);
  plugin->OnStart();

  // parameter texts for a few knob positions
  char text[64];
  float colorValues[] = {0.5f, 0.51f, 0.6f, 1.0f, 0.3f, 0.0f};
  for (float color : colorValues) {
    *host.params[0] = color;
    plugin->OnParameter(0);
    plugin->OnGetParameterString(0, text, 64);
    printf("  color %.2f -> %s\n", color, text);
  }
  plugin->OnGetParameterString(1, text, 64); printf("  speed -> %s\n", text);
  plugin->OnGetParameterString(2, text, 64); printf("  resonance -> %s\n", text);

  // 400 blocks of white noise with the knob well to the right
  *host.params[0] = 0.85f;
  std::vector<float> buffer(2*512);
  double peak = 0, energyIn = 0, energyOut = 0;
  unsigned seed = 1;
  for (int block = 0; block < 400; ++block) {
    for (auto &sample : buffer) {
      seed = seed*1664525u+1013904223u;   // small LCG: same noise on every platform
      sample = ((seed>>9)/8388608.0f - 1.0f)*0.3f;
      energyIn += sample*sample;
    }
    plugin->OnProcessSamples(buffer.data(), 512);
    for (auto sample : buffer) {
      if (!std::isfinite(sample)) { printf("NaN!\n"); return 1; }
      peak = std::fmax(peak, std::fabs(sample));
      energyOut += sample*sample;
    }
  }
  printf("  processed 400 blocks: peak %.3f, level change %+.2f dB\n", peak, 10*std::log10(energyOut/energyIn));
  plugin->OnStop();
  plugin->Release();
  dlclose(library);
  printf("HOST SIM OK\n");
  return 0;
}
