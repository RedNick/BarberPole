// Barber Pole VST3: the audio processor. It owns the DSP (src/barberpole_dsp.h) and feeds it
// the host's audio, parameter changes and tempo.
#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "BarberPoleIds.h"
#include "barberpole_dsp.h"

#include <vector>

namespace barberpole_vst3 {

class BarberPoleProcessor : public Steinberg::Vst::AudioEffect {
public:
    BarberPoleProcessor();

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new BarberPoleProcessor);
    }

    // --- Set-up
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 inputCount,
                                                     Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 outputCount) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override;

    // --- Audio
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;

    // --- Saved state (the three knob values)
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

private:
    void readParameterChanges(Steinberg::Vst::IParameterChanges* changes);
    static double beatsPerSecond(const Steinberg::Vst::ProcessContext* context);

    // Runs the DSP over one block of the host's separate left/right channel buffers.
    // SampleType is float (32-bit hosts) or double (64-bit hosts).
    template <typename SampleType>
    void processBlock(SampleType** inputs, SampleType** outputs, Steinberg::int32 frameCount);

    // Knob values, all 0..1, as last sent by the host.
    float colour_    = (float)kDefaultColour;
    float speed_     = (float)kDefaultSpeed;
    float resonance_ = (float)kDefaultResonance;
    float strength_  = (float)kDefaultStrength;

    barberpole::BarberPole effect_;
    int preparedSampleRate_ = 0;   // sample rate effect_ was last prepared for

    // The DSP works on interleaved stereo (L R L R ...), the way VirtualDJ delivers audio,
    // while VST3 hosts give separate channel buffers. Audio is copied through this buffer,
    // sized in setupProcessing() so process() never allocates.
    std::vector<float> interleaved_;
};

} // namespace barberpole_vst3
