// SC3's sound modules from the disc (CRI_ADXI, NUSOUND, SDRDRV), compiled to native code and run by BozoEngine's
// compiled-library host over native libsd: LIBSD.IRX is not translated, and PADMAN, SIO2MAN, MCMAN and MCSERV load as
// plain successes because the EE's libpad and libmc are answered natively.
#pragma once

#include "bozo/libs/libraries.h"
#include "bozo/native/compiled_libraries.h"

#include <string>
#include <vector>

namespace sc3::audio
{
    std::vector<bozo::native::irx::Image> images();
    bozo::native::irx::CompiledLibraries &attach(bozo::libs::Libraries &libraries);
    std::string moduleName(const std::string &path);
}
