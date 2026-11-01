// Barber Pole VST3: the plugin factory. The host reads this to find the processor and
// controller classes in the .vst3 bundle.

#include "BarberPoleIds.h"
#include "BarberPoleProcessor.h"
#include "BarberPoleController.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace barberpole_vst3;

// No contact e-mail on purpose: the project's GitHub page is the contact point.
BEGIN_FACTORY_DEF(BARBERPOLE_VST3_VENDOR, BARBERPOLE_VST3_URL, "")

    // The audio processor
    DEF_CLASS2(INLINE_UID_FROM_FUID(kProcessorUID),
               PClassInfo::kManyInstances,
               kVstAudioEffectClass,
               BARBERPOLE_VST3_NAME,
               Vst::kDistributable,
               "Fx|Filter",                 // where hosts list it
               BARBERPOLE_VST3_VERSION,
               kVstVersionString,
               BarberPoleProcessor::createInstance)

    // The controller (parameters and their display text)
    DEF_CLASS2(INLINE_UID_FROM_FUID(kControllerUID),
               PClassInfo::kManyInstances,
               kVstComponentControllerClass,
               BARBERPOLE_VST3_NAME " Controller",
               0,
               "",
               BARBERPOLE_VST3_VERSION,
               kVstVersionString,
               BarberPoleController::createInstance)

END_FACTORY
