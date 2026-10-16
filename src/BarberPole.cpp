// Barber Pole: VirtualDJ 8+ Color FX wrapper around the DSP core (barberpole_dsp.h).
//
// Knob centre : no effect at all (bit-exact, with a small dead zone for knob jitter)
// Knob right  : high-pass filter + notches that climb forever (build-ups)
// Knob left   : low-pass filter  + notches that fall forever (breakdowns)
// Further from centre = stronger filter.
// Speed       : sweep rate, synced to the track tempo (beats per octave)
// Resonance   : notch sharpness (soft/wide -> vocal/narrow)
//
// This file only translates between VirtualDJ's plugin API and the DSP class: parameters,
// their on-screen text, tempo, and start/stop.

#include "BarberPole.h"
#include <string.h>
#include <math.h>

using barberpole::BarberPole;

HRESULT VDJ_API CBarberPole::OnLoad()
{
    // Declared as a real Color FX: VirtualDJ drives it from the mixer's filter/color knob
    // (0.5 = 12 o'clock) and lists the effect in the Color FX menu.
    // The IDs and names are what VirtualDJ saves settings and mappings under; don't change them.
    DeclareParameterColorFX(&m_Color,    ID_COLOR,     "Color",     "CLR");
    DeclareParameterSlider(&m_Speed,     ID_SPEED,     "Speed",     "SPD",  0.5f);
    DeclareParameterSlider(&m_Resonance, ID_RESONANCE, "Resonance", "RES",  0.4f);
    return S_OK;
}

HRESULT VDJ_API CBarberPole::OnGetPluginInfo(TVdjPluginInfo8 *info)
{
    info->PluginName  = "Barber Pole";
    info->Author      = "Barber Pole project";
    info->Description = "Traktor-style Barber Pole: filter + endless rising/falling sweep. Centre = off.";
    info->Version     = "2.4.2";
    info->Flags       = 0x00;
    info->Bitmap      = NULL;
    return S_OK;
}

ULONG VDJ_API CBarberPole::Release()
{
    delete this;
    return 0;
}

HRESULT VDJ_API CBarberPole::OnParameter(int /*id*/)
{
    return S_OK;   // values are read every audio block in OnProcessSamples
}

double CBarberPole::beatsPerSecond() const
{
    // The SDK's SongBpm is actually "samples per beat", and 0 when the track has no tempo.
    if (SongBpm > 0 && SampleRate > 0) return (double)SampleRate / (double)SongBpm;
    return 0.0;   // the DSP falls back to 120 BPM
}

// Text VirtualDJ shows next to each parameter.
HRESULT VDJ_API CBarberPole::OnGetParameterString(int id, char *out, int outSize)
{
    switch (id) {
    case ID_COLOR: {
        // same knob mapping as BarberPole::setParams
        const double bipolar = ((double)m_Color - 0.5) * 2.0;   // -1 (left) .. +1 (right)
        const double fromCentre = fabs(bipolar);
        if (fromCentre <= BarberPole::kDeadZone) { snprintf(out, outSize, "OFF"); return S_OK; }
        const double turn = (fromCentre - BarberPole::kDeadZone) / (1.0 - BarberPole::kDeadZone);
        snprintf(out, outSize, "%s %.0f%%", bipolar > 0 ? "UP / HPF" : "DOWN / LPF", turn * 100.0);
        return S_OK;
    }
    case ID_SPEED: {
        // it rises a little slower than it falls (like Traktor)
        const double up = BarberPole::beatsPerOctave(m_Speed, true);
        const double down = BarberPole::beatsPerOctave(m_Speed, false);
        snprintf(out, outSize, "%.1f up / %.1f down beats/oct", up, down);
        return S_OK;
    }
    case ID_RESONANCE:
        snprintf(out, outSize, "%.0f%%", m_Resonance * 100.0f);
        return S_OK;
    }
    return E_NOTIMPL;
}

HRESULT VDJ_API CBarberPole::OnStart()
{
    if (SampleRate != m_PreparedSampleRate) {
        m_Effect.prepare(SampleRate);
        m_PreparedSampleRate = SampleRate;
    } else {
        m_Effect.reset();
    }
    return S_OK;
}

HRESULT VDJ_API CBarberPole::OnStop()
{
    return S_OK;
}

HRESULT VDJ_API CBarberPole::OnProcessSamples(float *buffer, int frameCount)
{
    if (SampleRate != m_PreparedSampleRate) {   // the engine's sample rate changed while running
        m_Effect.prepare(SampleRate);
        m_PreparedSampleRate = SampleRate;
    }
    m_Effect.setParams(m_Color, m_Speed, m_Resonance, beatsPerSecond());
    m_Effect.process(buffer, frameCount);       // interleaved stereo, processed in place
    return S_OK;
}

// ---------------------------------------------------------------------------
// Plugin entry point: VirtualDJ calls this to create the effect.
HRESULT VDJ_API DllGetClassObject(const GUID &rclsid, const GUID &riid, void **ppObject)
{
    if (memcmp(&rclsid, &CLSID_VdjPlugin8, sizeof(GUID)) == 0 &&
        memcmp(&riid, &IID_IVdjPluginDsp8, sizeof(GUID)) == 0) {
        *ppObject = new CBarberPole();
        return NO_ERROR;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}
