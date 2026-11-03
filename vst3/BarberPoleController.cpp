// Barber Pole VST3: the edit controller.
//
// The display text matches what VirtualDJ shows (src/BarberPole.cpp, OnGetParameterString).

#include "BarberPoleController.h"
#include "BarberPoleIds.h"
#include "BarberPoleState.h"
#include "barberpole_dsp.h"

#include "pluginterfaces/base/ustring.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;
using barberpole::BarberPole;

namespace barberpole_vst3 {

namespace {

// Copies plain ASCII text into the host's UTF-16 string.
void setDisplayText(String128 out, const char* text)
{
    UString(out, 128).fromAscii(text);
}

// Copies the host's UTF-16 string into plain ASCII text, for parsing typed-in values.
void getTypedText(const TChar* in, char* text, int32 size)
{
    UString(const_cast<TChar*>(in), 128).toAscii(text, size);
}

// The first number in the text (skipping any words before it), or false if there is none.
bool findNumber(const char* text, double& number)
{
    for (const char* p = text; *p; ++p) {
        if ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') {
            char* end = nullptr;
            number = std::strtod(p, &end);
            if (end != p) return true;
        }
    }
    return false;
}

double clamp01(double value) { return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value); }

// Colour knob: "OFF" in the centre dead zone, otherwise the direction and how far it's turned.
class ColourParameter : public Parameter {
public:
    ColourParameter()
    : Parameter(STR16("Colour"), kColourId, nullptr, kDefaultColour, 0, ParameterInfo::kCanAutomate) {}

    void toString(ParamValue normalized, String128 out) const override {
        // same knob mapping as BarberPole::setParams
        const double bipolar = (normalized - 0.5) * 2.0;   // -1 (left) .. +1 (right)
        const double fromCentre = std::fabs(bipolar);
        char text[64];
        if (fromCentre <= BarberPole::kDeadZone) {
            std::snprintf(text, sizeof(text), "OFF");
        } else {
            const double turn = (fromCentre - BarberPole::kDeadZone) / (1.0 - BarberPole::kDeadZone);
            std::snprintf(text, sizeof(text), "%s %.0f%%", bipolar > 0 ? "UP / HPF" : "DOWN / LPF", turn * 100.0);
        }
        setDisplayText(out, text);
    }

    // Reads back the text above, so typed values work: "OFF", "UP / HPF 40%", "DOWN / LPF 25%",
    // or a signed percentage such as "-25" (left) or "40" (right).
    bool fromString(const TChar* typed, ParamValue& normalized) const override {
        char text[64];
        getTypedText(typed, text, sizeof(text));
        if (std::strstr(text, "OFF") || std::strstr(text, "off")) { normalized = 0.5; return true; }
        double percent = 0.0;
        if (!findNumber(text, percent)) return false;
        const bool left = std::strstr(text, "DOWN") || std::strstr(text, "LPF") || percent < 0.0;
        const double turn = clamp01(std::fabs(percent) / 100.0);
        if (turn <= 0.0) { normalized = 0.5; return true; }
        const double fromCentre = BarberPole::kDeadZone + turn * (1.0 - BarberPole::kDeadZone);
        normalized = clamp01(0.5 + (left ? -fromCentre : fromCentre) * 0.5);
        return true;
    }
};

// Speed slider: the sweep rate in beats per octave, rising and falling.
class SpeedParameter : public Parameter {
public:
    SpeedParameter()
    : Parameter(STR16("Speed"), kSpeedId, nullptr, kDefaultSpeed, 0, ParameterInfo::kCanAutomate) {}

    void toString(ParamValue normalized, String128 out) const override {
        // it rises a little slower than it falls (like Traktor)
        const double up = BarberPole::beatsPerOctave((float)normalized, true);
        const double down = BarberPole::beatsPerOctave((float)normalized, false);
        char text[64];
        std::snprintf(text, sizeof(text), "%.1f up / %.1f down beats/oct", up, down);
        setDisplayText(out, text);
    }

    // Reads back the rising rate (the first number), e.g. "2.8" or "2.8 up / 1.8 down beats/oct".
    bool fromString(const TChar* typed, ParamValue& normalized) const override {
        char text[64];
        getTypedText(typed, text, sizeof(text));
        double risingBeatsPerOctave = 0.0;
        if (!findNumber(text, risingBeatsPerOctave) || risingBeatsPerOctave <= 0.0) return false;
        // inverse of BarberPole::beatsPerOctave: 8 * 16^-speed * kRisingSpeedFactor
        const double speed = -std::log(risingBeatsPerOctave / (8.0 * BarberPole::kRisingSpeedFactor)) / std::log(16.0);
        normalized = clamp01(speed);
        return true;
    }
};

// Resonance slider: shown as a percentage.
class ResonanceParameter : public Parameter {
public:
    ResonanceParameter()
    : Parameter(STR16("Resonance"), kResonanceId, nullptr, kDefaultResonance, 0, ParameterInfo::kCanAutomate) {}

    void toString(ParamValue normalized, String128 out) const override {
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f%%", normalized * 100.0);
        setDisplayText(out, text);
    }

    // Reads back a percentage, e.g. "40%" or "40".
    bool fromString(const TChar* typed, ParamValue& normalized) const override {
        char text[64];
        getTypedText(typed, text, sizeof(text));
        double percent = 0.0;
        if (!findNumber(text, percent)) return false;
        normalized = clamp01(percent / 100.0);
        return true;
    }
};

// Strength slider: shown as a percentage, with 0 % marked as the Traktor-matched sound.
class StrengthParameter : public Parameter {
public:
    StrengthParameter()
    : Parameter(STR16("Strength"), kStrengthId, nullptr, kDefaultStrength, 0, ParameterInfo::kCanAutomate) {}

    void toString(ParamValue normalized, String128 out) const override {
        char text[24];
        if (normalized <= 0.0) std::snprintf(text, sizeof(text), "0%% (Traktor)");
        else std::snprintf(text, sizeof(text), "%.0f%%", normalized * 100.0);
        setDisplayText(out, text);
    }

    // Reads back a percentage, e.g. "50%" or "50".
    bool fromString(const TChar* typed, ParamValue& normalized) const override {
        char text[64];
        getTypedText(typed, text, sizeof(text));
        double percent = 0.0;
        if (!findNumber(text, percent)) return false;
        normalized = clamp01(percent / 100.0);
        return true;
    }
};

} // namespace

tresult PLUGIN_API BarberPoleController::initialize(FUnknown* context)
{
    const tresult result = EditController::initialize(context);
    if (result != kResultOk) return result;

    // The container owns and deletes these.
    parameters.addParameter(new ColourParameter);
    parameters.addParameter(new SpeedParameter);
    parameters.addParameter(new ResonanceParameter);
    parameters.addParameter(new StrengthParameter);
    return kResultOk;
}

tresult PLUGIN_API BarberPoleController::setComponentState(IBStream* state)
{
    SavedState saved;
    if (!saved.read(state)) return kResultFalse;
    setParamNormalized(kColourId, saved.colour);
    setParamNormalized(kSpeedId, saved.speed);
    setParamNormalized(kResonanceId, saved.resonance);
    setParamNormalized(kStrengthId, saved.strength);
    return kResultOk;
}

} // namespace barberpole_vst3
