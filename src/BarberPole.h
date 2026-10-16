// Barber Pole - Traktor-style dual-pole filter + endless Shepard sweep, Color FX for VirtualDJ 8+
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
    HRESULT VDJ_API OnProcessSamples(float *buffer, int nb) override;

private:
    // The Color parameter is declared as VDJPARAM_COLORFX: VirtualDJ's Color FX knob
    // drives it (0.5 = knob at 12 o'clock = off).
    enum { ID_COLOR = 0, ID_SPEED, ID_RESONANCE };

    float m_Color = 0.5f;
    float m_Speed = 0.5f;       // 0..1 -> 8, 4, 2, 1, 1/2 beats per octave
    float m_Resonance = 0.4f;   // notch sharpness

    double beatsPerSecond() const;

    barberpole::BarberPole m_Dsp;
    int m_PreparedRate = 0;
};

#endif
