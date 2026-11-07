// What the game does with the frame drawn so far, with no GS. The sun flare's depth probe: 0x163650 reads an 8 x 8 block of Z
// around the sun through 0x13BDF0 (sceGsExecStoreImage, PSMZ32 into a0's buffer), answered from the Renderer's depth read back.
// The glow: 0x1725C0 builds a chain at the layer's end (the frame to half size, to quarter size, blurred back and forth half a
// texel, back to half size, added onto the frame with FIX), drawn here on the Renderer's targets. The shadows: tasks 0x1F70E0
// (set up), 0x1F8598 (each object's volume: two GIF streams by the sign of a triangle's winding, adding and taking away in a
// count target), 0x1F7530 (the count resolved) and 0x1F6C48 (the frame blended toward the shadow colour where it counted): the
// count is the Renderer's stencil.
#include "sc3_draw.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>

namespace sc3::offscreen
{
    namespace
    {
        constexpr uint32_t kStoreImage = 0x0013BDF0u;
        constexpr float kOffsetX = 2048.0f - 320.0f - 0.5f, kOffsetY = 2048.0f - 224.0f - 0.5f;
        constexpr uint32_t kGlowLayerEnd = 0x001725C0u, kGlowBufferBuild = 0x001705B0u, kGlowBufferDraw = 0x00172E90u;
        constexpr uint8_t kHalf = 1u, kQuarter = 2u, kBlur = 3u;

        constexpr uint32_t kShadowSetup = 0x001F70E0u, kShadowVolumes = 0x001F8598u, kShadowResolve = 0x001F7530u, kShadowDarken = 0x001F6C48u;
        constexpr uint32_t kEnvSlot = 0x00144198u, kColourLerp = 0x00102FA0u, kColourBytes = 0x0011AAF8u, kMatrixMul = 0x00101058u;
        constexpr uint32_t kShadowFlags = 0x004F8403u, kShadowWanted = 0x004F8402u, kGame = 0x0046DB10u, kView = 0x0046DA88u;
        constexpr uint32_t kSpr = 0x70000000u;
        constexpr uint32_t kDisplay = 0x00469234u, kDisplayCopy = 0x00141C80u;
        constexpr float kFadeOn = 1.0f / 255.0f;

