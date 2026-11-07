// The renderer's scratchpad packets: begin 0x10F4B8 and the closes that move a finished packet to the chain area in RAM
// and link it into the ordering table. The game reads those bytes back, so the move stays, as a plain copy.
#include "sc3_port.h"

#include <cstring>
#include <stdexcept>

namespace sc3::packet
{
    namespace
    {
        constexpr uint32_t kBegin = 0x0010F4B8u;
        constexpr uint32_t kCloseAtEnd = 0x0010DB88u;
        constexpr uint32_t kCloseDepth = 0x0010D9A0u;
        constexpr uint32_t kCloseFixed = 0x0010DC90u;
        constexpr uint32_t kCloseLayerEnd = 0x0010E1F8u;
        constexpr uint32_t kCriFromScratchpad = 0x003AE058u;
        constexpr uint32_t kCriToScratchpad = 0x003AE130u;

        constexpr uint32_t kScratchpad = 0x70000000u;
        constexpr uint32_t kWriteAt = kScratchpad + 0x00u;
        constexpr uint32_t kHalf = kScratchpad + 0x04u;
        constexpr uint32_t kHalfBases = kScratchpad + 0x08u;
        constexpr uint32_t kFixedArea = kScratchpad + 0x10u;
        constexpr uint32_t kHalfOffset = kScratchpad + 0x14u;
        constexpr uint32_t kHalfSize = kScratchpad + 0x18u;
        constexpr uint32_t kFixedSize = kScratchpad + 0x1Cu;
        constexpr uint32_t kMaxHalfSize = 0x1FC0u;

        constexpr uint32_t kRenderer = 0x005BE700u;
        constexpr uint32_t kOff = kRenderer + 0x00u;
        constexpr uint32_t kChainQuadwords = kRenderer + 0x2Cu;
        constexpr uint32_t kChainAt = kRenderer + 0x30u;
        constexpr uint32_t kOtScale = kRenderer + 0x34u;
        constexpr uint32_t kLayerBuckets = kRenderer + 0x50u;
        constexpr uint32_t kLayerLast = kRenderer + 0x64u;

        constexpr uint32_t kBucketMask = 0x00467488u;
        constexpr uint32_t kBucketMaskBase = 0x00467490u;

        constexpr uint32_t kRefTag = 0x20000000u;
        constexpr uint32_t kOtBuckets = 0x2000u;
        constexpr uint32_t kOne = 0x3F800000u;
        constexpr uint32_t kDepthToOt = 0x3DF5C28Fu;
        constexpr uint32_t kDepthMax = 0x41480000u;

        struct Guest
        {
            uint8_t *rdram;
            R5900Context *ctx;
            PS2Runtime *rt;

            uint32_t lw(uint32_t address) const { return bozo::load<uint32_t>(rdram, ctx, rt, address); }
            void sw(uint32_t address, uint32_t value) const { bozo::store<uint32_t>(rdram, ctx, rt, address, value); }
        };

        uint32_t reserveChain(const Guest &g, uint32_t quadwords)
        {
            const uint32_t chainAt = g.lw(kChainAt);
            const uint32_t destination = (chainAt - quadwords * 16u) & ~0x7Fu;
            g.sw(kChainAt, destination);
            g.sw(kChainQuadwords, g.lw(kChainQuadwords) + ((chainAt - destination) >> 4));
            return destination;
        }

        void markBucket(const Guest &g, uint32_t bucket)
        {
            const uint64_t mask = bozo::load<uint64_t>(g.rdram, g.ctx, g.rt, kBucketMask);
            const uint32_t group = (g.lw(kBucketMaskBase) + bucket) >> 7;
            bozo::store<uint64_t>(g.rdram, g.ctx, g.rt, kBucketMask, mask | (uint64_t{1} << (group & 63u)));
        }

        void flipHalf(const Guest &g)
        {
            const uint32_t half = g.lw(kHalf) ^ 1u;
            g.sw(kHalf, half);
            g.sw(kWriteAt, g.lw(kHalfBases + half * 4u));
        }

        void linkIntoBucket(const Guest &g, uint32_t bucket, uint32_t tagAt, uint32_t tagWord, uint32_t from,
                            uint32_t quadwords, bool clearTag)
        {
            const uint32_t entry = g.lw(kLayerBuckets) + bucket * 16u;
            markBucket(g, bucket);
            const uint32_t destination = reserveChain(g, quadwords);
            const uint32_t next = g.lw(entry + 4u);
            if (clearTag)
            {
                const uint8_t zero[16] = {};
                bozo::store128(g.rdram, g.ctx, g.rt, tagAt, zero);
            }
            g.sw(tagAt + 4u, next);
            g.sw(tagAt, tagWord | kRefTag);
            g.sw(entry + 4u, destination);
            copyScratchpad(g.rt, destination, from, quadwords, false);
        }

        void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

