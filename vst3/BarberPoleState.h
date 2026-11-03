// Barber Pole VST3: the saved-state format, shared by the processor (which saves and loads it)
// and the controller (which reads it to show the right knob positions).
//
// Layout, little-endian: int32 format version, then float colour, speed, resonance (0..1),
// then (from version 2) float strength. Version 1 states predate the Strength slider, so they
// load with strength 0, the Traktor-matched sound they were saved with.
#pragma once

#include "BarberPoleIds.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"

namespace barberpole_vst3 {

struct SavedState {
    static const Steinberg::int32 kFormatVersion = 2;

    float colour    = (float)kDefaultColour;
    float speed     = (float)kDefaultSpeed;
    float resonance = (float)kDefaultResonance;
    float strength  = (float)kDefaultStrength;

    bool read(Steinberg::IBStream* stream) {
        if (!stream) return false;
        Steinberg::IBStreamer streamer(stream, kLittleEndian);
        Steinberg::int32 version = 0;
        if (!streamer.readInt32(version) || version < 1) return false;
        if (!(streamer.readFloat(colour) && streamer.readFloat(speed) && streamer.readFloat(resonance))) return false;
        if (version < 2) { strength = 0.0f; return true; }
        return streamer.readFloat(strength);
    }

    bool write(Steinberg::IBStream* stream) const {
        if (!stream) return false;
        Steinberg::IBStreamer streamer(stream, kLittleEndian);
        return streamer.writeInt32(kFormatVersion) && streamer.writeFloat(colour) &&
               streamer.writeFloat(speed) && streamer.writeFloat(resonance) && streamer.writeFloat(strength);
    }
};

} // namespace barberpole_vst3
