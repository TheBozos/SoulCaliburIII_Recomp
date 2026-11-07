// The game's two time sources on the EE clock, no timer behind them: the lines (a free count of horizontal blanks, what
// the frame budget and the vblank handler read) and the frame clock (a count of 512 cycles the main loop restarts each
// frame). recomp/sc3_patches.txt turns the main loop's register accesses into calls of these.
#include "sc3_port.h"

#include "bozo/ee/kernel.h"

#include <cstring>

namespace sc3::clock
{
    uint32_t lines(PS2Runtime *runtime, R5900Context *ctx);

    namespace
    {
        constexpr uint32_t kLines = 0x0012D0D8u;
        constexpr uint32_t kFrameClock = 0x0012D0E8u;
        constexpr uint32_t kFrameClockRestart = 0x0010E910u;
        constexpr uint32_t kFrameClockSet = 0x0010E920u;
        constexpr uint32_t kLinesSinceMark = 0x001480D0u;

        constexpr uint32_t kLineMarks = 0x00469234u;
        constexpr uint32_t kFrameClockHigh = 0x00469248u;
        constexpr uint32_t kCountMask = 0xFFFFu;
        constexpr uint64_t kFrameClockTick = 512u;

        uint32_t g_lastLines = ~0u;
        uint32_t g_lastCount = ~0u;
        uint32_t g_clockStart = 0u;
        uint64_t g_clockBase = 0u;

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + address, 4);
            return v;
        }

        uint32_t since(const uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t mark)
        {
            uint16_t from;
            std::memcpy(&from, rdram + word(rdram, kLineMarks) + mark, 2);
            const uint32_t count = lines(runtime, ctx);
            return count < from ? count + 0x10000u - from : count - from;
        }

        void linesCall(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            setReturnU32(ctx, lines(runtime, ctx));
            ctx->pc = getRegU32(ctx, 31);
        }

        void frameClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t count = (g_clockStart + static_cast<uint32_t>((bozo::port::now(runtime) - g_clockBase) / kFrameClockTick)) & kCountMask;
            if (count == g_lastCount)
                bozo::port::accountCycles(runtime, ctx, kFrameClockTick);
            g_lastCount = count;
            setReturnU32(ctx, word(rdram, kFrameClockHigh) | count);
            ctx->pc = getRegU32(ctx, 31);
        }

        void restart(PS2Runtime *runtime, uint32_t count)
        {
            g_clockStart = count & kCountMask;
            g_clockBase = bozo::port::now(runtime);
        }

        void frameClockRestart(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            restart(runtime, 0u);
            ctx->pc = getRegU32(ctx, 31);
        }

        void frameClockSet(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            restart(runtime, getRegU32(ctx, 5));
            ctx->pc = getRegU32(ctx, 31);
        }

        void linesSinceMark(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            setReturnU32(ctx, since(rdram, ctx, runtime, 0x104u));
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    uint32_t lines(PS2Runtime *runtime, R5900Context *ctx)
    {
        const uint64_t line = bozo::port::kernel(runtime).hblank();
        const uint32_t count = static_cast<uint32_t>(bozo::port::now(runtime) / line) & kCountMask;
        if (count == g_lastLines)
            bozo::port::accountCycles(runtime, ctx, line);
        g_lastLines = count;
        return count;
    }

    uint32_t linesSinceVblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return since(rdram, ctx, runtime, 0x100u);
    }

    void install(Registry &runtime)
    {
        runtime.registerFunction(kLines, &linesCall);
        runtime.registerFunction(kFrameClock, &frameClock);
        runtime.registerFunction(kFrameClockRestart, &frameClockRestart);
        runtime.registerFunction(kFrameClockSet, &frameClockSet);
        runtime.registerFunction(kLinesSinceMark, &linesSinceMark);
    }
}
