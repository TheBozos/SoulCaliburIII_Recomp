#pragma once

class PS2Runtime;

namespace sc3
{
    // The game's 2D builders rewritten in C++ from their MIPS, drawn natively from the game's own variables at their place
    // in the ordering table (SC3_NATIVE_2D=1, or =@<file> following the file's first line): RECOMP_NOTES "Native 2D".
    void installNative2d(PS2Runtime &runtime, bool nativeGs);
}
