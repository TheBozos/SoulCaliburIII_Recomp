// What the game loads, told to the subsystems that find their data by where it was read from (ROOT.OLK's leaves): its first
// index request (0x150418: ROOT.OLK's sector), each sceCdRead, each memcpy of a model.
#include "sc3_draw.h"
#include "sc3_meshes.h"
#include "sc3_textures.h"

namespace sc3::loads
{
    namespace
    {
        constexpr uint32_t kFileRequest = 0x00150418u;
        constexpr uint32_t kCdRead = 0x00424E28u;
        constexpr uint32_t kCopy = 0x0012FE98u;

        void fileRequest(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            textures::noteRequest(ctx);
            meshes::noteRequest(ctx);
            twoD::noteRequest(rdram, ctx, runtime);
            func_0x150418(rdram, ctx, runtime);
        }

        void cdRead(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            const R5900Context arguments = *ctx;
            textures::noteReadBefore(rdram, ctx);
            meshes::noteReadBefore(rdram, ctx);
            twoD::noteReadBefore(rdram, ctx, runtime);
            bozo::port::callLibrary(runtime, ctx, kCdRead);
            meshes::noteReadAfter(rdram, const_cast<R5900Context *>(&arguments));
            twoD::noteReadAfter(rdram, const_cast<R5900Context *>(&arguments), runtime);
        }

        void copy(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            textures::noteCopy(rdram, ctx);
            meshes::noteCopy(rdram, ctx);
            func_0x12fe98(rdram, ctx, runtime);
        }
    }

    void install(Registry &runtime)
    {
        runtime.registerFunction(kFileRequest, &fileRequest);
        runtime.registerFunction(kCdRead, &cdRead);
        runtime.registerFunction(kCopy, &copy);
    }
}
