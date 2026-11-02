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
    // Game thread, at a per-part hook (0x104AF0's commands run in order: a batch's texture CALLs come right before its
    // draws): the pack texture this TEX0 (as the pack's material has it) and used size select among the upload groups the
    // frame being built has CALLed, the latest first, or null; with `part` (the EE address of the part drawn), the groups
    // of the leaf (or copy) the part sits in first: a model's own uploads (Character Creation CALLs every item's before
    // drawing any). True from texturePackReady() on.
    const ps2x::native::NativeTexture *packTextureAtHook(uint64_t tex0, uint32_t width, uint32_t height, uint32_t part = 0);
    bool texturePackReady();
    // Game thread, a part left to VU1: the GS samples what TEX0 selects from GS memory, so the upload group the pack
    // finds it in (as packTextureAtHook would) is still sent; with its texture unknown or in no group, every group feeding
    // textures the pack lacks is too (with it unknown, every group).
    void packNoteGsUse(uint64_t tex0, uint32_t width, uint32_t height);
    void packNoteGsUseAny();
}
