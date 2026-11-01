// Checks that the VST3 processor sounds exactly like the DSP core that the VirtualDJ plugin uses.
//
// It plays the same noise through BarberPoleProcessor (as a host would: separate channel
// buffers, parameter changes, tempo) and through barberpole::BarberPole directly (as VirtualDJ
// does: interleaved buffers, setParams once per block), turning the Colour knob both ways
// and back to centre, and compares the output sample by sample.
//
// Built and run by vst3/CMakeLists.txt (target compare_with_dsp). Prints PASS/FAIL per check
// and exits non-zero if any check fails.

#include "BarberPoleIds.h"
#include "BarberPoleProcessor.h"
#include "barberpole_dsp.h"

#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace barberpole_vst3;

namespace {

const int    kSampleRate  = 44100;
const int    kBlockFrames = 512;
const double kTempoBpm    = 128.0;
const int    kBlockCount  = 1200;   // about 14 s

int failureCount = 0;

void report(bool passed, const char* check, double worstDifference)
{
    std::printf("%s  %s (largest difference %.3g)\n", passed ? "PASS" : "FAIL", check, worstDifference);
    if (!passed) ++failureCount;
}

// Colour knob over the test: centre, slowly full right, back to centre, full left, centre.
float colourAtBlock(int block)
{
    const double position = (double)block / kBlockCount;   // 0..1
    return (float)(0.5 + 0.5 * std::sin(position * 2.0 * 3.14159265358979));
}

// Stereo noise, deterministic so every run uses the same input.
void makeNoise(std::vector<float>& left, std::vector<float>& right)
{
    std::mt19937 generator(12345);
    std::uniform_real_distribution<float> sample(-0.5f, 0.5f);
    for (size_t i = 0; i < left.size(); ++i) { left[i] = sample(generator); right[i] = sample(generator); }
}

// Runs the input through the DSP the way the VirtualDJ plugin does.
void runReference(const std::vector<float>& left, const std::vector<float>& right,
                  std::vector<float>& outLeft, std::vector<float>& outRight)
{
    barberpole::BarberPole effect;
    effect.prepare(kSampleRate);
    std::vector<float> interleaved(kBlockFrames * 2);
    for (int block = 0; block < kBlockCount; ++block) {
        const size_t start = (size_t)block * kBlockFrames;
        for (int i = 0; i < kBlockFrames; ++i) {
            interleaved[2 * i] = left[start + i];
            interleaved[2 * i + 1] = right[start + i];
        }
        effect.setParams(colourAtBlock(block), (float)kDefaultSpeed, (float)kDefaultResonance, kTempoBpm / 60.0);
        effect.process(interleaved.data(), kBlockFrames);
        for (int i = 0; i < kBlockFrames; ++i) {
            outLeft[start + i] = interleaved[2 * i];
            outRight[start + i] = interleaved[2 * i + 1];
        }
    }
}

// Runs the input through the VST3 processor the way a host does. hostBlockFrames can be larger
// than the promised maximum, to check that oversized blocks are handled in chunks.
template <typename SampleType>
void runProcessor(const std::vector<float>& left, const std::vector<float>& right,
                  std::vector<float>& outLeft, std::vector<float>& outRight,
                  int32 sampleSize, int promisedMaxFrames)
{
    BarberPoleProcessor* processor = new BarberPoleProcessor;
    processor->initialize(nullptr);
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    processor->setBusArrangements(&stereo, 1, &stereo, 1);
    ProcessSetup setup {kRealtime, sampleSize, promisedMaxFrames, (SampleRate)kSampleRate};
    processor->setupProcessing(setup);
    processor->setActive(true);

    std::vector<SampleType> inL(kBlockFrames), inR(kBlockFrames), outL(kBlockFrames), outR(kBlockFrames);
    SampleType* inputChannels[2] = {inL.data(), inR.data()};
    SampleType* outputChannels[2] = {outL.data(), outR.data()};

    AudioBusBuffers inputBus, outputBus;
    inputBus.numChannels = outputBus.numChannels = 2;
    if (sampleSize == kSample64) {
        inputBus.channelBuffers64 = (Sample64**)inputChannels;
        outputBus.channelBuffers64 = (Sample64**)outputChannels;
    } else {
        inputBus.channelBuffers32 = (Sample32**)inputChannels;
        outputBus.channelBuffers32 = (Sample32**)outputChannels;
    }

    ProcessContext context {};
    context.state = ProcessContext::kTempoValid;
    context.tempo = kTempoBpm;
    context.sampleRate = kSampleRate;

    for (int block = 0; block < kBlockCount; ++block) {
        const size_t start = (size_t)block * kBlockFrames;
        for (int i = 0; i < kBlockFrames; ++i) { inL[i] = left[start + i]; inR[i] = right[start + i]; }

        // the host sends the new knob position at the start of the block
        ParameterChanges changes;
        int32 queueIndex = 0, pointIndex = 0;
        changes.addParameterData(kColourId, queueIndex)->addPoint(0, colourAtBlock(block), pointIndex);

        ProcessData data;
        data.processMode = kRealtime;
        data.symbolicSampleSize = sampleSize;
        data.numSamples = kBlockFrames;
        data.numInputs = data.numOutputs = 1;
        data.inputs = &inputBus;
        data.outputs = &outputBus;
        data.inputParameterChanges = &changes;
        data.processContext = &context;
        processor->process(data);

        for (int i = 0; i < kBlockFrames; ++i) { outLeft[start + i] = (float)outL[i]; outRight[start + i] = (float)outR[i]; }
    }
    processor->setActive(false);
    processor->terminate();
    processor->release();
}

double largestDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i) worst = std::fmax(worst, std::fabs((double)a[i] - (double)b[i]));
    return worst;
}

