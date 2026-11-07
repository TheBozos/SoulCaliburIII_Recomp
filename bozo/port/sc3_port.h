// SC3's port on BozoEngine: what every port file includes. The context helpers (bozo/context.h), the generated
// functions by name (sc3_functions.h, made by bozo-recomp from SLUS_212.16 with SC3's externals) and the port API
// (bozo/port.h). Each subsystem registers its overrides from its own install(), called by applySc3 (sc3_port.cpp).
#pragma once

#include "bozo/context.h"
#include "bozo/port.h"
#include "bozo/recomp.h"
#include "sc3_functions.h"

#include <vector>

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
        // A frame with no kick (a movie's): presents what the renderer holds.
        void presentAlone();
    }
    namespace twoD { void install(Registry &runtime); }
    namespace meshes { void install(Registry &runtime); }
    namespace textures { void install(Registry &runtime); }
    namespace vu0 { void install(Registry &runtime); }
    namespace movies
    {
        void install(Registry &runtime);
        // At every frame end: the movies' clocks, pictures and soundtracks.
        void tick(PS2Runtime *runtime);
        // A smaller movie's current picture by Sofdec handle, for the 2D builders: RGBA rows (alpha 0x80 = 1.0), nothing copied while `known` is
        // its number.
        bool picture(PS2Runtime *runtime, uint32_t handle, uint32_t known, std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height, uint32_t &number);
    }
    namespace audio { void install(Registry &runtime); }
}
