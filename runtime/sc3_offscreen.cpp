#include "sc3_offscreen.h"
#include "sc3_2d.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/native/native_gs_hook.h"
#include "runtime/native/native_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc3
{
    namespace offscreen
    {
    namespace
    {
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kGifListAppend = 0x0010F808u;
        // 0x1725C0 (alpha, colour, fix, alpha ref, blurs): the glow (0x17006C's chain: the frame to 0x162 at half size,
        // 0x185 a quarter, blurred into 0x191 and back, 0x162 again, added onto the frame) inserted at the current layer's
        // end. 0x172D98 builds the same chain into an object's own buffer (0x172510, by depth, is not reached: no caller).
        constexpr uint32_t kGlowLayerEnd = 0x001725C0u, kGlowBuffer = 0x00172D98u;
        // 0x1705B0 (buffer, alpha, colour, fix, alpha ref, blurs): 0x172D98's build of the chain into the object's buffer
        // (+0x50) once, a RET-ended list; 0x172E90 (object) adds a CALL of it to the view's list (0x104350) each frame.
        constexpr uint32_t kGlowBufferBuild = 0x001705B0u, kGlowBufferDraw = 0x00172E90u;
        // The shadow tasks, in their order in the GIF list: the count target 0x168 cleared and context 2 set for the
        // volumes; each object's volume strip (0x1F8598); 0x168 read as Z16S into 0x1AE; the frame darkened where 0x1AE
        // as Z says a volume counted.
        constexpr uint32_t kShadowSetup = 0x001F70E0u, kShadowVolumes = 0x001F8598u, kShadowResolve = 0x001F7530u,
                           kShadowDarken = 0x001F6C48u;
        // 0x163650's read of an 8 x 8 block of Z around the sun (sceGsExecStoreImage, PSMZ32) through 0x13BDF0.
        constexpr uint32_t kStoreImage = 0x0013BDF0u, kProbeReturn = 0x001637F0u;
        constexpr uint32_t kEnvSlot = 0x00144198u, kColourLerp = 0x00102FA0u, kColourBytes = 0x0011AAF8u, kMatrixMul = 0x00101058u;
        constexpr uint32_t kContextPacket = 0x0013B3F0u;

        constexpr uint32_t kShadowFlags = 0x004F8403u, kShadowWanted = 0x004F8402u;
        constexpr uint32_t kScratchpad = 0x70000000u;
        constexpr uint32_t kView = 0x0046DA88u;
        constexpr uint32_t kGame = 0x0046DB10u;
        constexpr uint32_t kDisplay = 0x00469234u;
        // The frame the passes draw into: FRAME_2 0xA0000 (fbp 0, 640 wide, CT32), its Z buffer 0xD2 (Z24), XYOFFSET_2.
        constexpr uint64_t kFrame = 0x000A0000ull, kZbuf = 0x1010000D2ull, kXyOffset = (0x7200ull << 32) | 0x6C00ull;
        constexpr float kOffsetX = 1728.0f, kOffsetY = 1824.0f;

        using Function = PS2Runtime::RecompiledFunction;
        using ps2x::native::GsVertex;
        using Pass = ps2x::native::Renderer::TargetPass;

        enum Feature
        {
            Glow,
            Shadows,
            Flare,
            Features,
        };
        constexpr const char *kSwitchNames[Features] = {"SC3_NATIVE_GLOW", "SC3_NATIVE_SHADOWS", "SC3_NATIVE_FLARE"};

        struct Switch
        {
            bool on = false;
            std::string path;
            std::chrono::steady_clock::time_point read{};
        };

        struct Stats
        {
            uint64_t glowNative = 0, glowGame = 0, glowBuffer = 0, glowDropped = 0;
            uint64_t setups = 0, volumes = 0, volumeTriangles = 0, volumesAborted = 0, resolves = 0, darkens = 0, shadowDropped = 0;
            uint64_t probes = 0, probeAnswered = 0, probeUnknown = 0, probeMarkers = 0;
            uint64_t gameSetups = 0, gameVolumes = 0, gameResolves = 0, gameDarkens = 0, gameProbes = 0;
        };

        struct GlowBuffer
        {
            uint32_t mode = 0, colour = 0, fix = 0, ref = 0, blurs = 0;
            uint64_t key = 0;
            bool native = false;
        };

        struct State
        {
            std::unordered_map<uint32_t, GlowBuffer> glowBuffers;
            PS2Runtime *runtime = nullptr;
            uint8_t *rdram = nullptr, *scratchpad = nullptr;
            std::unordered_map<uint32_t, Function> originals;
            Switch switches[Features];
            Stats stats;
            uint64_t frames = 0;
        };

        State g_state;

        uint8_t *guest(uint32_t address, uint32_t size)
        {
            if ((address & 0xF0000000u) == kScratchpad)
                return g_state.scratchpad + (address & 0x3FFFu & ~(size - 1u));
            return g_state.rdram + (address & PS2_RAM_MASK & ~(size - 1u));
        }

        template <typename T>
        T peek(uint32_t address)
        {
            T value;
            std::memcpy(&value, guest(address, sizeof(T)), sizeof(T));
            return value;
        }

        template <typename T>
        void poke(uint32_t address, T value)
        {
            std::memcpy(guest(address, sizeof(T)), &value, sizeof(T));
        }

        uint8_t peek8(uint32_t address) { return peek<uint8_t>(address); }
        uint16_t peek16(uint32_t address) { return peek<uint16_t>(address); }
        uint32_t peek32(uint32_t address) { return peek<uint32_t>(address); }
        float peekFloat(uint32_t address) { return peek<float>(address); }

        bool on(Feature feature)
        {
            Switch &s = g_state.switches[feature];
            if (s.path.empty())
                return s.on;
            const auto now = std::chrono::steady_clock::now();
            if (now - s.read >= std::chrono::milliseconds(200))
            {
                s.read = now;
                std::ifstream file(s.path);
                std::string line;
                const bool was = s.on;
                s.on = std::getline(file, line) && !line.empty() && line[0] == '1';
                if (s.on != was)
                    std::fprintf(stderr, "[sc3:offscreen] %s %s\n", kSwitchNames[feature], s.on ? "on" : "off");
            }
            return s.on;
        }

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_state.originals.at(address)(rdram, ctx, runtime);
        }

        // A game function run to its return from C++ (generated code may go back to the dispatcher at a call), with
        // the arguments given and a stack frame of its own below the caller's; returns v0.
        uint64_t sx(uint32_t value) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value))); }

        uint32_t callGame(uint32_t address, R5900Context *ctx, std::initializer_list<uint64_t> args, float f12 = 0.0f)
        {
            PS2Runtime *runtime = g_state.runtime;
            uint8_t *rdram = g_state.rdram;
            R5900Context saved = *ctx;
            int reg = 4;
            for (const uint64_t value : args)
                SET_GPR_U64(ctx, reg++, value);
            ctx->f[12] = f12;
            const uint32_t sp = getRegU32(ctx, 29);
            SET_GPR_U32(ctx, 31, kShadowDarken);
            ctx->pc = address;
            runtime->lookupFunction(address)(rdram, ctx, runtime);
            while (ctx->pc != kShadowDarken || getRegU32(ctx, 29) != sp)
                runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
            const uint32_t v0 = getRegU32(ctx, 2);
            *ctx = saved;
            return v0;
        }

        Pass framePass(Pass::Kind kind)
        {
            Pass p;
            p.kind = kind;
            p.frame = kFrame;
            p.zbuf = kZbuf;
            p.xyoffset = kXyOffset;
            p.width = 640u;
            p.height = 448u;
            return p;
        }

        bool queue(Pass pass, MarkerPlace place, float depth = 0.0f)
        {
            const uint64_t key = nativeMarkerKey();
            ps2x::native::Renderer::instance().passKeyed(key, std::move(pass));
            return linkNativeMarker(key, place, depth);
        }

        // ---- the glow

        uint64_t glowAlpha(uint32_t mode, uint32_t fix)
        {
            const uint64_t code = mode == 0x44u ? 0x64u : mode == 0x42u ? 0x62u : mode == 0x48u ? 0x68u : 0x0Au;
            return code | (static_cast<uint64_t>(fix & 0xFFu) << 32);
        }

        void glow(R5900Context *ctx, MarkerPlace place)
        {
            Pass p = framePass(Pass::Kind::Glow);
            p.alpha = glowAlpha(getRegU32(ctx, 4), getRegU32(ctx, 6));
            p.rgba = getRegU32(ctx, 5);
            p.alphaRef = static_cast<uint8_t>(getRegU32(ctx, 7));
            p.blurs = std::min<uint32_t>(getRegU32(ctx, 8), 16u);
            if (queue(std::move(p), place, ctx->f[12]))
                ++g_state.stats.glowNative;
            else
                ++g_state.stats.glowDropped;
        }

        void glowLayerEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!on(Glow) || !nativeMarkers())
            {
                ++g_state.stats.glowGame;
                callOriginal(kGlowLayerEnd, rdram, ctx, runtime);
                return;
            }
            glow(ctx, MarkerPlace::LayerEnd);
        }

        void glowBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_state.stats.glowBuffer;
            callOriginal(kGlowBuffer, rdram, ctx, runtime);
        }

        // The buffer as the game builds it (its own packet, the glow on the GS), its parameters kept.
        void glowBufferBuild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            GlowBuffer &b = g_state.glowBuffers[getRegU32(ctx, 4)];
            b.mode = getRegU32(ctx, 5);
            b.colour = getRegU32(ctx, 6);
            b.fix = getRegU32(ctx, 7);
            b.ref = getRegU32(ctx, 8);
            b.blurs = getRegU32(ctx, 9);
            b.native = false;
            callOriginal(kGlowBufferBuild, rdram, ctx, runtime);
        }

        // Each frame, before the game adds its CALL: with the glow native the buffer holds a marker (a CNT of no data,
        // the key in its upper half) then a RET, and the key's standing pass draws the glow where the GS reaches it;
        // switched back, the game's packet is built into it again.
        void glowBufferDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t buffer = peek32(getRegU32(ctx, 4) + 0x50u);
            const auto it = g_state.glowBuffers.find(buffer);
            if (it != g_state.glowBuffers.end())
            {
                GlowBuffer &b = it->second;
                const bool native = on(Glow) && nativeMarkers();
                if (native && !b.native)
                {
                    if (b.key == 0u)
                        b.key = nativeMarkerKey();
                    Pass p = framePass(Pass::Kind::Glow);
                    p.alpha = glowAlpha(b.mode, b.fix);
                    p.rgba = b.colour;
                    p.alphaRef = static_cast<uint8_t>(b.ref);
                    p.blurs = std::min<uint32_t>(b.blurs, 16u);
                    ps2x::native::Renderer::instance().standingPassKeyed(b.key, std::move(p));
                    poke<uint64_t>(buffer, 0x10000000ull);
                    poke<uint64_t>(buffer + 8u, b.key);
                    poke<uint64_t>(buffer + 16u, 0x60000000ull);
                    poke<uint64_t>(buffer + 24u, 0u);
                    b.native = true;
                }
                else if (!native && b.native)
                {
                    callGame(kGlowBufferBuild, ctx,
                             {buffer, b.mode, b.colour, b.fix, b.ref, b.blurs});
                    b.native = false;
                }
                if (b.native)
                    ++g_state.stats.glowNative;
                else
                    ++g_state.stats.glowGame;
            }
            callOriginal(kGlowBufferDraw, rdram, ctx, runtime);
        }

        // ---- the shadows

        void taskDone() { poke<uint8_t>(peek32(kScratchpad + 0x24u) + 0x25u, 1u); }

        // The depth probe's marker: the target's depth after the 3D, read where the GS stream reaches it.
        void probeMarker()
        {
            if (!on(Flare) || !nativeMarkers())
                return;
            if (queue(framePass(Pass::Kind::Probe), MarkerPlace::GifList))
            {
                ++g_state.stats.probeMarkers;
                switchPacketHalf();
            }
        }

        // 0x1F70E0, the shadows' set-up task: whether this frame has shadows (0x4F8403: bit 0 on, bit 2 none this frame),
        // then the count target cleared and context 2 set for the volumes (two packets).
        void shadowSetup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            probeMarker();
            if (!on(Shadows) || !nativeMarkers())
            {
                ++g_state.stats.gameSetups;
                callOriginal(kShadowSetup, rdram, ctx, runtime);
                return;
            }
            const int8_t before = peek<int8_t>(kShadowFlags);
            uint8_t flags = peek8(kGame + 0xAC1u) == 0u && peek<int8_t>(kShadowWanted) > 0 ? 1u : 0u;
            poke<uint8_t>(kShadowFlags, flags);
            if (flags == 0u)
            {
                poke<uint8_t>(kShadowWanted, 0u);
                taskDone();
                return;
            }
            if (peek8(kGame + 0xACBu) != 0u || peek32(peek32(0x00470BF0u) + 0x158u) == 0u)
                poke<uint8_t>(kShadowFlags, flags | 4u);
            if (peek32(peek32(kScratchpad + 0x24u) + 0x40u) == 0u)
            {
                flags = peek8(kShadowFlags);
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
            if (peek8(kShadowFlags) & 4u)
            {
                poke<uint8_t>(kShadowFlags, static_cast<uint8_t>((peek8(kShadowFlags) & 0xFEu) | (static_cast<uint8_t>(before) & 1u)));
                return;
            }
            Pass p = framePass(Pass::Kind::Volumes);
            p.clear = true;
            if (queue(std::move(p), MarkerPlace::GifList))
                ++g_state.stats.setups;
            else
                ++g_state.stats.shadowDropped;
            switchPacketHalf();
            poke<uint32_t>(kScratchpad + 0x64u, 0xFFFFu);
            poke<uint32_t>(kScratchpad + 0x6Cu, 0u);
            poke<uint32_t>(kScratchpad + 0x60u, 0xFFFFu);
            poke<uint32_t>(kScratchpad + 0x68u, 0u);
        }

        struct V4
        {
            float x = 0, y = 0, z = 0, w = 0;
        };

        int32_t ftoi4(float v) { return ps2FpuCvtWS(v * 16.0f); }

        // 0x1F8598 (object): the object's volume as a triangle strip (+8 count, at most 247; +0xC 0x40-byte entries, the
        // point at +0x10, +0xC not 0 for a vertex that only starts a strip), taken to the screen by the view's matrix (times
        // the object's +0x10 when +0x1D) on VU0, each triangle in the GS's two packets by the sign of its screen winding:
        // the first's ALPHA adds where the object's +0 is 0x42, the second's takes away. A point nearer than w 0.08 or off
        // the GS's coordinate range drops the whole object (+0x2C -1). +0xA: in the GIF list (0), else by depth 0.1.
        void shadowVolumes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!on(Shadows) || !nativeMarkers())
            {
                ++g_state.stats.gameVolumes;
                callOriginal(kShadowVolumes, rdram, ctx, runtime);
                return;
            }
            const uint32_t object = getRegU32(ctx, 4);
            if (peek8(object + 0x1Eu) == 0u)
                return;
            uint32_t entry = peek32(object + 0xCu);
            const uint32_t count = std::min<uint32_t>(peek16(object + 8u), 0xF7u);
            if (entry == 0u || (entry & 0xFu) != 0u)
                return;
            const uint32_t view = peek32(kView);
            float m[16];
            uint32_t matrix = peek32(view + 0x448u);
            const uint32_t sp = getRegU32(ctx, 29) - 0x90u;
            if (peek8(object + 0x1Du) != 0u)
            {
                SET_GPR_U32(ctx, 29, sp);
                callGame(kMatrixMul, ctx, {sp, matrix, peek32(object + 0x10u)});
                SET_GPR_U32(ctx, 29, sp + 0x90u);
                matrix = sp;
            }
            for (int i = 0; i < 16; ++i)
                m[i] = peekFloat(matrix + static_cast<uint32_t>(i) * 4u);
            const bool firstAdds = peek<uint64_t>(object) == 0x42u;
            std::vector<GsVertex> up, down;
            GsVertex history[2];
            V4 p16, p17, p18;
            bool abort = false;
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
                    abort = true;
                    break;
                }
                const float q = 1.0f / v.w;
                v.x *= q;
                v.y *= q;
                v.z *= q;
                const int32_t ix = ftoi4(v.x), iy = ftoi4(v.y), iz = ftoi4(v.z);
                if (((static_cast<uint32_t>(ix) >> 16) | (static_cast<uint32_t>(iy) >> 16)) != 0u)
                {
                    abort = true;
                    break;
                }
                GsVertex g;
                g.x = static_cast<float>(ix) / 16.0f;
                g.y = static_cast<float>(iy) / 16.0f;
                g.z = (static_cast<uint32_t>(iz) >> 4) & 0xFFFFFFu;
                if (peek32(entry + 0xCu) != 0u)
                {
                    p18 = p17;
                    p17 = v;
                }
                else
                {
                    float e1x, e1y, e2x, e2y;
                    if (i & 1u)
                    {
                        e1x = p18.x - v.x;
                        e1y = p18.y - v.y;
                        e2x = p17.x - v.x;
                        e2y = p17.y - v.y;
                    }
                    else
                    {
                        e1x = v.x - p18.x;
                        e1y = v.y - p18.y;
                        e2x = p17.x - p18.x;
                        e2y = p17.y - p18.y;
                    }
                    p18 = p17;
                    p17 = v;
                    const float cross = e1x * e2y - e2x * e1y;
                    const bool first = std::signbit(cross);
                    if (i >= 2u)
                    {
                        std::vector<GsVertex> &into = first == firstAdds ? up : down;
                        into.insert(into.end(), {history[0], history[1], g});
                    }
                }
                history[0] = history[1];
                history[1] = g;
                p16 = v;
            }
            (void)p16;
            if (abort)
            {
                poke<float>(object + 0x2Cu, -1.0f);
                ++g_state.stats.volumesAborted;
                return;
            }
            poke<float>(object + 0x2Cu, 1.0f);
            Pass p = framePass(Pass::Kind::Volumes);
            g_state.stats.volumeTriangles += (up.size() + down.size()) / 3u;
            p.up = std::move(up);
            p.down = std::move(down);
            if (queue(std::move(p), peek8(object + 0xAu) == 0u ? MarkerPlace::GifList : MarkerPlace::LayerDepth, 0.1f))
                ++g_state.stats.volumes;
            else
                ++g_state.stats.shadowDropped;
        }

        // 0x1F7530: the count target read as Z16S into 0x1AE (one packet): nothing to draw with a stencil.
        void shadowResolve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!on(Shadows) || !nativeMarkers())
            {
                ++g_state.stats.gameResolves;
                callOriginal(kShadowResolve, rdram, ctx, runtime);
                return;
            }
            const uint8_t flags = peek8(kShadowFlags);
            if (flags == 0u)
            {
                taskDone();
                return;
            }
            if (flags & 4u)
                return;
            switchPacketHalf();
            ++g_state.stats.resolves;
        }

        // 0x1F6C48: the shadow colour (two of the environment's colour slots, mixed by which of two lights is on, or by
        // their weights), the frame blended toward it (ALPHA 0x44) where a volume counted, then context 2 set back for
        // what follows (0x13B3F0's packet, the game's own).
        void shadowDarken(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!on(Shadows) || !nativeMarkers())
            {
                ++g_state.stats.gameDarkens;
                callOriginal(kShadowDarken, rdram, ctx, runtime);
                return;
            }
            if (peek8(kGame + 0xACBu) != 0u || peek32(peek32(0x00470BF0u) + 0x158u) == 0u)
                return;
            const uint8_t flags = peek8(kShadowFlags);
            if (flags == 0u)
            {
                taskDone();
                return;
            }
            if (flags & 4u)
                return;
            const uint32_t slots = peek32(0x00470BF4u);
            const auto slot = [&](uint32_t which) { return slots + callGame(kEnvSlot, ctx, {sx(which)}) * 16u + 0x1510u; };
            const auto copy16 = [&](uint32_t from, uint32_t to) {
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
            SET_GPR_U32(ctx, 29, sp);
            const float wa = peekFloat(a + 0xCu), wb = peekFloat(b + 0xCu);
            const auto lerp = [&](float t) { callGame(kColourLerp, ctx, {mixed, a, b}, t); };
            if (peek8(kGame + 0xAD0u) < 2u)
                lerp(wb < wa ? 0.75f : 0.25f);
            else if (wa < 1.0f / 128.0f)
                copy16(b, mixed);
            else if (wb < 1.0f / 128.0f)
                copy16(a, mixed);
            else
                lerp(wa < wb ? 0.75f : 0.25f);
            const uint32_t colour = callGame(kColourBytes, ctx, {mixed}, 255.0f);
            Pass p = framePass(Pass::Kind::Darken);
            p.rgba = colour;
            p.alpha = 0x44u;
            if (queue(std::move(p), MarkerPlace::GifList))
                ++g_state.stats.darkens;
            else
                ++g_state.stats.shadowDropped;
            const uint32_t display = peek32(kDisplay);
            const uint32_t packet = peek32(kScratchpad);
            poke<uint32_t>(sp, 0u);
            const uint64_t context = (static_cast<uint64_t>(peek32(0x00470FB4u)) << 32) | 2u;
            const uint32_t end = callGame(kContextPacket, ctx,
                                          {packet, sx(peek32(display + 4u)), 0u, sx(peek32(display + 0x10u)), 0u, 0x5002Du, context,
                                           sx(0x80000000u)});
            callGame(kGifListAppend, ctx, {end});
            SET_GPR_U32(ctx, 29, sp + 0x60u);
        }

        // ---- the sun's depth probe

        void storeImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 31) != kProbeReturn || !on(Flare))
            {
                if (getRegU32(ctx, 31) == kProbeReturn)
                    ++g_state.stats.gameProbes;
                callOriginal(kStoreImage, rdram, ctx, runtime);
                return;
            }
            const int32_t x = static_cast<int32_t>(getRegU32(ctx, 7)), y = static_cast<int32_t>(getRegU32(ctx, 8));
            const uint32_t w = std::min<uint32_t>(getRegU32(ctx, 9), 64u), h = std::min<uint32_t>(getRegU32(ctx, 10), 64u);
            const uint32_t dest = getRegU32(ctx, 6);
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            ++g_state.stats.probes;
            for (uint32_t j = 0; j < h; ++j)
                for (uint32_t i = 0; i < w; ++i)
                {
                    uint32_t z = 0;
                    if (renderer.depthAt(kOffsetX + static_cast<float>(x + static_cast<int32_t>(i)),
                                         kOffsetY + static_cast<float>(y + static_cast<int32_t>(j)), z))
                        ++g_state.stats.probeAnswered;
                    else
                        ++g_state.stats.probeUnknown;
                    poke<uint32_t>(dest + (j * w + i) * 4u, z & 0xFFFFFFu);
                }
        }

        // ---- frame

        void report()
        {
            State &s = g_state;
            if (++s.frames % 250u != 0u)
                return;
            const Stats &t = s.stats;
            std::fprintf(stderr,
                         "[sc3:offscreen] glow %s: %llu native, %llu by the game (%llu to an object's buffer), %llu dropped; "
                         "shadows %s: %llu set-ups, %llu volumes (%llu triangles, %llu dropped by the game's own tests), %llu resolves, "
                         "%llu darkens native, %llu not linked; by the game: %llu set-ups, %llu volumes, %llu resolves, %llu darkens; "
                         "flare %s: %llu probes native (%llu points answered, %llu not yet), %llu probe markers, %llu read from the GS\n",
                         on(Glow) ? "on" : "off", static_cast<unsigned long long>(t.glowNative), static_cast<unsigned long long>(t.glowGame),
                         static_cast<unsigned long long>(t.glowBuffer), static_cast<unsigned long long>(t.glowDropped), on(Shadows) ? "on" : "off",
                         static_cast<unsigned long long>(t.setups), static_cast<unsigned long long>(t.volumes),
                         static_cast<unsigned long long>(t.volumeTriangles), static_cast<unsigned long long>(t.volumesAborted),
                         static_cast<unsigned long long>(t.resolves), static_cast<unsigned long long>(t.darkens),
                         static_cast<unsigned long long>(t.shadowDropped), static_cast<unsigned long long>(t.gameSetups),
                         static_cast<unsigned long long>(t.gameVolumes), static_cast<unsigned long long>(t.gameResolves),
                         static_cast<unsigned long long>(t.gameDarkens), on(Flare) ? "on" : "off", static_cast<unsigned long long>(t.probes),
                         static_cast<unsigned long long>(t.probeAnswered), static_cast<unsigned long long>(t.probeUnknown),
                         static_cast<unsigned long long>(t.probeMarkers), static_cast<unsigned long long>(t.gameProbes));
            s.stats = Stats{};
        }

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            report();
            callOriginal(kFrameKick, rdram, ctx, runtime);
        }

        void wrap(PS2Runtime &runtime, uint32_t address, Function hook)
        {
            Function original = runtime.lookupFunction(address);
            if (!original)
            {
                std::fprintf(stderr, "[sc3:offscreen] no function at %#x\n", address);
                return;
            }
            g_state.originals[address] = original;
            runtime.registerFunction(address, hook);
        }
    }
    }

    bool nativeGlowOn() { return offscreen::g_state.runtime && offscreen::on(offscreen::Glow) && nativeMarkers(); }

    void installOffscreen(PS2Runtime &runtime, bool nativeGs)
    {
        using namespace offscreen;
        offscreen::State &s = offscreen::g_state;
        bool any = false;
        for (int f = 0; f < Features; ++f)
        {
            const char *value = std::getenv(kSwitchNames[f]);
            if (!value || !*value || *value == '0')
                continue;
            if (*value == '@')
                s.switches[f].path = value + 1;
            else
                s.switches[f].on = true;
            any = true;
        }
        if (!any)
            return;
        if (!nativeGs || !nativeMarkers())
        {
            std::fprintf(stderr, "[sc3:offscreen] the native off-screen passes need the native GS renderer and the native 2D: the game's packets\n");
            return;
        }
        s.runtime = &runtime;
        s.rdram = runtime.memory().getRDRAM();
        s.scratchpad = runtime.memory().getScratchpad();
        wrap(runtime, kGlowLayerEnd, glowLayerEnd);
        wrap(runtime, kGlowBuffer, glowBuffer);
        wrap(runtime, kGlowBufferBuild, glowBufferBuild);
        wrap(runtime, kGlowBufferDraw, glowBufferDraw);
        wrap(runtime, kShadowSetup, shadowSetup);
        wrap(runtime, kShadowVolumes, shadowVolumes);
        wrap(runtime, kShadowResolve, shadowResolve);
        wrap(runtime, kShadowDarken, shadowDarken);
        wrap(runtime, kStoreImage, storeImage);
        wrap(runtime, kFrameKick, frameKick);
        std::fprintf(stderr, "[sc3:offscreen] native glow %s, shadows %s, flare probe %s\n",
                     s.switches[Glow].path.empty() ? (s.switches[Glow].on ? "on" : "off") : "by its switch file",
                     s.switches[Shadows].path.empty() ? (s.switches[Shadows].on ? "on" : "off") : "by its switch file",
                     s.switches[Flare].path.empty() ? (s.switches[Flare].on ? "on" : "off") : "by its switch file");
    }
}
