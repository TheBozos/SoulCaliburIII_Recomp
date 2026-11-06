// Sofdec answered at mwPly's level: a player is never started, so every start reports its end straight away. The decoder
// and its pictures come with the movie stage.
#include "sc3_port.h"

#include <cstring>

namespace sc3::movies
{
    namespace
    {
        constexpr uint32_t kMwPlyStarts[] = {0x003AD4B0u, 0x003AD5E8u, 0x003AD6E8u, 0x003ADA20u, 0x003ADBD0u,
                                             0x003B1328u, 0x003B1428u, 0x003B1698u, 0x003B17E0u, 0x003B2DF0u};
        constexpr uint32_t kMwPlyGetStat = 0x003AE9F8u;
        constexpr int32_t kMwPlyStatPlayEnd = 3;
        constexpr uint32_t kMoviePlayStat = 0x003AE988u;
        constexpr int32_t kMoviePlayStatFinished = 4;
        constexpr uint32_t kMovieTaskEnd = 0x00218600u;
        constexpr uint32_t kMovieStatePointer = 0x70000030u;

        uint32_t readWord(PS2Runtime *runtime, uint32_t address)
        {
            uint32_t word = 0;
            if (const uint8_t *p = runtime->memory.hostPointer(address, 4))
                std::memcpy(&word, p, 4);
            return word;
        }

        void startSkipped(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = getRegU32(ctx, 31); }

        void statPlayEnd(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, kMwPlyStatPlayEnd);
            ctx->pc = getRegU32(ctx, 31);
        }

        void playStatFinished(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, kMoviePlayStatFinished);
            ctx->pc = getRegU32(ctx, 31);
        }

        // The task's end leaves the job it runs for marked playing unless the job is released here.
        void taskEndReleasesJob(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t movieState = readWord(runtime, kMovieStatePointer);
            if (movieState && readWord(runtime, movieState + 8u) == getRegU32(ctx, 4))
                if (uint8_t *p = runtime->memory.hostPointer(movieState + 8u, 4))
                    std::memset(p, 0, 4);
            func_0x218600(rdram, ctx, runtime);
        }
    }

    void install(Registry &runtime)
    {
        for (uint32_t address : kMwPlyStarts)
            runtime.registerFunction(address, &startSkipped);
        runtime.registerFunction(kMwPlyGetStat, &statPlayEnd);
        runtime.registerFunction(kMoviePlayStat, &playStatFinished);
        runtime.registerFunction(kMovieTaskEnd, &taskEndReleasesJob);
    }
}
