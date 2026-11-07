// What SC3's native drawing shares: the frame's order (the native ordering table, sc3_ot.cpp), the GS terms the rewritten
// builders compute a draw in (sc3_material.cpp turns them into a Renderer Material) and the R5900's float rules for code
// rewritten from MIPS and VU0 macro code.
#pragma once

#include "sc3_port.h"

#include "bozo/ops_fpu.h"
#include "bozo/ops_vu0.h"
#include "bozo/render/renderer.h"

#include <immintrin.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace sc3
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;

    // A draw of the frame being built. Run at the kick, in the game's draw order.
    namespace ot
    {
        using Draw = std::function<void(bozo::render::Renderer &)>;
        // The frame's ordering table: a bucket of the whole table (0x10DDF8's), or of the current layer (0x10D9A0's). False
        // when the frame is not being built (the game's linkers do nothing then) or the bucket is outside the table.
        bool linkBucket(uint8_t *rdram, int bucket, Draw draw);
        bool linkLayerBucket(uint8_t *rdram, int bucket, Draw draw);
        // The in-order lists: the GIF list (0x10EF78's) and the VIF1 list (0x10EF08's).
        bool appendGif(uint8_t *rdram, Draw draw);
        bool appendVif(uint8_t *rdram, Draw draw);
        // A place in the GIF list whose draw comes later (filled while the frame is built; empty: nothing).
        std::shared_ptr<Draw> gifSlot(uint8_t *rdram);
        // The draws of the frame the game kicks now, in its order, into the Renderer, and the frame published.
        void kick(uint8_t *rdram, R5900Context *ctx, Runtime *runtime);
    }

    // A GS draw as a builder computes it.
    namespace draw
    {
        enum GSPrimType : uint32_t
        {
            GS_PRIM_POINT = 0,
            GS_PRIM_LINE = 1,
            GS_PRIM_LINESTRIP = 2,
            GS_PRIM_TRIANGLE = 3,
            GS_PRIM_TRISTRIP = 4,
            GS_PRIM_TRIFAN = 5,
            GS_PRIM_SPRITE = 6,
        };

        struct GsPrim
        {
            GSPrimType type = GS_PRIM_TRIANGLE;
            bool iip = false, tme = false, fge = false, abe = false, aa1 = false, fst = false, ctxt = false;
        };
        GsPrim decodePrim(uint32_t bits);

        struct GsTex0
        {
            uint32_t tbp = 0, tbw = 0, psm = 0, tw = 0, th = 0, tcc = 0, tfx = 0, cbp = 0, cpsm = 0, csm = 0, csa = 0, cld = 0;
        };
        GsTex0 decodeTex0(uint64_t tex0);

        struct GsVertex
        {
            float x = 0, y = 0; // GS primitive coordinates over 16
            uint32_t z = 0;
            float s = 0, t = 0, q = 1.0f;
            uint32_t rgba = 0x80808080u;
            float fog = 1.0f;
        };

        struct GsMaterial
        {
            GsPrim prim;
            GsTex0 tex0;
            uint64_t tex1 = 0, clamp = 0, alpha = 0x44u, test = 0;
            bool testFromTarget = false; // TEST as the draws before it left it
            std::shared_ptr<const bozo::render::Texture> texture;
            // A page the game drew rather than uploaded: 0 the frame so far, else the Renderer's target of that number; its size in GS pixels.
            int8_t frameSource = -1;
            uint16_t sourceWidth = 0, sourceHeight = 0;
            bool dropped = false; // its page is not in the packs: linked as built, nothing drawn
        };

        // A builder's batches, kept by a key until the game's link call says where they go (or discard).
        void drawKeyed(uint64_t key, const GsMaterial &material, const GsVertex *vertices, uint32_t count);
        void discard(uint64_t key);
        void releaseKeyed();
        // The key's batches as one draw of the ordering table (taken: the key is free again).
        ot::Draw takeKeyed(uint64_t key);
        // The Renderer's material and triangles of a GS draw: the sprites and lines as two triangles.
        void toRenderer(GsMaterial &material, const GsVertex *vertices, uint32_t count, bozo::render::Material &out,
                        std::vector<bozo::render::Vertex> &triangles);
        // Textures sampled with the mip levels TEX1 asks for (SC3_MIPMAPS=0: level 0 only, as before).
        bool mipmapsOn();
        extern const float kIdentity[16];
    }

    // Guest code run to its end from inside an override, on a context of its own: on the thread's context an event due would make
    // it leave the override halfway (kernel.md section 3b). Registers and flags come back to ctx, the thread's time goes on.
    inline void runGuest(RecompiledFunction function, uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
    {
        auto own = std::make_unique<R5900Context>(*ctx);
        own->leaving = 0;
        function(rdram, own.get(), runtime);
        const uint64_t cycles = own->cycles, checkpoints = own->checkpoints, limit = ctx->cycleLimit;
        const uint32_t leaving = ctx->leaving;
        *ctx = *own;
        ctx->cycleLimit = limit;
        ctx->leaving = leaving;
        ctx->cycles = cycles;
        ctx->checkpoints = checkpoints;
    }

    // A game function by its address, as generated (its override, if any, not taken).
    RecompiledFunction guestFunction(uint32_t address);

    // The R5900's FPU and VU0 float rules, on host floats holding the guest's bits.
    namespace fp
    {
        inline uint32_t bits(float v)
        {
            uint32_t b;
            std::memcpy(&b, &v, 4);
            return b;
        }
        inline float flt(uint32_t b)
        {
            float v;
            std::memcpy(&v, &b, 4);
            return v;
        }
        inline float ps2_fadd(float a, float b) { uint32_t f = 0; return flt(bozo::ps2f::add(bits(a), bits(b), f)); }
        inline float ps2_fsub(float a, float b) { uint32_t f = 0; return flt(bozo::ps2f::sub(bits(a), bits(b), f)); }
        inline float ps2_fmul(float a, float b) { uint32_t f = 0; return flt(bozo::ps2f::mul(bits(a), bits(b), f)); }
        inline float ps2_fdiv(float a, float b) { uint32_t f = 0; return flt(bozo::ps2f::div(bits(a), bits(b), f)); }
        // cvt.w.s
        int32_t ps2FpuCvtWS(float v);

        inline bool FPU_C_OLT_S(float a, float b) { return bozo::ps2f::lt(bits(a), bits(b)); }
        inline float FPU_CVT_S_W(uint32_t i) { return flt(bozo::ps2f::fromInt(i)); }
        inline float FPU_ADD_S(float a, float b) { return ps2_fadd(a, b); }
        inline float FPU_SUB_S(float a, float b) { return ps2_fsub(a, b); }
        inline float FPU_MUL_S(float a, float b) { return ps2_fmul(a, b); }
        inline float FPU_DIV_S(float a, float b) { return ps2_fdiv(a, b); }

        using Vec = __m128;
        template <typename F>
        inline Vec lanes(Vec a, Vec b, F op)
        {
            uint32_t x[4], y[4], r[4];
            std::memcpy(x, &a, 16);
            std::memcpy(y, &b, 16);
            uint32_t fl = 0;
            for (int i = 0; i < 4; ++i)
                r[i] = op(x[i], y[i], fl);
            Vec out;
            std::memcpy(&out, r, 16);
            return out;
        }
        inline Vec ps2_vadd(Vec a, Vec b) { return lanes(a, b, bozo::vu0::addf); }
        inline Vec ps2_vsub(Vec a, Vec b) { return lanes(a, b, bozo::vu0::subf); }
        inline Vec ps2_vmul(Vec a, Vec b) { return lanes(a, b, bozo::vu0::mulf); }
    }

    namespace twoD
    {
        // A small movie's current picture by its player handle (RGBA rows, alpha 0x80 = 1.0, and its number; rgba left as it
        // is while known is still that number): sc3_movies.cpp's decoder.
        using MoviePicture = bool (*)(uint32_t handle, uint32_t known, std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height,
                                      uint32_t &number);
        void setMoviePictures(MoviePicture pictures);
        // The 2D draws this player's pictures from the decoder: they need not be written into the game's memory.
        bool drawsMovie(uint32_t handle);
        void frameDone();
        void noteRequest(uint8_t *rdram, R5900Context *ctx, Runtime *runtime);
        void noteReadBefore(uint8_t *rdram, R5900Context *ctx, Runtime *runtime);
        void noteReadAfter(uint8_t *rdram, R5900Context *ctx, Runtime *runtime);
    }

    namespace loads
    {
        void install(Registry &runtime);
    }

    namespace offscreen
    {
        // The glow's half-size picture (FBP 0x162, TBP 0x2C40), the target it is drawn in.
        constexpr uint8_t kGlowHalf = 1u;
        void install(Registry &runtime);
        void frameDone();
        // The game's last pass on a frame (0x141C80's builders, FBP 0x8C): the frame through Highlight2 with the colour
        // 0x13D130 computes, faded by fog.
        void displayCopy(uint8_t *rdram, bozo::render::Renderer &renderer);
    }
}
