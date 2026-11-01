// Barber Pole VST3: the audio processor.
//
// Like src/BarberPole.cpp for VirtualDJ, this file only translates between the plugin API and
// the DSP class. The sound comes entirely from barberpole::BarberPole, so a 32-bit float host
// gets exactly the same output as VirtualDJ for the same knob settings and tempo.

#include "BarberPoleProcessor.h"
#include "BarberPoleIds.h"
#include "BarberPoleState.h"

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace barberpole_vst3 {

BarberPoleProcessor::BarberPoleProcessor()
{
    // Tells the host which controller class shows this processor's parameters.
    setControllerClass(kControllerUID);
}

tresult PLUGIN_API BarberPoleProcessor::initialize(FUnknown* context)
{
    const tresult result = AudioEffect::initialize(context);
    if (result != kResultOk) return result;

    // One stereo input and one stereo output: the DSP is built for stereo.
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    return kResultOk;
}

tresult PLUGIN_API BarberPoleProcessor::setBusArrangements(SpeakerArrangement* inputs, int32 inputCount,
                                                           SpeakerArrangement* outputs, int32 outputCount)
{
    // Only stereo in, stereo out. Refusing anything else makes the host fall back to stereo.
    if (inputCount == 1 && outputCount == 1 &&
        inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo)
        return AudioEffect::setBusArrangements(inputs, inputCount, outputs, outputCount);
    return kResultFalse;
}

tresult PLUGIN_API BarberPoleProcessor::canProcessSampleSize(int32 symbolicSampleSize)
{
    // 64-bit hosts are supported too; their audio goes through the DSP at 32-bit precision.
    return (symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API BarberPoleProcessor::setupProcessing(ProcessSetup& setup)
{
    // Room for the largest block the host will send, two channels interleaved.
    interleaved_.assign((size_t)std::max<int32>(setup.maxSamplesPerBlock, 1) * 2, 0.0f);
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API BarberPoleProcessor::setActive(TBool state)
{
    if (state) {
        // Same as VirtualDJ's OnStart: a new sample rate re-prepares the DSP,
        // otherwise the filters and sweep position are just cleared.
        const int sampleRate = (int)processSetup.sampleRate;
        if (sampleRate != preparedSampleRate_) {
            effect_.prepare(sampleRate);
            preparedSampleRate_ = sampleRate;
        } else {
            effect_.reset();
        }
    }
    return AudioEffect::setActive(state);
}

uint32 PLUGIN_API BarberPoleProcessor::getTailSamples()
{
    return (uint32)(processSetup.sampleRate * kTailSeconds);
}

// Takes the last value the host sent for each parameter in this block. The DSP smooths every
// change over 25 ms, so per-block values are enough (VirtualDJ also updates once per block).
void BarberPoleProcessor::readParameterChanges(IParameterChanges* changes)
{
    if (!changes) return;
    const int32 changedCount = changes->getParameterCount();
    for (int32 i = 0; i < changedCount; ++i) {
        IParamValueQueue* queue = changes->getParameterData(i);
        if (!queue || queue->getPointCount() <= 0) continue;
        int32 sampleOffset = 0;
        ParamValue value = 0.0;
        if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, value) != kResultTrue) continue;
        switch (queue->getParameterId()) {
        case kColourId:    colour_    = (float)value; break;
        case kSpeedId:     speed_     = (float)value; break;
        case kResonanceId: resonance_ = (float)value; break;
        default: break;
        }
    }
}

// The host's tempo in beats per second, or 0 if it doesn't send one (the DSP then uses 120 BPM).
double BarberPoleProcessor::beatsPerSecond(const ProcessContext* context)
{
    if (context && (context->state & ProcessContext::kTempoValid) && context->tempo > 0.0)
        return context->tempo / 60.0;
    return 0.0;
}

template <typename SampleType>
void BarberPoleProcessor::processBlock(SampleType** inputs, SampleType** outputs, int32 frameCount)
{
    // The host may hand over blocks bigger than it promised, so work in chunks that fit.
    const int32 chunkCapacity = (int32)(interleaved_.size() / 2);
    for (int32 chunkStart = 0; chunkStart < frameCount; chunkStart += chunkCapacity) {
        const int32 chunkFrames = std::min(chunkCapacity, frameCount - chunkStart);

        // Separate left/right -> interleaved
        for (int32 frame = 0; frame < chunkFrames; ++frame) {
            interleaved_[2 * frame]     = (float)inputs[0][chunkStart + frame];
            interleaved_[2 * frame + 1] = (float)inputs[1][chunkStart + frame];
        }

        effect_.process(interleaved_.data(), chunkFrames);

        // Interleaved -> separate left/right (the output may be the same memory as the input,
        // which is fine because the input has already been copied)
        for (int32 frame = 0; frame < chunkFrames; ++frame) {
            outputs[0][chunkStart + frame] = (SampleType)interleaved_[2 * frame];
            outputs[1][chunkStart + frame] = (SampleType)interleaved_[2 * frame + 1];
        }
    }
}

tresult PLUGIN_API BarberPoleProcessor::process(ProcessData& data)
{
    readParameterChanges(data.inputParameterChanges);

    // A call with no audio only delivers parameter changes.
    if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
    AudioBusBuffers& input = data.inputs[0];
    AudioBusBuffers& output = data.outputs[0];
    if (input.numChannels < 2 || output.numChannels < 2) return kResultOk;

    // The host's sample rate changed without a setActive() call
    if ((int)processSetup.sampleRate != preparedSampleRate_) {
        preparedSampleRate_ = (int)processSetup.sampleRate;
        effect_.prepare(preparedSampleRate_);
    }

    effect_.setParams(colour_, speed_, resonance_, beatsPerSecond(data.processContext));

    if (data.symbolicSampleSize == kSample64)
        processBlock(input.channelBuffers64, output.channelBuffers64, data.numSamples);
    else
        processBlock(input.channelBuffers32, output.channelBuffers32, data.numSamples);

    output.silenceFlags = 0;   // the resonant tail can ring on after silent input
    return kResultOk;
}

tresult PLUGIN_API BarberPoleProcessor::setState(IBStream* state)
{
    SavedState saved;
    if (!saved.read(state)) return kResultFalse;
    colour_ = saved.colour;
    speed_ = saved.speed;
    resonance_ = saved.resonance;
    return kResultOk;
}

tresult PLUGIN_API BarberPoleProcessor::getState(IBStream* state)
{
    SavedState saved;
    saved.colour = colour_;
    saved.speed = speed_;
    saved.resonance = resonance_;
    return saved.write(state) ? kResultOk : kResultFalse;
}

} // namespace barberpole_vst3
