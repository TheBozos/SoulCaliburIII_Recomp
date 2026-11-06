#pragma once

class PS2Runtime;

namespace sc3
{
    // The game's off-screen GS work done natively at its place in the frame, from the game's own variables at its builders
    // (RECOMP_NOTES "Native off-screen passes"): the glow (SC3_NATIVE_GLOW), the shadow volumes (SC3_NATIVE_SHADOWS) and
    // the sun's depth probe (SC3_NATIVE_FLARE); each 1, or @<file> following the file's first line. Needs the native 2D
    // (its GIF chain markers).
    void installOffscreen(PS2Runtime &runtime, bool nativeGs);

    // The glow is drawn natively now (its half picture is the native renderer's, not GS memory's).
    bool nativeGlowOn();
}
