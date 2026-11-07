// A frame on BozoEngine with no GS: the kick 0x10F098 in C++ (the draws go to the renderer, the state the GS handler
// 0x10E658 leaves behind is written at once), the game's DMA driver answered at its own functions so no chain is ever
// linked or sent, and the frame ends in the main loop's Timer 3 wait.
#include "sc3_port.h"

#include "bozo/app/frame_pacer.h"

#include <cstring>
#include <stdexcept>

namespace sc3::frame
{
    namespace
    {
        constexpr uint32_t kGsDrainWait = 0x00127778u;
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kOtWalk = 0x0010E4B8u;
        constexpr uint32_t kOtWalkByGroup = 0x0010E5C8u;
        constexpr uint32_t kDmaInit = 0x0010C890u;
        constexpr uint32_t kElapsedLines = 0x00148098u;
        constexpr uint32_t kFrameEndReturn = 0x0012EA90u;
        constexpr uint32_t kAdxmVsyncSleep = 0x003F0F10u;
        constexpr uint32_t kAdxmVsyncSleepReturn = 0x003F0F60u;
        constexpr uint32_t kMovieFrameWaitReturn = 0x0012E654u;
        constexpr uint32_t kGsExecStoreImage = 0x00420370u;
        constexpr uint32_t kPadSetCallback = 0x00422F20u;
        constexpr uint32_t kPadCallback = 0x00576DD8u;
        constexpr uint32_t kFromScratchpad = 0x0010CF10u;
        constexpr uint32_t kFromScratchpadStop = 0x0010CF88u;
        constexpr uint32_t kToScratchpadWait = 0x0010CFC8u;
        constexpr uint32_t kToScratchpad = 0x0010CFF8u;

        // VIF0, VIF1 and GIF: wait, sends and stops of the driver at 0x10C9D0-0x10CED0.
        constexpr uint32_t kChannelDriver[] = {0x0010C9D0u, 0x0010CA00u, 0x0010CA78u, 0x0010CAE0u,
                                               0x0010CB20u, 0x0010CB50u, 0x0010CBC8u, 0x0010CC40u, 0x0010CCA8u, 0x0010CCE0u, 0x0010CD48u,
                                               0x0010CD88u, 0x0010CDB8u, 0x0010CE30u, 0x0010CE98u, 0x0010CED0u};

        constexpr uint32_t kInitSys = 0x00440860u;

        constexpr uint32_t kRenderer = 0x005BE700u;
        constexpr uint32_t kFrameLists = 0x004674C0u;
        constexpr uint32_t kFinishFlag = 0x0046E5CEu;
        constexpr uint32_t kOverBudget = 0x0046E5DCu;
        constexpr uint32_t kFrameSkipped = 0x0046E5DDu;
        constexpr uint32_t kFrameNumber = 0x0046E608u;
        constexpr uint32_t kFinishCount = 0x0046E60Cu;
        constexpr uint32_t kSignals = 0x00467494u;
        constexpr uint32_t kKickedFrameNumber = 0x0047100Cu;

        bool g_frameOpen = false;
        bozo::app::FramePacer g_pacer;

        void endOfFrame(R5900Context *ctx, PS2Runtime *runtime)
        {
            movies::tick(runtime);
            g_pacer.setCap(60u);
            g_pacer.setPaced(bozo::port::realTime(runtime));
            g_pacer.frameEnd();
            bozo::port::frameEnd(runtime, ctx);
        }

        template <typename T>
        T get(const uint8_t *rdram, uint32_t address)
        {
            T v;
            std::memcpy(&v, rdram + address, sizeof(T));
            return v;
        }

        template <typename T>
        void put(uint8_t *rdram, uint32_t address, T v) { std::memcpy(rdram + address, &v, sizeof(T)); }

        void answered(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = getRegU32(ctx, 31); }

        // The rest of _InitSys is the kernel's and the timer library's, native. Its first call makes the two library locks
        // that guest code goes on using (0x5792F8, 0x5792FC: the stdio list at 0x42B358).
        void initSys(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = getRegU32(ctx, 31);
            func_0x440610(rdram, ctx, runtime);
            setReturnS32(ctx, 0);
            ctx->pc = returnTo;
        }

        void adxmVsyncSleep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t caller = *reinterpret_cast<uint32_t *>(rdram + (getRegU32(ctx, 29) & 0x1FFFFFFu));
            if (getRegU32(ctx, 31) == kAdxmVsyncSleepReturn && caller == kMovieFrameWaitReturn)
            {
                ot::presentAlone();
                endOfFrame(ctx, runtime);
                ctx->pc = kAdxmVsyncSleepReturn;
                return;
            }
            func_0x3f0f10(rdram, ctx, runtime);
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