        uint32_t load32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + (address & 0x01FFFFFFu), 4u);
            return value;
        }

        uint8_t *g_rdram = nullptr, *g_spr = nullptr;
        uint64_t g_frames = 0, g_glows = 0, g_setups = 0, g_volumes = 0, g_volumesDropped = 0, g_triangles = 0, g_darkens = 0;

        uint8_t *guest(uint32_t address)
        {
            return (address & 0xF0000000u) == kSpr ? g_spr + (address & 0x3FFFu) : g_rdram + (address & kRamMask);
        }

        template <typename T>
        T peek(uint32_t address)
        {
            T value;
            std::memcpy(&value, guest(address), sizeof(T));
            return value;
        }

        template <typename T>
        void poke(uint32_t address, T value)
        {
            std::memcpy(guest(address), &value, sizeof(T));
        }

        uint32_t peek32(uint32_t address) { return peek<uint32_t>(address); }
        float peekFloat(uint32_t address) { return peek<float>(address); }

        void returnFrom(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

        // A game function on the override's own context: a0.. from the list, f12, the result in v0.
        uint32_t callGame(uint32_t address, uint8_t *rdram, R5900Context *ctx, Runtime *runtime, std::initializer_list<uint32_t> args, float f12 = 0.0f)
        {
            auto own = std::make_unique<R5900Context>(*ctx);
            own->leaving = 0;
            int reg = 4;
            for (uint32_t arg : args)
                setRegU32(own.get(), reg++, arg);
            own->f[12] = f12;
            setRegU32(own.get(), 31, 0u);
            own->pc = address;
            guestFunction(address)(rdram, own.get(), runtime);
            ctx->cycles = own->cycles;
            ctx->checkpoints = own->checkpoints;
            return getRegU32(own.get(), 2);
        }

        struct Glow
        {
            uint32_t mode = 0, colour = 0, fix = 0, blurs = 0, reference = 0;
        };

        // A quad over the whole target: its corners in GS coordinates, u and v over the source.
        void wholeTarget(bozo::render::Renderer &renderer, const bozo::render::Material &material, uint32_t colour, float shiftU, float shiftV)
        {
            bozo::render::Vertex v[6];
            const float corners[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
            for (int i = 0; i < 6; ++i)
            {
                v[i].x = kOffsetX + corners[i][0] * 640.0f;
                v[i].y = kOffsetY + corners[i][1] * 448.0f;
                v[i].u = corners[i][0] + shiftU;
                v[i].v = corners[i][1] + shiftV;
                v[i].rgba = colour;
            }
            renderer.draw(material, draw::kIdentity, v, 6u);
        }

        bozo::render::Material targetCopy(uint8_t target, uint8_t source)
        {
            bozo::render::Material m;
            m.textured = true;
            m.blend = bozo::render::Material::Blend::Opaque;
            m.depthTest = m.depthWrite = false;
            m.fromFrame = 1u;
            m.target = target;
            m.source = source;
            m.frameWidth = 640u;
            m.frameHeight = 448u;
            return m;
        }

        void drawGlow(bozo::render::Renderer &renderer, const Glow &glow)
        {
            constexpr uint32_t kWhite = 0x80808080u;
            renderer.setTargetSize(kHalf, 320u, 224u);
            renderer.setTargetSize(kQuarter, 160u, 112u);
            renderer.setTargetSize(kBlur, 160u, 112u);
            wholeTarget(renderer, targetCopy(kHalf, 0u), kWhite, 0.0f, 0.0f);
            wholeTarget(renderer, targetCopy(kQuarter, kHalf), kWhite, 0.0f, 0.0f);
            const float texelU = 0.5f / 160.0f, texelV = 0.5f / 112.0f;
            for (uint32_t i = 0; i < glow.blurs; ++i)
            {
                wholeTarget(renderer, targetCopy(kBlur, kQuarter), kWhite, texelU, -texelV);
                wholeTarget(renderer, targetCopy(kQuarter, kBlur), kWhite, -texelU, texelV);
            }
            wholeTarget(renderer, targetCopy(kHalf, kQuarter), kWhite, 0.0f, 0.0f);
            bozo::render::Material add = targetCopy(0u, kHalf);
            const uint32_t code = glow.mode == 0x44u ? 0x64u : glow.mode == 0x42u ? 0x62u : glow.mode == 0x48u ? 0x68u : 0x0Au;
            if (code != 0x0Au)
            {
                add.blend = bozo::render::Material::Blend::Alpha;
                add.gsBlend = true;
                add.blendA = static_cast<uint8_t>(code & 3u);
                add.blendB = static_cast<uint8_t>((code >> 2) & 3u);
                add.blendC = static_cast<uint8_t>((code >> 4) & 3u);
                add.blendD = static_cast<uint8_t>((code >> 6) & 3u);
                add.blendFix = static_cast<uint8_t>(glow.fix);
            }
            add.colourMask = 7u;
            wholeTarget(renderer, add, glow.colour, 0.0f, 0.0f);
        }

        // 0x1725C0 (mode, colour, fix, reference, blurs): linked at the current layer's end as 0x10E1F8 links it. Its alpha test
        // on the frame's alpha has reference 0 from its one caller, which passes everything.
        void glowLayerEnd(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            Glow glow;
            glow.mode = getRegU32(ctx, 4);
            glow.colour = getRegU32(ctx, 5);
            glow.fix = getRegU32(ctx, 6);
            glow.blurs = std::min<uint32_t>(getRegU32(ctx, 8), 16u);
            uint32_t each;
            std::memcpy(&each, rdram + 0x005BE764u, 4);
            ++g_glows;
            if (ot::linkLayerBucket(rdram, static_cast<int>(each) - 1, [glow](bozo::render::Renderer &renderer) { drawGlow(renderer, glow); }))
            {
                uint8_t *spr = bozo::port::scratchpad(runtime);
                uint32_t half, base;
                std::memcpy(&half, spr + 4u, 4);
                half ^= 1u;
                std::memcpy(spr + 4u, &half, 4);
                std::memcpy(&base, spr + 8u + half * 4u, 4);
                std::memcpy(spr, &base, 4);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        void sunFlare(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            if (ot::hidden(0x00163650u))
                returnFrom(ctx);
            else
                func_0x163650(rdram, ctx, runtime);
        }

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

        // The glow an object keeps: 0x1705B0 (buffer, mode, colour, fix, reference, blurs) builds it into the object's buffer, then
        // 0x172E90 (object, its +0x50 the buffer) has it CALLed each frame through 0x104350's batch list, under the last 2D.
        std::map<uint32_t, Glow> g_glowBuffers;

        void glowBufferBuild(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            Glow &glow = g_glowBuffers[getRegU32(ctx, 4)];
            glow.mode = getRegU32(ctx, 5);
            glow.colour = getRegU32(ctx, 6);
            glow.fix = getRegU32(ctx, 7);
            glow.blurs = std::min<uint32_t>(getRegU32(ctx, 9), 16u);
            glow.reference = getRegU32(ctx, 8);
            std::fprintf(stderr, "[sc3:offscreen] glow buffer %#x: mode %#x colour %#x fix %#x reference %#x blurs %u\n", getRegU32(ctx, 4), glow.mode, glow.colour, glow.fix, glow.reference, glow.blurs);
            func_0x1705b0(rdram, ctx, runtime);
        }

        void glowBufferDraw(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            if (ot::hidden(kGlowBufferDraw))
            {
                returnFrom(ctx);
                return;
            }
            const auto it = g_glowBuffers.find(peek32(getRegU32(ctx, 4) + 0x50u));
            if (it == g_glowBuffers.end())
            {
                func_0x172e90(rdram, ctx, runtime);
                return;
            }
            const Glow glow = it->second;
            ++g_glows;
            ot::linkLayerBucket(rdram, static_cast<int>(peek32(0x005BE764u)) - 1, [glow](bozo::render::Renderer &renderer) { drawGlow(renderer, glow); });
            returnFrom(ctx);
        }

        // ---- the shadows

        void taskDone() { poke<uint8_t>(peek32(kSpr + 0x24u) + 0x25u, 1u); }

        bozo::render::Material volumeMaterial()
        {
            bozo::render::Material m;
            m.textured = false;
            m.blend = bozo::render::Material::Blend::Opaque;
            m.depthTest = true;
            m.depthWrite = false;
            m.colourMask = 0u;
            return m;
        }

        // 0x1F70E0: whether this frame has shadows (0x4F8403: bit 0 on, bit 2 none this frame), then the count emptied.
        void shadowSetup(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            returnFrom(ctx);
            const int8_t before = peek<int8_t>(kShadowFlags);
            uint8_t flags = peek<uint8_t>(kGame + 0xAC1u) == 0u && peek<int8_t>(kShadowWanted) > 0 ? 1u : 0u;
            poke<uint8_t>(kShadowFlags, flags);
            if (flags == 0u)
            {
                poke<uint8_t>(kShadowWanted, 0u);
                taskDone();
                return;
            }
            if (peek<uint8_t>(kGame + 0xACBu) != 0u || peek32(peek32(0x00470BF0u) + 0x158u) == 0u)
                poke<uint8_t>(kShadowFlags, flags | 4u);
            if (peek32(peek32(kSpr + 0x24u) + 0x40u) == 0u)
            {
                flags = peek<uint8_t>(kShadowFlags);
                uint32_t which = peek32(0x004B487Cu) == 0u ? 1u : 0u;
                if (peek32(0x004D1FFCu) == 0u)
                    which |= 2u;
                if ((flags & 4u) == 0u)
                {
                    if (which == 1u)
                        poke<uint8_t>(kShadowFlags, flags | 3u);
                    else if (which == 2u)
                        poke<uint8_t>(kShadowFlags, static_cast<uint8_t>((flags & ~3u) | 2u));
                    else if (which == 3u)
                        poke<uint8_t>(kShadowFlags, flags | 4u);
                }
            }
            if (peek<uint8_t>(kShadowFlags) & 4u)
            {
                poke<uint8_t>(kShadowFlags, static_cast<uint8_t>((peek<uint8_t>(kShadowFlags) & 0xFEu) | (static_cast<uint8_t>(before) & 1u)));
                return;
            }
            ++g_setups;
            ot::appendGif(rdram, [](bozo::render::Renderer &renderer)
                          {
                              bozo::render::Material m = volumeMaterial();
                              m.depthTest = false;
                              m.stencilClear = true;
                              wholeTarget(renderer, m, 0u, 0.0f, 0.0f);
                          });
            poke<uint32_t>(kSpr + 0x64u, 0xFFFFu);
            poke<uint32_t>(kSpr + 0x6Cu, 0u);
            poke<uint32_t>(kSpr + 0x60u, 0xFFFFu);
            poke<uint32_t>(kSpr + 0x68u, 0u);
        }

        struct V4
        {
            float x = 0, y = 0, z = 0, w = 0;
        };

        // 0x1F8598 (object): the volume as a triangle strip (+8 count, at most 247; +0xC 0x40-byte entries, the point at +0x10, +0xC
        // not 0 for a vertex that only starts a strip), taken to the screen by the view's matrix (times the object's +0x10 when
        // +0x1D), each triangle counted up or down by the sign of its screen winding and which way the object's +0 goes. A point
        // nearer than w 0.08 or off the GS's coordinate range drops the object (+0x2C -1). +0xA: the GIF list (0), else depth 0.1.
        void shadowVolumes(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            returnFrom(ctx);
            const uint32_t object = getRegU32(ctx, 4);
            if (peek<uint8_t>(object + 0x1Eu) == 0u)
                return;
            uint32_t entry = peek32(object + 0xCu);
            const uint32_t count = std::min<uint32_t>(peek<uint16_t>(object + 8u), 0xF7u);
            if (entry == 0u || (entry & 0xFu) != 0u)
                return;
            float m[16];
            uint32_t matrix = peek32(peek32(kView) + 0x448u);
            const uint32_t sp = getRegU32(ctx, 29) - 0x90u;
            if (peek<uint8_t>(object + 0x1Du) != 0u)
            {
                setRegU32(ctx, 29, sp);
                callGame(kMatrixMul, rdram, ctx, runtime, {sp, matrix, peek32(object + 0x10u)});
                setRegU32(ctx, 29, sp + 0x90u);
                matrix = sp;
            }
            for (int i = 0; i < 16; ++i)
                m[i] = peekFloat(matrix + static_cast<uint32_t>(i) * 4u);
            const bool firstAdds = peek<uint64_t>(object) == 0x42u;
            auto up = std::make_shared<std::vector<bozo::render::Vertex>>(), down = std::make_shared<std::vector<bozo::render::Vertex>>();
            bozo::render::Vertex history[2];
            V4 p17, p18;
            for (uint32_t i = 0; i < count; ++i, entry += 0x40u)
            {
                const float x = peekFloat(entry + 0x10u), y = peekFloat(entry + 0x14u), z = peekFloat(entry + 0x18u);
                V4 v;
                v.x = m[12] + m[0] * x + m[4] * y + m[8] * z;
                v.y = m[13] + m[1] * x + m[5] * y + m[9] * z;
                v.z = m[14] + m[2] * x + m[6] * y + m[10] * z;
                v.w = m[15] + m[3] * x + m[7] * y + m[11] * z;
                if (v.w < 0.08f)
                {
                    poke<float>(object + 0x2Cu, -1.0f);
                    ++g_volumesDropped;
                    return;
                }
                const float q = 1.0f / v.w;
                v.x *= q;
                v.y *= q;
                v.z *= q;
                const int32_t ix = fp::ps2FpuCvtWS(v.x * 16.0f), iy = fp::ps2FpuCvtWS(v.y * 16.0f), iz = fp::ps2FpuCvtWS(v.z * 16.0f);
                if (((static_cast<uint32_t>(ix) >> 16) | (static_cast<uint32_t>(iy) >> 16)) != 0u)
                {
                    poke<float>(object + 0x2Cu, -1.0f);
                    ++g_volumesDropped;
                    return;
                }
                bozo::render::Vertex g;
                g.x = static_cast<float>(ix) / 16.0f;
                g.y = static_cast<float>(iy) / 16.0f;
                g.z = static_cast<float>((static_cast<uint32_t>(iz) >> 4) & 0xFFFFFFu);
                g.rgba = 0x80808080u;
                if (peek32(entry + 0xCu) != 0u)
                {
                    p18 = p17;
                    p17 = v;
                }
                else
                {
                    const float e1x = (i & 1u) ? p18.x - v.x : v.x - p18.x, e1y = (i & 1u) ? p18.y - v.y : v.y - p18.y;
                    const float e2x = (i & 1u) ? p17.x - v.x : p17.x - p18.x, e2y = (i & 1u) ? p17.y - v.y : p17.y - p18.y;
                    p18 = p17;
                    p17 = v;
                    const bool first = std::signbit(e1x * e2y - e2x * e1y);
                    if (i >= 2u)
                    {
                        std::vector<bozo::render::Vertex> &into = first == firstAdds ? *up : *down;
                        into.insert(into.end(), {history[0], history[1], g});
                    }
                }
                history[0] = history[1];
                history[1] = g;
            }
            poke<float>(object + 0x2Cu, 1.0f);
            ++g_volumes;
            g_triangles += (up->size() + down->size()) / 3u;
            ot::Draw draw = [up, down](bozo::render::Renderer &renderer)
            {
                bozo::render::Material m = volumeMaterial();
                m.stencilOp = bozo::render::Material::StencilOp::IncrementWrap;
                if (!up->empty())
                    renderer.draw(m, draw::kIdentity, up->data(), static_cast<uint32_t>(up->size()));
                m.stencilOp = bozo::render::Material::StencilOp::DecrementWrap;
                if (!down->empty())
                    renderer.draw(m, draw::kIdentity, down->data(), static_cast<uint32_t>(down->size()));
            };
            if (peek<uint8_t>(object + 0xAu) == 0u)
                ot::appendGif(rdram, std::move(draw));
            else
            {
                const float z = fp::ps2_fmul(0.1f, 0.12f);
                ot::linkLayerBucket(rdram, fp::ps2FpuCvtWS(fp::ps2_fmul(z, peekFloat(0x005BE734u))), std::move(draw));
            }
        }

        // 0x1F7530: the count target read as Z16S: nothing to draw with a stencil.
        void shadowResolve(uint8_t *, R5900Context *ctx, Runtime *)
        {
            returnFrom(ctx);
            if (peek<uint8_t>(kShadowFlags) == 0u)
                taskDone();
        }

        // 0x1F6C48: the shadow colour (two of the environment's colour slots, mixed by which of two lights is on, or by their
        // weights), the frame blended toward it where a volume counted.
        void shadowDarken(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            returnFrom(ctx);
            if (peek<uint8_t>(kGame + 0xACBu) != 0u || peek32(peek32(0x00470BF0u) + 0x158u) == 0u)
                return;
            const uint8_t flags = peek<uint8_t>(kShadowFlags);
            if (flags == 0u)
            {
                taskDone();
                return;
            }
            if (flags & 4u)
                return;
            const uint32_t slots = peek32(0x00470BF4u);
            const auto slot = [&](uint32_t which) { return slots + callGame(kEnvSlot, rdram, ctx, runtime, {which}) * 16u + 0x1510u; };
            const auto copy16 = [&](uint32_t from, uint32_t to)
            {
                poke<uint64_t>(to, peek<uint64_t>(from));
                poke<uint64_t>(to + 8u, peek<uint64_t>(from + 8u));
            };
            if ((flags & 3u) == 2u)
            {
                const uint32_t from = slot(0x80000002u);
                copy16(from, slot(0x80000004u));
            }
            else if ((flags & 3u) == 3u)
            {
                const uint32_t from = slot(0x80000004u);
                copy16(from, slot(0x80000002u));
            }
            uint32_t a = slot(0x80000002u), b = slot(0x80000004u);
            uint32_t lights = peekFloat(0x004B6954u) < -1.0f ? 1u : 0u;
            if (peekFloat(0x004D40D4u) < -1.0f)
                lights |= 2u;
            if (lights == 1u)
                a = b;
            else if (lights == 2u)
                b = a;
            const uint32_t sp = getRegU32(ctx, 29) - 0x60u;
            const uint32_t mixed = sp + 0x10u;
            setRegU32(ctx, 29, sp);
            const float wa = peekFloat(a + 0xCu), wb = peekFloat(b + 0xCu);
            const auto lerp = [&](float t) { callGame(kColourLerp, rdram, ctx, runtime, {mixed, a, b}, t); };
            if (peek<uint8_t>(kGame + 0xAD0u) < 2u)
                lerp(wb < wa ? 0.75f : 0.25f);
            else if (wa < 1.0f / 128.0f)
                copy16(b, mixed);
            else if (wb < 1.0f / 128.0f)
                copy16(a, mixed);
            else
                lerp(wa < wb ? 0.75f : 0.25f);
            const uint32_t colour = callGame(kColourBytes, rdram, ctx, runtime, {mixed}, 255.0f);
            setRegU32(ctx, 29, sp + 0x60u);
            ++g_darkens;
            if (ot::hidden(0x001F6C48u))
                return;
            ot::appendGif(rdram, [colour](bozo::render::Renderer &renderer)
                          {
                              bozo::render::Material m;
                              m.textured = false;
                              m.blend = bozo::render::Material::Blend::Alpha;
                              m.gsBlend = true;
                              m.depthTest = m.depthWrite = false;
                              m.stencilNotZero = true;
                              m.colourMask = 7u;
                              wholeTarget(renderer, m, colour, 0.0f, 0.0f);
                          });
        }

        template <void (*Hook)(uint8_t *, R5900Context *, Runtime *)>
        void hooked(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            g_rdram = rdram;
            g_spr = bozo::port::scratchpad(runtime);
            Hook(rdram, ctx, runtime);
        }
    }

    void displayCopy(uint8_t *rdram, bozo::render::Renderer &renderer)
    {
        if (ot::hidden(kDisplayCopy))
            return;
        const uint32_t display = load32(rdram, kDisplay);
        if (display == 0u)
            return;
        bozo::render::Material copy = targetCopy(0u, 0u);
        copy.nearest = true;
        copy.textureFunction = bozo::render::Material::TextureFunction::Highlight2;
        float fade = 0.0f;
        std::memcpy(&fade, rdram + display + 0xE0u, 4u);
        copy.fog = fade > kFadeOn;
        copy.fogColour = load32(rdram, display + 0x60u);
        bozo::render::Vertex v[6];
        const float corners[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < 6; ++i)
        {
            v[i].x = kOffsetX + corners[i][0] * 640.0f;
            v[i].y = kOffsetY + corners[i][1] * 448.0f;
            v[i].u = corners[i][0];
            v[i].v = corners[i][1];
            v[i].rgba = load32(rdram, display + 0x64u);
            v[i].fog = static_cast<float>(load32(rdram, display + 0x68u) & 0xFFu) / 255.0f;
        }
        renderer.draw(copy, draw::kIdentity, v, 6u);
        // 1, 2, 1 over three lines: the copy samples half a line up (v 0 on bilinear), PMODE 0x8063 averages DISPFB1 (DBY 1) and DISPFB2.
        bozo::render::Material line = targetCopy(0u, 0u);
        line.nearest = true;
        line.gsBlend = true;
        line.blendA = 0u, line.blendB = 1u, line.blendC = 2u, line.blendD = 1u, line.blendFix = 0x40u;
        wholeTarget(renderer, line, 0x80808080u, 0.0f, 1.0f / 448.0f);
        wholeTarget(renderer, line, 0x80808080u, 0.0f, -1.0f / 448.0f);
    }

    void frameDone()
    {
        if (++g_frames % 250u != 0u)
            return;
        std::fprintf(stderr, "[sc3:offscreen] 250 frames: glows %llu; shadows: set up %llu, volumes %llu (%llu triangles, %llu dropped), darkened %llu\n", static_cast<unsigned long long>(g_glows),
                     static_cast<unsigned long long>(g_setups), static_cast<unsigned long long>(g_volumes), static_cast<unsigned long long>(g_triangles), static_cast<unsigned long long>(g_volumesDropped),
                     static_cast<unsigned long long>(g_darkens));
        g_glows = g_setups = g_volumes = g_triangles = g_volumesDropped = g_darkens = 0;
    }

    void install(Registry &runtime)
    {
        runtime.registerFunction(kShadowSetup, &hooked<shadowSetup>);
        runtime.registerFunction(kShadowVolumes, &hooked<shadowVolumes>);
        runtime.registerFunction(kShadowResolve, &hooked<shadowResolve>);
        runtime.registerFunction(kShadowDarken, &hooked<shadowDarken>);
        runtime.registerFunction(kStoreImage, &storeImage);
        runtime.registerFunction(kGlowLayerEnd, &hooked<glowLayerEnd>);
        runtime.registerFunction(kGlowBufferBuild, &hooked<glowBufferBuild>);
        runtime.registerFunction(kGlowBufferDraw, &hooked<glowBufferDraw>);
        runtime.registerFunction(0x00163650u, &hooked<sunFlare>);
    }
}
