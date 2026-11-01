// Barber Pole VST3: identifiers, defaults and version strings shared by the processor,
// the controller and the factory (BarberPoleEntry.cpp).
#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace barberpole_vst3 {

// Parameter IDs. Hosts save automation, presets and project state by these numbers,
// so they must never change once released.
enum ParameterId : Steinberg::Vst::ParamID {
    kColourId    = 0,   // the main knob: 0.5 = centre = off
    kSpeedId     = 1,   // sweep speed
    kResonanceId = 2,   // notch sharpness
};

// Defaults, the same as the VirtualDJ plugin (src/BarberPole.cpp).
constexpr double kDefaultColour    = 0.5;
constexpr double kDefaultSpeed     = 0.5;
constexpr double kDefaultResonance = 0.4;

// Class IDs. Hosts identify the plugin by these; never change them once released.
static const Steinberg::FUID kProcessorUID (0x0667FD1C, 0x13AF4B0F, 0x807CC67A, 0x170EE717);
static const Steinberg::FUID kControllerUID(0x6A57DE00, 0xAFCE4C9C, 0x96A28B7A, 0x593B62A2);

// Shown in the host's plugin browser.
#define BARBERPOLE_VST3_NAME     "Barber Pole"
#define BARBERPOLE_VST3_VENDOR   "Barber Pole project"
#define BARBERPOLE_VST3_URL      "https://github.com/RedNick/BarberPole"
// BARBERPOLE_VST3_VERSION comes from project(... VERSION) in vst3/CMakeLists.txt.
#ifndef BARBERPOLE_VST3_VERSION
#error "BARBERPOLE_VST3_VERSION is set by vst3/CMakeLists.txt"
#endif

// How long the resonant notches and peaks can ring after the input goes silent. Hosts keep
// calling process() for this long, so the tail isn't cut off.
constexpr double kTailSeconds = 1.0;

} // namespace barberpole_vst3
