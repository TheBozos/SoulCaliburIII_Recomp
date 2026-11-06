// A frame on BozoEngine with no GS: nothing raises the GS interrupt, so what the game waits on is written at the kick
// (the FINISH side of handler 0x10e658), and the frame ends in the main loop's Timer 3 wait.
#include "sc3_port.h"

namespace sc3::frame
{
    namespace
    {
        constexpr uint32_t kGsDrainWait = 0x00127778u;
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kElapsedLines = 0x00148098u;
        constexpr uint32_t kFrameEndReturn = 0x0012EA90u;
        constexpr uint32_t kFinishFlag = 0x0046E5CEu;
        constexpr uint32_t kFinishCount = 0x0046E60Cu;
        constexpr uint32_t kFinishSeen = 0x005BE782u;
        constexpr uint32_t kSignals = 0x00467494u;
        constexpr uint32_t kPadSetCallback = 0x00422F20u;
        constexpr uint32_t kPadCallback = 0x00576DD8u;

        bool g_frameOpen = false;

        template <class R>
        void declareTransfersDone(R &runtime)
        {
            if constexpr (requires { runtime.transfersAlwaysDone(); })
                runtime.transfersAlwaysDone();
        }

        void gsDrainWait(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
        }

        void padSetCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            auto *current = reinterpret_cast<uint32_t *>(rdram + kPadCallback);
            setReturnU32(ctx, *current);
            *current = getRegU32(ctx, 4);
            ctx->pc = getRegU32(ctx, 31);
        }

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            func_0x10f098(rdram, ctx, runtime);
            rdram[kFinishFlag] = 1;
            *reinterpret_cast<uint32_t *>(rdram + kFinishCount) += 1;
            *reinterpret_cast<uint16_t *>(rdram + kFinishSeen) = 1;
            *reinterpret_cast<uint32_t *>(rdram + kSignals) = 0;
            g_frameOpen = true;
        }

        void elapsedLines(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameOpen && getRegU32(ctx, 31) == kFrameEndReturn)
            {
                g_frameOpen = false;
                bozo::port::frameEnd(runtime, ctx);
            }
            func_0x148098(rdram, ctx, runtime);
        }
    }

    void install(Registry &runtime)
    {
        declareTransfersDone(runtime);
        runtime.registerFunction(kGsDrainWait, &gsDrainWait);
        runtime.registerFunction(kPadSetCallback, &padSetCallback);
        runtime.registerFunction(kFrameKick, &frameKick);
        runtime.registerFunction(kElapsedLines, &elapsedLines);
    }
}