// The saved state must load back the same knob values.
void checkStateRoundTrip()
{
    BarberPoleProcessor* saver = new BarberPoleProcessor;
    saver->initialize(nullptr);
    ParameterChanges changes;
    int32 queueIndex = 0, pointIndex = 0;
    changes.addParameterData(kColourId, queueIndex)->addPoint(0, 0.8, pointIndex);
    changes.addParameterData(kSpeedId, queueIndex)->addPoint(0, 0.3, pointIndex);
    changes.addParameterData(kResonanceId, queueIndex)->addPoint(0, 0.7, pointIndex);
    ProcessData data;
    data.inputParameterChanges = &changes;   // no audio: parameters only
    saver->process(data);

    MemoryStream saved;
    saver->getState(&saved);
    saved.seek(0, IBStream::kIBSeekSet, nullptr);

    BarberPoleProcessor* loader = new BarberPoleProcessor;
    loader->initialize(nullptr);
    const bool loaded = loader->setState(&saved) == kResultOk;
    MemoryStream reloaded;
    loader->getState(&reloaded);

    const bool same = loaded && saved.getSize() == reloaded.getSize() &&
                      std::equal(saved.getData(), saved.getData() + saved.getSize(), reloaded.getData());
    report(same, "saved state loads back the same values", 0.0);
    saver->terminate(); saver->release();
    loader->terminate(); loader->release();
}

} // namespace

int main()
{
    const size_t totalFrames = (size_t)kBlockCount * kBlockFrames;
    std::vector<float> left(totalFrames), right(totalFrames);
    makeNoise(left, right);

    std::vector<float> refLeft(totalFrames), refRight(totalFrames);
    runReference(left, right, refLeft, refRight);

    std::vector<float> vstLeft(totalFrames), vstRight(totalFrames);

    runProcessor<Sample32>(left, right, vstLeft, vstRight, kSample32, kBlockFrames);
    double worst = std::fmax(largestDifference(refLeft, vstLeft), largestDifference(refRight, vstRight));
    report(worst == 0.0, "32-bit host: bit-identical to the DSP core", worst);

    // The host promised blocks of 100 but sends 512: processed in chunks, same result
    runProcessor<Sample32>(left, right, vstLeft, vstRight, kSample32, 100);
    worst = std::fmax(largestDifference(refLeft, vstLeft), largestDifference(refRight, vstRight));
    report(worst == 0.0, "oversized host blocks: bit-identical to the DSP core", worst);

    // 64-bit hosts go through the DSP at 32-bit precision, so this must match too
    runProcessor<Sample64>(left, right, vstLeft, vstRight, kSample64, kBlockFrames);
    worst = std::fmax(largestDifference(refLeft, vstLeft), largestDifference(refRight, vstRight));
    report(worst == 0.0, "64-bit host: identical to the DSP core", worst);

    // The test turns the knob, so the output must differ from the input somewhere
    worst = std::fmax(largestDifference(left, vstLeft), largestDifference(right, vstRight));
    report(worst > 0.01, "the effect changes the sound when the knob is turned", worst);

    checkStateRoundTrip();

    std::printf("%s\n", failureCount == 0 ? "All checks passed." : "Some checks FAILED.");
    return failureCount == 0 ? 0 : 1;
}
