#include "sc3_port.h"

namespace
{
    void applySc3(sc3::Registry &runtime)
    {
        sc3::frame::install(runtime);
        sc3::ot::install(runtime);
        sc3::twoD::install(runtime);
        sc3::meshes::install(runtime);
        sc3::textures::install(runtime);
        sc3::vu0::install(runtime);
        sc3::movies::install(runtime);
        sc3::audio::install(runtime);
    }
}

BOZO_PORT("Soulcalibur III (USA)", "SLUS_212.16", sc3::kEntry, &applySc3)
