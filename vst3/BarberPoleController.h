// Barber Pole VST3: the edit controller. It declares the three parameters and the text the
// host shows for them. There's no custom editor window: hosts draw their own knob panel.
#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace barberpole_vst3 {

class BarberPoleController : public Steinberg::Vst::EditController {
public:
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IEditController*>(new BarberPoleController);
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;

    // Called with the processor's saved state when a project or preset loads,
    // so the knobs show the loaded values.
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
};

} // namespace barberpole_vst3
