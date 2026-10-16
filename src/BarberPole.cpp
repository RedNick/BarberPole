// Barber Pole - Traktor-style dual-pole filter + endless Shepard sweep, Color FX for VirtualDJ 8+
//
// Knob centre : no effect at all (bit-exact, with a small dead zone for knob jitter)
// Knob right  : high-pass filter + notches that climb forever (build-ups)
// Knob left   : low-pass filter  + notches that fall forever (drops / breakdowns)
// Further from centre = stronger filter, deeper and slightly faster sweep.
// Speed       : sweep rate, synced to the track tempo (beats per octave)
// Resonance   : notch sharpness (soft/wide -> vocal/narrow)

#include "BarberPole.h"
#include <string.h>
#include <math.h>

HRESULT VDJ_API CBarberPole::OnLoad()
{
    // Registers this as a proper Color FX: VirtualDJ drives it from the filter/color knob
    // (0.5 = 12 o'clock) and lists the effect in the Color FX menu.
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
    info->Version     = "2.3";
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
    // SongBpm is "samples per beat" in the SDK; 0 when the track has no tempo.
    if (SongBpm > 0 && SampleRate > 0) return (double)SampleRate / (double)SongBpm;
    return 0.0;   // DSP falls back to 120 BPM
}

HRESULT VDJ_API CBarberPole::OnGetParameterString(int id, char *out, int outSize)
{
    switch (id) {
    case ID_COLOR: {
        const double c = ((double)m_Color - 0.5) * 2.0;
        const double m = fabs(c);
        if (m <= barberpole::BarberPole::kDeadZone) { snprintf(out, outSize, "OFF"); return S_OK; }
        const double t = (m - barberpole::BarberPole::kDeadZone) / (1.0 - barberpole::BarberPole::kDeadZone);
        snprintf(out, outSize, "%s %.0f%%", c > 0 ? "UP / HPF" : "DOWN / LPF", t * 100.0);
        return S_OK;
    }
    case ID_SPEED: {
        // rises a little slower than it falls (like Traktor)
        const double up = barberpole::BarberPole::beatsPerOctave(m_Speed, true);
        const double down = barberpole::BarberPole::beatsPerOctave(m_Speed, false);
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
    if (SampleRate != m_PreparedRate) {
        m_Dsp.prepare(SampleRate);
        m_PreparedRate = SampleRate;
    } else {
        m_Dsp.reset();
    }
    return S_OK;
}

HRESULT VDJ_API CBarberPole::OnStop()
{
    return S_OK;
}

HRESULT VDJ_API CBarberPole::OnProcessSamples(float *buffer, int nb)
{
    if (SampleRate != m_PreparedRate) {   // engine rate changed while running
        m_Dsp.prepare(SampleRate);
        m_PreparedRate = SampleRate;
    }
    m_Dsp.setParams(m_Color, m_Speed, m_Resonance, beatsPerSecond());
    m_Dsp.process(buffer, nb);            // interleaved stereo, nb frames
    return S_OK;
}

// ---------------------------------------------------------------------------
// Plugin entry point
HRESULT VDJ_API DllGetClassObject(const GUID &rclsid, const GUID &riid, void **ppObject)
{
    if (memcmp(&rclsid, &CLSID_VdjPlugin8, sizeof(GUID)) == 0 &&
        memcmp(&riid, &IID_IVdjPluginDsp8, sizeof(GUID)) == 0) {
        *ppObject = new CBarberPole();
        return NO_ERROR;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}
