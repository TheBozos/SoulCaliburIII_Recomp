// SC3's port on BozoEngine: what every port file includes. The context helpers (bozo/context.h), the generated
// functions by name (sc3_functions.h, made by bozo-recomp from SLUS_212.16 with SC3's externals) and the port API
// (bozo/port.h). Each subsystem registers its overrides from its own install(), called by applySc3 (sc3_port.cpp).
#pragma once

#include "bozo/context.h"
#include "bozo/port.h"
#include "bozo/recomp.h"
#include "sc3_functions.h"

using PS2Runtime = Runtime;

namespace sc3
{
    using Registry = bozo::port::Registry;

    constexpr uint32_t kEntry = 0x100008u;

    namespace frame { void install(Registry &runtime); }
    namespace ot
    {
        void install(Registry &runtime);
        void kick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        void dropFrame(uint8_t *rdram);
    }
    namespace twoD { void install(Registry &runtime); }
    namespace meshes { void install(Registry &runtime); }
    namespace textures { void install(Registry &runtime); }
    namespace vu0 { void install(Registry &runtime); }
    namespace movies { void install(Registry &runtime); }
    namespace audio { void install(Registry &runtime); }
}
