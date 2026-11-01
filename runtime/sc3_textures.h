#pragma once

#include <cstddef>
#include <cstdint>

class PS2Runtime;

namespace ps2x::native
{
    struct NativeTexture;
}

namespace sc3
{
    void installTexturePack(PS2Runtime &runtime, bool nativeGs);

    // VIF1 thread, for a direct draw: the pack texture its TEX0 selects, at the used size its material gives, in the
    // upload group being drawn, or null (the draw samples GS memory as before).
    const ps2x::native::NativeTexture *packTexture(uint64_t tex0, uint32_t width, uint32_t height);
    // VIF1 thread, each DIRECT the VIF1 stream carries: the game's sync points (the SIGNAL 2 its VIF1 list carries between
    // an upload group and the draws after it) tell how far the GS has got through the frame's uploads.
    void packNoteDirect(const uint32_t *words, size_t qwords);
    // VIF1 thread, where the game's frame ends: the next draws belong to the next kicked frame's upload groups.
    void packFrameEnd();
}