        void begin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
        {
            const Guest g{rdram, ctx, rt};
            const uint32_t size = (getRegU32(ctx, 4) + 0xFu) & ~0xFu;
            int32_t result = 0;
            if (g.lw(kHalfSize) != size)
            {
                if (size > kMaxHalfSize)
                    result = -1;
                else
                {
                    const uint32_t offset = 0x4000u - size * 2u;
                    const uint32_t fixedSize = offset - 0x80u;
                    g.sw(kFixedSize, fixedSize);
                    g.sw(kFixedArea, fixedSize != 0u ? kScratchpad + 0x80u : 0u);
                    g.sw(kHalf, 0u);
                    g.sw(kHalfBases, kScratchpad + offset);
                    g.sw(kHalfBases + 4u, kScratchpad + offset + size);
                    g.sw(kHalfOffset, offset);
                    g.sw(kWriteAt, kScratchpad + offset);
                    g.sw(kHalfSize, size);
                }
            }
            setReturnS32(ctx, result);
            returnToCaller(ctx);
        }

        void closeAtEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
        {
            const Guest g{rdram, ctx, rt};
            uint32_t destination = 0;
            if (g.lw(kOff) == 0u)
            {
                const uint32_t start = g.lw(kWriteAt);
                const uint32_t quadwords = (getRegU32(ctx, 4) - start) >> 4;
                destination = reserveChain(g, quadwords);
                copyScratchpad(rt, destination, start, quadwords, false);
                flipHalf(g);
            }
            setReturnU32(ctx, destination);
            returnToCaller(ctx);
        }

        void criFromScratchpad(uint8_t *, R5900Context *ctx, PS2Runtime *rt)
        {
            const int32_t size = getRegS32(ctx, 5);
            copyScratchpad(rt, getRegU32(ctx, 6), getRegU32(ctx, 4), static_cast<uint32_t>((size < 0 ? size + 15 : size) >> 4), false);
            setReturnS32(ctx, 0);
            returnToCaller(ctx);
        }

        void criToScratchpad(uint8_t *, R5900Context *ctx, PS2Runtime *rt)
        {
            const int32_t size = getRegS32(ctx, 5);
            copyScratchpad(rt, getRegU32(ctx, 4), getRegU32(ctx, 6), static_cast<uint32_t>((size < 0 ? size + 15 : size) >> 4), true);
            setReturnS32(ctx, 0);
            returnToCaller(ctx);
        }
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

    void closeDepth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const Guest g{rdram, ctx, rt};
        const uint32_t depth = ctx->fu[12];
        returnToCaller(ctx);
        if (g.lw(kOff) != 0u || bozo::ps2f::lt(depth, 0u) || bozo::ps2f::lt(kDepthMax, depth))
            return;
        uint32_t z = bozo::fmul(ctx, depth, kDepthToOt);
        if (bozo::ps2f::lt(kOne, z))
            z = kOne;
        const uint32_t bucket = bozo::ps2f::toInt(bozo::fmul(ctx, z, g.lw(kOtScale)));
        if (bucket >= kOtBuckets)
            return;
        const uint32_t start = g.lw(kWriteAt);
        linkIntoBucket(g, bucket, getRegU32(ctx, 5), getRegU32(ctx, 6), start, (getRegU32(ctx, 4) - start) >> 4, true);
        flipHalf(g);
    }

    void closeFixed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const Guest g{rdram, ctx, rt};
        returnToCaller(ctx);
        if (g.lw(kOff) != 0u)
            return;
        const uint32_t z = bozo::fmul(ctx, ctx->fu[12], kDepthToOt);
        const uint32_t scale = g.lw(kOtScale);
        const uint32_t bucket = bozo::ps2f::toInt(bozo::ps2f::lt(z, kOne) ? bozo::fmul(ctx, z, scale) : scale);
        const uint32_t start = getRegU32(ctx, 4);
        linkIntoBucket(g, bucket, getRegU32(ctx, 6), getRegU32(ctx, 7), start, (getRegU32(ctx, 5) - start) >> 4, false);
    }

    void closeLayerEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const Guest g{rdram, ctx, rt};
        returnToCaller(ctx);
        if (g.lw(kOff) != 0u)
            return;
        const uint32_t start = g.lw(kWriteAt);
        linkIntoBucket(g, g.lw(kLayerLast) - 1u, getRegU32(ctx, 5), getRegU32(ctx, 6), start, (getRegU32(ctx, 4) - start) >> 4, true);
        flipHalf(g);
    }

    void install(Registry &runtime)
    {
        runtime.registerFunction(kBegin, &begin);
        runtime.registerFunction(kCloseAtEnd, &closeAtEnd);
        runtime.registerFunction(kCloseDepth, &closeDepth);
        runtime.registerFunction(kCloseFixed, &closeFixed);
        runtime.registerFunction(kCloseLayerEnd, &closeLayerEnd);
        runtime.registerFunction(kCriFromScratchpad, &criFromScratchpad);
        runtime.registerFunction(kCriToScratchpad, &criToScratchpad);
    }
}
