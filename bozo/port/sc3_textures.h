// SC3's texture pack (scripts/sc3_texture_pack.py): the texture a 3D part's material selects among the upload groups its
// frame has CALLed. The game's uploads are never sent anywhere.
#pragma once

#include "sc3_port.h"
#include "sc3_texture_pack.h"

namespace sc3::textures
{
    bool ready();
    void frameDone();
    // At a per-part hook: 0x104AF0's commands run in order, a batch's texture CALLs come right before its draws. The texture
    // this TEX0 and used size select, the latest group first; with `part` (the part's EE address), the groups of the leaf or
    // copy the part sits in first (Character Creation CALLs every item's before drawing any).
    TexturePtr atHook(uint64_t tex0, uint32_t width, uint32_t height, uint32_t part = 0);
    void noteDepthLink(R5900Context *ctx);
    void noteRequest(R5900Context *ctx);
    void noteReadBefore(uint8_t *rdram, R5900Context *ctx);
    void noteCopy(uint8_t *rdram, R5900Context *ctx);
}
