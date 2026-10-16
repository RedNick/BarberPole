// MINIMAL STAND-IN for compile-testing on Linux only. NOT the real SDK - do not ship.
#pragma once
#include "vdjPlugin8.h"
static const GUID IID_IVdjPluginDsp8 = {0x7cfcf3f5, 0x6fb9, 0x434c, {0xb6,0x03,0xd7,0x3a,0x88,0xf6,0x72,0x26}};
class IVdjPluginDsp8 : public IVdjPlugin8 { public:
  virtual HRESULT VDJ_API OnStart() { return 0; }
  virtual HRESULT VDJ_API OnStop() { return 0; }
  virtual HRESULT VDJ_API OnProcessSamples(float *buffer, int nb) = 0;
  int SampleRate; int SongBpm; double SongPosBeats;
};
