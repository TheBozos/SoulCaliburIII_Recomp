// The game's reads of what was drawn, with no GS: the sun flare's depth probe. 0x163650 reads an 8 x 8 block of Z around the sun
// through 0x13BDF0 (sceGsExecStoreImage, PSMZ32 into a0's buffer): answered from the Renderer's depth read back.
#include "sc3_draw.h"

namespace sc3::offscreen
{
    namespace
    {
        constexpr uint32_t kStoreImage = 0x0013BDF0u;
        constexpr float kOffsetX = 2048.0f - 320.0f, kOffsetY = 2048.0f - 224.0f;

        void storeImage(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            const int32_t x = getRegS32(ctx, 7), y = getRegS32(ctx, 8);
            const uint32_t w = std::min<uint32_t>(getRegU32(ctx, 9), 64u), h = std::min<uint32_t>(getRegU32(ctx, 10), 64u);
            const uint32_t dest = getRegU32(ctx, 6) & kRamMask;
            bozo::render::Renderer &renderer = bozo::render::Renderer::instance();
            renderer.wantDepth();
            for (uint32_t j = 0; j < h; ++j)
                for (uint32_t i = 0; i < w; ++i)
                {
                    uint32_t z = 0;
                    renderer.depthAt(kOffsetX + static_cast<float>(x + static_cast<int32_t>(i)), kOffsetY + static_cast<float>(y + static_cast<int32_t>(j)), z);
                    z &= 0xFFFFFFu;
                    std::memcpy(rdram + dest + (j * w + i) * 4u, &z, 4);
                }
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void install(Registry &runtime) { runtime.registerFunction(kStoreImage, &storeImage); }
}
