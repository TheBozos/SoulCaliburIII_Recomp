// SC3's VU0 microprograms (image 5f8cbf2471656edf: 9 entries, about 370k calls a second in fights) on BozoEngine's VU0
// semantics, registered by entry (Registry::vu0Program). The order of the instructions is the one the pipeline gives.
#include "sc3_port.h"

#include "bozo/ops_vu0.h"

namespace sc3::vu0
{
    namespace
    {
        using namespace bozo::vu0;

        inline int32_t i16(int32_t value) { return static_cast<int16_t>(value); }

        inline uint32_t clipFlags(const Vec &s, const Vec &t)
        {
            const uint32_t w = t.u[3];
            const int32_t limit = (w & 0x7F800000u) != 0u ? static_cast<int32_t>(w & 0x7FFFFFFFu) : 0x007FFFFF;
            uint32_t flags = 0u;
            for (int k = 0; k < 3; ++k)
            {
                flags |= static_cast<int32_t>(s.u[k]) > limit ? 1u << (2 * k) : 0u;
                flags |= static_cast<int32_t>(s.u[k] ^ 0x80000000u) > limit ? 2u << (2 * k) : 0u;
            }
            return flags;
        }

        void run(uint32_t entry, R5900Context *ctx, Runtime *rt)
        {
            Vec *vf = ctx->vu0_vf;
            Vec &acc = ctx->vu0_acc;
            const Vec kVf0 = vf[0];
            uint32_t &i = ctx->vu0_i_bits;
            uint32_t &q = ctx->vu0_q_bits;
            uint32_t &clip = ctx->vu0_clip_flags;
            uint32_t qPending = 0u, clip_0388 = 0u, clip_0530 = 0u;
            int32_t vi[16];
            for (int r = 0; r < 16; ++r)
                vi[r] = i16(static_cast<int32_t>(ctx->vi[r]));
            clip &= 0xFFFFFFu;
            switch (entry)
            {
            case 0x0u: goto n0;
            case 0x18u: goto n1;
            case 0x38u: goto n2;
            case 0x60u: goto n3;
            case 0xf0u: goto n4;
            case 0x360u: goto n5;
            case 0x3c8u: goto n6;
            case 0x3e8u: goto n7;
            case 0x438u: goto n8;
            default: return;
            }
            n0:
                i = 0x3fc90fdbu;
            n129:
                mul(ctx, vf[1], vf[1], broadcast(i), 15);
                mul(ctx, vf[5], vf[1], vf[1], 15);
                i = 0x3c0bd0f0u;
            n112:
                add(ctx, acc, kVf0, broadcast(i), 14);
                move(vf[2], vf[1], 15);
                mul(ctx, acc, kVf0, broadcast(i), 1);
                mul(ctx, vf[7], vf[5], vf[5], 15);
                i = 0xb9526c8fu;
                madd(ctx, acc, acc, vf[5], broadcast(i), 15);
                i = 0x362e9bf5u;
                madd(ctx, vf[8], acc, vf[7], broadcast(i), 15);
                add(ctx, vf[7], vf[7], vf[5], 15);
                i = 0xbe3367b3u;
                mul(ctx, acc, vf[5], broadcast(i), 15);
                madd(ctx, vf[7], acc, vf[7], vf[8], 15);
                mul(ctx, acc, vf[2], bc(kVf0, 3), 15);
                madd(ctx, vf[2], acc, vf[7], vf[2], 15);
                goto done;
            n1:
                itof(vf[1], vf[1], 15, 0);
                i = 0x38c90fdbu;
                goto n129;
            n2:
                i = 0x40c90fdbu;
                goto n129;
            n3:
                mul(ctx, vf[5], vf[1], vf[1], 15);
                i = 0x3c0bd0f0u;
                goto n112;
            n4:
                add(ctx, vf[7], vf[1], bc(vf[1], 0), 4);
                sub(ctx, vf[8], vf[1], bc(vf[1], 0), 4);
                qPending = div(ctx, vf[8].u[1], vf[7].u[1]);
                add(ctx, vf[1], kVf0, bc(kVf0, 3), 14);
                mul(ctx, vf[1], kVf0, kVf0, 1);
                i = 0xbddca708u;
                add(ctx, acc, kVf0, broadcast(i), 4);
                q = qPending;
                mul(ctx, vf[1], vf[1], broadcast(q), 15);
                mul(ctx, vf[5], vf[1], vf[1], 15);
                mul(ctx, vf[7], vf[5], vf[5], 15);
                i = 0xbeaaa61cu;
                mul(ctx, acc, vf[5], broadcast(i), 8);
                i = 0x3cf58245u;
                madd(ctx, acc, acc, vf[5], broadcast(i), 4);
                mul(ctx, vf[8], vf[7], vf[5], 15);
                add(ctx, vf[9], vf[7], vf[5], 15);
                i = 0x3e9d4a15u;
                madd(ctx, acc, acc, vf[7], broadcast(i), 8);
                i = 0xbb84d7e7u;
                madd(ctx, vf[2], acc, vf[7], broadcast(i), 4);
                mul(ctx, vf[7], vf[8], vf[5], 15);
                add(ctx, vf[3], vf[9], vf[8], 15);
                i = 0x3d3e2971u;
                madd(ctx, acc, acc, vf[8], broadcast(i), 8);
                mul(ctx, vf[2], vf[2], vf[9], 4);
                i = 0x3e831493u;
                madd(ctx, acc, acc, vf[7], broadcast(i), 8);
                madd(ctx, vf[7], acc, vf[3], bc(vf[2], 1), 8);
                i = 0x3f490fdbu;
                add(ctx, acc, kVf0, broadcast(i), 8);
                i = 0x3f7ffff5u;
                madd(ctx, acc, acc, vf[1], broadcast(i), 8);
                madd(ctx, vf[1], acc, vf[7], vf[1], 8);
                i = 0x3e22f983u;
                {
                    Vec upper = vf[1];
                    mul(ctx, upper, vf[1], broadcast(i), 8);
                    move(vf[2], vf[1], 8);
                    vf[1] = upper;
                }
                goto done;
            n5:
                mul(ctx, acc, vf[22], bc(vf[1], 0), 15);
                qPending = div(ctx, kVf0.u[3], vf[12].u[3]);
                madd(ctx, acc, acc, vf[23], bc(vf[1], 1), 15);
                move(vf[13], vf[12], 15);
                madd(ctx, acc, acc, vf[24], bc(vf[1], 2), 15);
                vi[2] = i16(static_cast<int32_t>(vf[1].u[3] & 0xFFFFu));
                madd(ctx, vf[12], acc, vf[25], bc(kVf0, 3), 15);
                vi[1] = 1;
                clip = ((clip << 6) | clipFlags(vf[16], vf[16])) & 0xFFFFFFu;
                vi[1] = i16(vi[1] + 32767);
                clip = ((clip << 6) | clipFlags(vf[14], kVf0)) & 0xFFFFFFu;
                clip_0388 = clip;
                vi[13] = vi[14];
                mul(ctx, acc, vf[18], bc(vf[1], 0), 15);
                vi[14] = vi[15];
                q = qPending;
                madd(ctx, acc, acc, vf[19], bc(vf[1], 1), 15);
                vi[15] = vi[2] & vi[1];
                mul(ctx, vf[13], vf[13], broadcast(q), 15);
                move(vf[16], vf[15], 15);
                madd(ctx, acc, acc, vf[20], bc(vf[1], 2), 15);
                vi[2] = static_cast<int32_t>(clip_0388 & 0xFFFu);
                madd(ctx, vf[15], acc, vf[21], bc(kVf0, 3), 15);
                vi[1] = (clip_0388 & 0x20u) != 0u ? 1 : 0;
                mul(ctx, acc, vf[26], vf[27], 15);
                vi[1] = i16(vi[1] + 4063);
                madd(ctx, vf[14], acc, vf[13], vf[27], 15);
                vi[12] = vi[1] & vi[2];
                goto done;
            n6:
                mul(ctx, acc, vf[28], bc(vf[2], 0), 15);
                madd(ctx, acc, acc, vf[29], bc(vf[2], 1), 15);
                madd(ctx, acc, acc, vf[30], bc(vf[2], 2), 15);
                madd(ctx, vf[2], acc, vf[31], bc(kVf0, 3), 15);
                goto done;
            n7:
                vi[13] = i16(static_cast<int32_t>(vf[4].u[3] & 0xFFFFu));
                lq(rt, vf[5], static_cast<uint32_t>(vi[13] + 0), 15);
                lq(rt, vf[6], static_cast<uint32_t>(vi[13] + 1), 15);
                lq(rt, vf[7], static_cast<uint32_t>(vi[13] + 2), 15);
                add(ctx, acc, vf[1], bc(kVf0, 0), 15);
                lq(rt, vf[8], static_cast<uint32_t>(vi[13] + 3), 15);
                madd(ctx, acc, acc, vf[5], bc(vf[3], 0), 15);
                vi[1] = vi[13] & vi[14];
                madd(ctx, acc, acc, vf[6], bc(vf[3], 1), 15);
                madd(ctx, acc, acc, vf[7], bc(vf[3], 2), 15);
                madd(ctx, vf[1], acc, vf[8], bc(vf[3], 3), 15);
                add(ctx, acc, vf[2], bc(kVf0, 0), 15);
                goto done;
            n8:
                mul(ctx, acc, vf[24], bc(vf[2], 0), 15);
                madd(ctx, acc, acc, vf[25], bc(vf[2], 1), 15);
                madd(ctx, vf[2], acc, vf[26], bc(vf[2], 2), 15);
                mul(ctx, acc, vf[28], bc(vf[1], 0), 15);
                madd(ctx, acc, acc, vf[29], bc(vf[1], 1), 15);
                madd(ctx, acc, acc, vf[30], bc(vf[1], 2), 15);
                add(ctx, vf[6], vf[15], bc(vf[2], 3), 3);
                madd(ctx, vf[1], acc, vf[31], bc(kVf0, 3), 15);
                mul(ctx, vf[13], vf[3], vf[14], 12);
                max(vf[2], vf[2], bc(kVf0, 0), 14);
                max(vf[6], vf[6], bc(kVf0, 0), 3);
                add(ctx, acc, vf[23], bc(kVf0, 0), 15);
                qPending = div(ctx, kVf0.u[3], vf[1].u[3]);
                madd(ctx, acc, acc, vf[19], bc(vf[2], 0), 15);
                mul(ctx, vf[6], vf[6], vf[16], 3);
                madd(ctx, acc, acc, vf[20], bc(vf[2], 1), 15);
                madd(ctx, vf[2], acc, vf[21], bc(vf[2], 2), 15);
                q = qPending;
                mul(ctx, vf[1], vf[1], broadcast(q), 14);
                mini(vf[7], vf[6], bc(vf[16], 1), 1);
                mul(ctx, vf[3], vf[13], broadcast(q), 14);
                move(vf[12], vf[11], 15);
                mul(ctx, acc, vf[17], vf[18], 15);
                move(vf[11], vf[10], 15);
                madd(ctx, vf[5], acc, vf[1], vf[18], 15);
                move(vf[10], vf[1], 15);
                mul(ctx, acc, vf[22], bc(vf[6], 2), 15);
                madd(ctx, vf[6], acc, vf[22], bc(vf[7], 3), 15);
                mini(vf[5], vf[5], bc(vf[15], 0), 1);
                mul(ctx, acc, vf[2], vf[4], 15);
                i = 0x43000000u;
                madd(ctx, vf[2], acc, vf[6], broadcast(i), 15);
                max(vf[1], vf[5], bc(vf[15], 1), 1);
                clip = ((clip << 6) | clipFlags(vf[5], kVf0)) & 0xFFFFFFu;
                clip_0530 = clip;
                mini(vf[2], vf[2], bc(vf[16], 0), 15);
                ftoi(vf[1], vf[1], 15, 4);
                vi[1] = (clip_0530 & 0x3ffffu) != 0u ? 1 : 0;
                ftoi(vf[2], vf[2], 15, 0);
                goto done;
        done:
            for (int r = 1; r < 16; ++r)
                ctx->vi[r] = static_cast<uint16_t>(vi[r]);
        }

        template <uint32_t kEntry>
        void entry(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            run(kEntry, ctx, rt);
        }
    }

    void install(Registry &runtime)
    {
        [](auto &r) {
            if constexpr (requires { r.vu0Program(0u, nullptr); })
            {
                r.vu0Program(0x0u, entry<0x0u>);
                r.vu0Program(0x18u, entry<0x18u>);
                r.vu0Program(0x38u, entry<0x38u>);
                r.vu0Program(0x60u, entry<0x60u>);
                r.vu0Program(0xf0u, entry<0xf0u>);
                r.vu0Program(0x360u, entry<0x360u>);
                r.vu0Program(0x3c8u, entry<0x3c8u>);
                r.vu0Program(0x3e8u, entry<0x3e8u>);
                r.vu0Program(0x438u, entry<0x438u>);
            }
        }(runtime);
    }
}
