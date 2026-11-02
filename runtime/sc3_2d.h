#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace sc3
{
    // The game's 2D builders rewritten in C++ from their MIPS, drawn natively from the game's own variables at their place
    // in the ordering table (SC3_NATIVE_2D=1, or =@<file> following the file's first line): RECOMP_NOTES "Native 2D".
    void installNative2d(PS2Runtime &runtime, bool nativeGs);

    // A small movie's current picture by its player handle (the host decoder's): RGBA rows, alpha 0x80 = 1.0, and its
    // number; rgba is left as it is while known is still that number.
    using MoviePicture = bool (*)(uint32_t handle, uint32_t known, std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height,
                                  uint32_t &number);
    void setMoviePictures(MoviePicture pictures);

    // The native 2D draws this player's pictures from the host decoder: they need not be written into the game's memory.
    bool native2dMovie(uint32_t handle);
}