        void copyScratchpad(PS2Runtime *runtime, uint32_t ram, uint32_t scratchpad, uint32_t quadwords, bool toScratchpad)
        {
            for (uint32_t i = 0; i < quadwords; ++i, ram += 16u, scratchpad += 16u)
            {
                uint8_t *r = runtime->memory.hostPointer(ram, 16);
                uint8_t *s = runtime->memory.hostPointer(0x70000000u | (scratchpad & 0x3FF0u), 16);
                if (!r || !s)
                    throw std::runtime_error("sc3: scratchpad copy outside memory");
                std::memcpy(toScratchpad ? s : r, toScratchpad ? r : s, 16);
            }
        }

        void fromScratchpad(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            copyScratchpad(runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6) & 0xFFFFu, false);
            ctx->pc = getRegU32(ctx, 31);
        }

        void toScratchpad(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            copyScratchpad(runtime, getRegU32(ctx, 5), getRegU32(ctx, 4), getRegU32(ctx, 6) & 0xFFFFu, true);
            ctx->pc = getRegU32(ctx, 31);
        }

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto overBudget = [&](uint32_t limit, uint32_t used) {
                return get<uint32_t>(rdram, kFrameLists + limit) < get<uint32_t>(rdram, kRenderer + used);
            };
            const uint32_t counts = kRenderer + 0x80u + (get<uint32_t>(rdram, kRenderer + 0x38u) ^ 1u);
            const uint8_t signalsA = rdram[counts + 0x16u];
            const uint8_t signalsB = rdram[counts + 0x18u];
            g_frameOpen = true;
            if (overBudget(0x28u, 0x68u) || overBudget(0x2Cu, 0x6Cu) || overBudget(0x34u, 0x74u) || overBudget(0x30u, 0x70u) ||
                get<uint16_t>(rdram, kFrameLists + 0x3Eu) != 0u || rdram[kOverBudget] != 0u)
            {
                rdram[kFrameSkipped] = 1u;
                put<uint16_t>(rdram, kFrameLists + 0x3Eu, 0u);
                rdram[kRenderer + 0x93u] = signalsB;
                rdram[kFinishFlag] = 1u;
                rdram[kRenderer + 0x92u] = signalsA;
                put<uint32_t>(rdram, kKickedFrameNumber, 0u);
                put<uint16_t>(rdram, kRenderer + 0x82u, 1u);
                ot::dropFrame(rdram);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            rdram[kFrameSkipped] = 0u;
            put<uint32_t>(rdram, kKickedFrameNumber, get<uint32_t>(rdram, kFrameNumber));
            put<uint16_t>(rdram, kRenderer + 0x90u, 0u);
            ot::kick(rdram, ctx, runtime);
            rdram[kRenderer + 0x93u] = signalsB;
            rdram[kRenderer + 0x92u] = signalsA;
            rdram[kFinishFlag] = 1u;
            put<uint32_t>(rdram, kFinishCount, get<uint32_t>(rdram, kFinishCount) + 1u);
            put<uint16_t>(rdram, kRenderer + 0x82u, 1u);
            put<uint32_t>(rdram, kSignals, 0u);
            ctx->pc = getRegU32(ctx, 31);
        }

        void elapsedLines(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameOpen && getRegU32(ctx, 31) == kFrameEndReturn)
            {
                g_frameOpen = false;
                endOfFrame(ctx, runtime);
            }
            func_0x148098(rdram, ctx, runtime);
        }
    }

    void install(Registry &runtime)
    {
        runtime.registerFunction(kInitSys, &initSys);
        runtime.registerFunction(kGsDrainWait, &gsDrainWait);
        runtime.registerFunction(kPadSetCallback, &padSetCallback);
        runtime.registerFunction(kGsExecStoreImage, &gsDrainWait);
        runtime.registerFunction(kFrameKick, &frameKick);
        runtime.registerFunction(kOtWalk, &answered);
        runtime.registerFunction(kOtWalkByGroup, &answered);
        runtime.registerFunction(kDmaInit, &answered);
        for (uint32_t address : kChannelDriver)
            runtime.registerFunction(address, &answered);
        runtime.registerFunction(kFromScratchpad, &fromScratchpad);
        runtime.registerFunction(kFromScratchpadStop, &answered);
        runtime.registerFunction(kToScratchpadWait, &answered);
        runtime.registerFunction(kToScratchpad, &toScratchpad);
        runtime.registerFunction(kElapsedLines, &elapsedLines);
        runtime.registerFunction(kAdxmVsyncSleep, &adxmVsyncSleep);
    }
}
