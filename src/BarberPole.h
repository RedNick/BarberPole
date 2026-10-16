// Barber Pole: VirtualDJ 8+ Color FX plugin class (see BarberPole.cpp and barberpole_dsp.h).
#ifndef BARBERPOLE_PLUGIN_H
#define BARBERPOLE_PLUGIN_H

#define _CRT_SECURE_NO_WARNINGS
#ifndef NOMINMAX
#define NOMINMAX   // stop <windows.h> defining min/max macros
#endif
#include <stdio.h>
#include "vdjDsp8.h"
#include "barberpole_dsp.h"

class CBarberPole : public IVdjPluginDsp8 {
public:
    HRESULT VDJ_API OnLoad() override;
    HRESULT VDJ_API OnGetPluginInfo(TVdjPluginInfo8 *info) override;
    ULONG   VDJ_API Release() override;
    HRESULT VDJ_API OnParameter(int id) override;
    HRESULT VDJ_API OnGetParameterString(int id, char *out, int outSize) override;
    HRESULT VDJ_API OnStart() override;
    HRESULT VDJ_API OnStop() override;
    HRESULT VDJ_API OnProcessSamples(float *buffer, int frameCount) override;

private:
    // Parameter IDs. VirtualDJ stores settings and controller mappings by these numbers,
    // so they must never change.
    enum { ID_COLOR = 0, ID_SPEED, ID_RESONANCE };

    // Parameter values, written by VirtualDJ (all 0..1).
    float m_Color = 0.5f;       // Color FX knob: 0.5 = 12 o'clock = off
    float m_Speed = 0.5f;       // sweep speed: 0..1 -> 4x slower .. 4x faster than default
    float m_Resonance = 0.4f;   // notch sharpness

    double beatsPerSecond() const;   // track tempo, or 0 if unknown

    barberpole::BarberPole m_Effect;
    int m_PreparedSampleRate = 0;    // sample rate m_Effect was last prepared for
};

#endif
