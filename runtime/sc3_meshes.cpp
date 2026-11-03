#include "sc3_meshes.h"
#include "sc3_textures.h"
#include "sc3_vu1_port.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_registers.h"
#include "runtime/native/native_mesh_draw.h"
#include "runtime/native/native_mesh_pack.h"
#include "runtime/native/native_vif1.h"
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc3
{
    namespace meshes
    {
    namespace
    {
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kFileRequest = 0x00150418u;
        constexpr uint32_t kCdRead = 0x00424E28u;
        constexpr uint32_t kRawPart = 0x0010A178u;
        constexpr uint32_t kRawPartTail = 0x00109990u;
        constexpr uint32_t kChainPart = 0x0010A4E8u;
        constexpr uint32_t kPacketFlush = 0x0010F8B8u;
        constexpr uint32_t kOtInsertDepth = 0x0010D9A0u;
        constexpr uint32_t kOtInsertFixed = 0x0010DC90u;
        constexpr uint32_t kScratchpad = 0x70000000u;
        constexpr uint32_t kSector = 2048u;
        constexpr uint32_t kRawPartKind = 1u;
        constexpr uint32_t kTagRegs = 0x100u;
        constexpr uint32_t kSkipPartList = 0x0C290019u;

        constexpr uint32_t kFlagProgramA = 0x40000u, kFlagRaw = 0x20000u, kFlagShineA = 0x1000000u, kFlagShineB = 0x2000000u;
        constexpr uint32_t kFlagLights = 0x800000u, kFlagNoLights = 0x8000000u, kFlagLightRows = 0x20000000u, kFlagColourScale = 0x400000u;

        using Function = PS2Runtime::RecompiledFunction;
        using ps2x::native::GsMaterial;
        using ps2x::native::GsVertex;
        using ps2x::native::MeshBatch;
        using ps2x::native::MeshDraw;
        using ps2x::native::MeshSubmitter;
        using ps2x::native::MeshViewList;
        namespace packs = ps2x::native::packs;

        enum class Mode
        {
            Off,
            On,
            Check, // drawn natively, each part also run through the lifted program and compared
        };

        // What 0x10AD18 sends to VU TOP + 0x1AD for a part: 19 quadwords, the VU1 setups load them into vf14-vf31 and
        // vi1 / vi3 / vi12 / vi15, which later parts without one keep using.
        struct Header
        {
            uint32_t q[19][4] = {};
            bool valid = false;
            bool shine = false; // 0x10AFE8's layout, loaded by A:0x1C80
            // As the VU reads a float: no infinity or NaN (exponent 255 is the largest magnitude), no denormals.
            float f(int qword, int lane) const
            {
                uint32_t bits = q[qword][lane];
                const uint32_t exponent = (bits >> 23) & 0xFFu;
                if (exponent == 0xFFu)
                    bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
                else if (exponent == 0u)
                    bits &= 0x80000000u;
                float v;
                std::memcpy(&v, &bits, 4);
                return v;
            }
        };

        struct Material
        {
            uint64_t tex0 = 0, tex1 = 0, clamp = 0, alpha = 0x44, test = 0, miptbp1 = 0, miptbp2 = 0, tagRegs = 0;
            bool setsAlpha = false, setsTest = false;
            uint32_t address = 0, qwc = 0; // the game's block, for parts left to VU1
        };

        struct Sc3Draw : MeshDraw
        {
            uint16_t mark = 0;
        };

        struct Leaf
        {
            std::string name;
            uint64_t offset = 0;
            uint32_t size = 0;
            uint32_t base = 0;     // EE address the reads put its start at
            uint32_t covered = 0;  // bytes read there so far
            bool bound = false;
        };

        struct Stats
        {
            uint64_t parts = 0, native = 0, triangles = 0, leftToVu1 = 0, notInPack = 0, unsupported = 0, noMaterial = 0, noTexture = 0;
            uint64_t chainParts = 0, headers = 0, bound = 0, boundBad = 0, kicks = 0, drawsSubmitted = 0;
            uint64_t checked = 0, checkVertexDiffs = 0, checkCountDiffs = 0, checkExact = 0;
            std::map<uint32_t, uint64_t> routinesLeft;
            std::map<uint64_t, uint64_t> textureMisses;
            std::atomic<uint64_t> marks{0}, marksOutOfOrder{0}, marksUnknown{0};
        };

        struct State
        {
            Mode mode = Mode::Off;
            PS2Runtime *runtime = nullptr;
            uint8_t *rdram = nullptr, *scratchpad = nullptr;
            std::map<uint32_t, Function> originals;
            packs::MeshPack pack;
            packs::LoadedImages loaded;
            std::vector<Leaf> leaves; // by offset in ROOT.OLK
            int64_t olkLbn = -1;
            uint32_t requestDest = 0, requestPos = 0;
            bool requestPending = false;
            Header header;
            Material material;
            MeshViewList<Sc3Draw> building{8192};
            std::vector<size_t> unflushed; // indices into the draws since the last packet flush
            std::vector<Sc3Draw> pendingDraws;
            uint16_t nextMark = 1;
            uint64_t nextGroup = 1;
            std::mutex markMutex;
            std::unordered_map<uint16_t, uint64_t> markGroups;
            uint64_t lastGroup = 0;
            MeshSubmitter submitter;
            Stats stats;
            uint64_t frames = 0;
            VU1State checkState{};
            std::vector<uint8_t> checkMemory = std::vector<uint8_t>(16u * 1024u);
        };

        State g_state;

        // SC3_NATIVE_MESHES_ROUTINES=<a,b,...> or @file (read every 64 frames): only those routines drawn natively.
        std::string g_routinesPath, g_routinesSpec;
        std::vector<uint32_t> g_routines;
        uint64_t g_traceFrame = ~0ull;

        void parseRoutines(const std::string &spec)
        {
            g_routines.clear();
            std::string item;
            for (char c : spec + ",")
            {
                if (c == ',' || c == ' ' || c == '\n')
                {
                    if (!item.empty())
                        g_routines.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
                    item.clear();
                }
                else
                    item += c;
            }
        }

        void followRoutines(uint64_t frame)
        {
            if (g_routinesPath.empty() || (frame & 63u) != 0u)
                return;
            std::ifstream file(g_routinesPath);
            std::string spec;
            std::getline(file, spec);
            if (spec == g_routinesSpec)
                return;
            g_routinesSpec = spec;
            parseRoutines(spec);
            std::fprintf(stderr, "[sc3:mesh] frame %llu: native routines '%s'\n", static_cast<unsigned long long>(frame), spec.c_str());
        }

        bool routineAllowed(uint32_t routine)
        {
            return g_routines.empty() && g_routinesPath.empty() ? true : std::find(g_routines.begin(), g_routines.end(), routine) != g_routines.end();
        }

        uint32_t peek32(uint32_t address)
        {
            uint32_t value = 0;
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(&value, g_state.scratchpad + (address & 0x3FFCu), 4);
            else
                std::memcpy(&value, g_state.rdram + (address & PS2_RAM_MASK & ~3u), 4);
            return value;
        }

        void poke32(uint32_t address, uint32_t value)
        {
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(g_state.scratchpad + (address & 0x3FFCu), &value, 4);
            else
                std::memcpy(g_state.rdram + (address & PS2_RAM_MASK & ~3u), &value, 4);
        }

        uint32_t sp32(uint32_t offset) { return peek32(kScratchpad + offset); }
        void spSet(uint32_t offset, uint32_t value) { poke32(kScratchpad + offset, value); }

        void setReg(R5900Context *ctx, int reg, uint32_t value)
        {
            ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        }

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_state.originals.at(address)(rdram, ctx, runtime);
        }

        // ---- where the game loaded ROOT.OLK's leaves (bound before the game relocates them in place)

        Leaf *leafAt(uint64_t olk)
        {
            std::vector<Leaf> &leaves = g_state.leaves;
            auto it = std::upper_bound(leaves.begin(), leaves.end(), olk, [](uint64_t o, const Leaf &l) { return o < l.offset; });
            if (it == leaves.begin())
                return nullptr;
            --it;
            return olk < it->offset + it->size ? &*it : nullptr;
        }

        void noteRead(uint32_t buffer, uint64_t olk, uint32_t bytes)
        {
            State &s = g_state;
            for (uint64_t at = olk; at < olk + bytes;)
            {
                Leaf *leaf = leafAt(at);
                if (!leaf)
                {
                    at = (at / kSector + 1u) * kSector;
                    continue;
                }
                const uint32_t base = buffer - static_cast<uint32_t>(olk - leaf->offset);
                const uint64_t end = std::min<uint64_t>(olk + bytes, leaf->offset + leaf->size);
                if (leaf->base != base || leaf->bound)
                {
                    leaf->base = base;
                    leaf->covered = 0;
                    leaf->bound = false;
                }
                leaf->covered += static_cast<uint32_t>(end - at);
                if (leaf->covered >= leaf->size)
                {
                    const packs::LoadedImages::Bound bound = s.loaded.bind(s.pack, s.rdram, leaf->name, base, leaf->size, false);
                    leaf->bound = true;
                    if (bound.image.file)
                        ++s.stats.bound;
                    else
                    {
                        ++s.stats.boundBad;
                        std::fprintf(stderr, "[sc3:mesh] leaf %s at %#x: not the image the pack was made from (crc %#x)\n", leaf->name.c_str(),
                                     base, bound.crc);
                    }
                }
                at = end;
            }
        }

        void fileRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            if (getRegU32(ctx, 5) == 1u && s.olkLbn < 0)
            {
                s.requestDest = getRegU32(ctx, 8);
                s.requestPos = getRegU32(ctx, 6);
                s.requestPending = true;
            }
            callOriginal(kFileRequest, rdram, ctx, runtime);
        }

        void cdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t lbn = getRegU32(ctx, 4), sectors = getRegU32(ctx, 5), buffer = getRegU32(ctx, 6) & PS2_RAM_MASK;
            if (s.olkLbn < 0 && s.requestPending && buffer == (s.requestDest & PS2_RAM_MASK) && (s.requestPos % kSector) == 0u)
                s.olkLbn = static_cast<int64_t>(lbn) - s.requestPos / kSector;
            callOriginal(kCdRead, rdram, ctx, runtime);
            if (s.olkLbn >= 0 && lbn >= s.olkLbn && sectors != 0u)
                noteRead(buffer, static_cast<uint64_t>(lbn - s.olkLbn) * kSector, sectors * kSector);
        }

        // ---- 0x10AD18 in C++: the header a part's setup loads, from the renderer's state in the scratchpad

        void copyQwords(Header &h, int qword, uint32_t scratch, int count)
        {
            for (int i = 0; i < count; ++i)
                for (int l = 0; l < 4; ++l)
                    h.q[qword + i][l] = sp32(scratch + 16u * static_cast<uint32_t>(i) + 4u * static_cast<uint32_t>(l));
        }

        void setFloat(Header &h, int qword, int lane, float v) { std::memcpy(&h.q[qword][lane], &v, 4); }
        float spf(uint32_t offset)
        {
            const uint32_t bits = sp32(offset);
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
        }

        // The common setup (0x10ADF8, then 0x10B2F8, 0x10AF28, 0x10AFA8); false for the shine setups (0x10AFE8, 0x10B160).
        bool buildHeader(uint32_t flags, uint32_t jump, Header &h)
        {
            if ((flags & kFlagShineA) && (flags & kFlagProgramA))
            {
                Header out = h;
                copyQwords(out, 0, 0x100u, 4);
                copyQwords(out, 4, 0x2C0u, 3);
                copyQwords(out, 7, 0x180u, 3);
                copyQwords(out, 10, 0x270u, 1);
                copyQwords(out, 11, 0x2F0u, 1);
                out.q[14][0] = 0x8000u;
                out.q[14][1] = sp32(0x36Cu) != 0u ? 0x19Fu : 0u;
                out.q[14][2] = jump;
                out.q[14][3] = sp32(0x1ACu);
                const float u = spf(0x2A0u), v = spf(0x2A4u);
                setFloat(out, 12, 0, u * -0.5f);
                setFloat(out, 12, 1, v * -0.5f);
                out.q[12][2] = 0u;
                setFloat(out, 13, 0, -(spf(0x300u) + 0.5f) * u);
                setFloat(out, 13, 1, -(spf(0x304u) + 0.5f) * v);
                setFloat(out, 13, 2, 1.0f);
                out.valid = true;
                out.shine = true;
                h = out;
                return true;
            }
            if (flags & (kFlagShineA | kFlagShineB))
                return false;
            Header out = h;
            copyQwords(out, 0, (flags & kFlagProgramA) ? 0x100u : 0x140u, 4);
            copyQwords(out, 12, 0x260u, 1);
            copyQwords(out, 13, 0x180u, 1);
            copyQwords(out, 14, 0x190u, 1);
            copyQwords(out, 15, 0x1B0u, 1);
            copyQwords(out, 16, 0x1C0u, 1);
            out.q[17][0] = 0x8000u;
            out.q[17][1] = sp32(0x36Cu) != 0u ? 0x19Fu : 0u;
            out.q[17][2] = jump;
            out.q[17][3] = sp32(0x1ACu);
            copyQwords(out, 18, 0x2A0u, 1);
            if (flags & kFlagColourScale)
            {
                const float scale = [] { const uint32_t b = 0x3B808080u; float v; std::memcpy(&v, &b, 4); return v; }();
                setFloat(out, 14, 3, out.f(14, 3) * scale);
                setFloat(out, 15, 0, out.f(15, 0) * scale);
            }
            const bool lights = (flags & kFlagLights) != 0u;
            if (lights)
            {
                setFloat(out, 4, 0, 0.0f);
                setFloat(out, 4, 1, 0.0f);
                if (!(flags & kFlagNoLights))
                {
                    copyQwords(out, 4, 0x1E0u, 4);
                    copyQwords(out, 8, 0x220u, 4);
                    if (flags & kFlagLightRows)
                    {
                        const uint32_t row[4] = {sp32(0x290u), sp32(0x294u), sp32(0x298u), sp32(0x29Cu)};
                        for (int i = 0; i < 4; ++i)
                            out.q[4 + i][3] = row[i];
                        copyQwords(out, 11, 0x280u, 1);
                    }
                }
            }
            else
            {
                setFloat(out, 4, 0, spf(0x2B0u) * spf(0x2A0u));
                setFloat(out, 4, 1, spf(0x2B4u) * spf(0x2A4u));
            }
            out.valid = true;
            out.shine = false;
            h = out;
            return true;
        }

        // ---- the A+D material block a part REFs to TOP + 0x19F, from the pack

        bool readMaterial(uint32_t address, Material &m)
        {
            packs::LoadedImages::Image image;
            if (!g_state.loaded.find(address & PS2_RAM_MASK, image) || !image.file)
                return false;
            const packs::RegisterBlock *block = image.file->block((address & PS2_RAM_MASK) - image.base);
            if (!block)
                return false;
            for (const auto &[reg, value] : block->writes)
            {
                switch (reg)
                {
                case 0x06u: m.tex0 = value; break;
                case 0x08u: m.clamp = value; break;
                case 0x14u: m.tex1 = value; break;
                case 0x34u: m.miptbp1 = value; break;
                case 0x36u: m.miptbp2 = value; break;
                case 0x42u: m.alpha = value, m.setsAlpha = true; break;
                case 0x47u: m.test = value, m.setsTest = true; break;
                case kTagRegs: m.tagRegs = value; break;
                default: break;
                }
            }
            return true;
        }

        // ---- the A program's rigid routines in plain C++ (their microcode's arithmetic, IEEE floats)

        struct In
        {
            float st[2], colour[4], position[3], normal[3];
            uint32_t flags;
        };

        // The VU's FMAC saturates at the largest float instead of going to infinity (and never makes a NaN).
        float sat(double v)
        {
            constexpr double kMax = 3.4028234663852886e38;
            return static_cast<float>(v > kMax ? kMax : v < -kMax ? -kMax : v);
        }

        int32_t ftoi(float v, float scale)
        {
            const double x = static_cast<double>(v) * scale;
            if (!(x == x))
                return 0;
            if (x >= 2147483647.0)
                return 2147483647;
            if (x <= -2147483648.0)
                return INT32_MIN;
            return static_cast<int32_t>(x);
        }

        // What a rigid routine of program A does to a vertex: lit by the header's lights (or its colour times vf19), fogged
        // (F from the clamped fog term, else the vertex's own), its texture coordinates scrolled by vf24.xy.
        struct Shade
        {
            bool lit = false, fog = false, scroll = false, environment = false;
        };

        bool routineShade(uint32_t routine, Shade &shade)
        {
            switch (routine)
            {
            case 0xD8u: shade = {true, true, false}; return true;
            case 0x2B0u: shade = {true, false, false}; return true;
            case 0xC18u: shade = {false, false, false}; return true;
            case 0x1558u: shade = {false, true, true}; return true;
            case 0x1730u: shade = {false, false, true}; return true;
            case 0xA48u: shade = {false, true, false}; return true;
            case 0x1D28u: shade = {false, true, false, true}; return true;
            default: return false;
            }
        }

        struct Out
        {
            float s, t, q;
            int32_t rgba[4];
            int32_t xyz[3];
            uint32_t w; // XYZF2's last word: bit 15 ADC, bits 4-11 F
        };

        // A:0x1D28 after A:0x1C80 (0x10AFE8's header): texture coordinates from the normal plus the position times
        // vf20 through vf25-27 (an environment map), one colour for all (vf21's words), fogged.
        void shadeEnvironment(const Header &h, const std::vector<In> &in, std::vector<Out> &out)
        {
            out.resize(in.size());
            bool outside[3] = {false, false, false};
            for (size_t k = 0; k < in.size(); ++k)
            {
                const In &v = in[k];
                float p[4], e[4], r[4];
                for (int c = 0; c < 4; ++c)
                    p[c] = h.f(0, c) * v.position[0] + h.f(1, c) * v.position[1] + h.f(2, c) * v.position[2] + h.f(3, c);
                const float q = 1.0f / p[3];
                const float pos[4] = {v.position[0], v.position[1], v.position[2], 0.0f};
                const float nrm[4] = {v.normal[0], v.normal[1], v.normal[2], 0.0f};
                for (int c = 0; c < 4; ++c)
                    e[c] = nrm[c] + pos[c] * h.f(11, c);
                for (int c = 0; c < 4; ++c)
                    r[c] = h.f(4, c) * e[0] + h.f(5, c) * e[1] + h.f(6, c) * e[2];
                const float s0 = h.f(13, 0) + r[0] * h.f(12, 0), t0 = h.f(13, 1) + r[1] * h.f(12, 1);
                const float position[4] = {p[0] * q, p[1] * q, p[2] * q, p[3]};
                float g[4];
                for (int i = 0; i < 4; ++i)
                    g[i] = sat(static_cast<double>(sat(static_cast<double>(h.f(7, i)) * h.f(8, i))) +
                               static_cast<double>(sat(static_cast<double>(position[i]) * h.f(8, i))));
                outside[k % 3] = std::fabs(g[0]) > 1.0f || std::fabs(g[1]) > 1.0f || std::fabs(g[2]) > 1.0f;
                const bool clipped = outside[0] || outside[1] || outside[2];
                Out &o = out[k];
                o.s = s0 * q;
                o.t = t0 * q;
                o.q = q;
                for (int i = 0; i < 4; ++i)
                    o.rgba[i] = static_cast<int32_t>(h.q[10][i]);
                for (int i = 0; i < 3; ++i)
                    o.xyz[i] = ftoi(position[i], 16.0f);
                const float fog = std::max(std::min(g[3], h.f(9, 0)), h.f(9, 1));
                o.w = ((((clipped ? 0x8000u : 0u) | v.flags) & 0x8000u) + (static_cast<uint32_t>(ftoi(fog, 16.0f)) & 0xFFFFu)) & 0xFFFFu;
            }
        }

        void shade(const Header &h, Shade mode, const std::vector<In> &in, std::vector<Out> &out)
        {
            if (mode.environment)
            {
                shadeEnvironment(h, in, out);
                return;
            }
            out.resize(in.size());
            bool outside[3] = {false, false, false};
            for (size_t k = 0; k < in.size(); ++k)
            {
                const In &v = in[k];
                float p[4];
                for (int c = 0; c < 4; ++c)
                    p[c] = h.f(0, c) * v.position[0] + h.f(1, c) * v.position[1] + h.f(2, c) * v.position[2] + h.f(3, c);
                const float q = 1.0f / p[3];
                const float xs = p[0] * q, ys = p[1] * q, zs = p[2] * q;
                const float position[4] = {xs, ys, zs, p[3]};
                float g[4];
                for (int i = 0; i < 4; ++i)
                    g[i] = sat(static_cast<double>(sat(static_cast<double>(h.f(13, i)) * h.f(14, i))) +
                               static_cast<double>(sat(static_cast<double>(position[i]) * h.f(14, i))));
                outside[k % 3] = std::fabs(g[0]) > 1.0f || std::fabs(g[1]) > 1.0f || std::fabs(g[2]) > 1.0f;
                const bool clipped = outside[0] || outside[1] || outside[2];
                Out &o = out[k];
                float c[4];
                if (!mode.lit)
                {
                    for (int i = 0; i < 4; ++i)
                        c[i] = v.colour[i] * h.f(12, i);
                }
                else
                {
                    float l[4];
                    for (int i = 0; i < 4; ++i)
                        l[i] = std::max(h.f(4, i) * v.normal[0] + h.f(5, i) * v.normal[1] + h.f(6, i) * v.normal[2], 0.0f);
                    for (int i = 0; i < 4; ++i)
                    {
                        const float lit = h.f(12, i) + h.f(8, i) * l[0] + h.f(9, i) * l[1] + h.f(10, i) * l[2] + h.f(11, i) * l[3];
                        c[i] = std::min(lit * v.colour[i], h.f(16, 0));
                    }
                }
                for (int i = 0; i < 4; ++i)
                    o.rgba[i] = ftoi(c[i], 1.0f);
                const float s0 = mode.scroll ? h.f(4, 0) + v.st[0] * h.f(18, 0) : v.st[0] * h.f(18, 0);
                const float t0 = mode.scroll ? h.f(4, 1) + v.st[1] * h.f(18, 1) : v.st[1] * h.f(18, 1);
                o.s = s0 * q;
                o.t = t0 * q;
                o.q = q;
                o.xyz[0] = ftoi(xs, 16.0f);
                o.xyz[1] = ftoi(ys, 16.0f);
                o.xyz[2] = ftoi(zs, 16.0f);
                const uint32_t clipBit = clipped ? 0x8000u : 0u;
                if (mode.fog)
                {
                    const float fog = std::max(std::min(g[3], h.f(15, 0)), h.f(15, 1));
                    o.w = (((clipBit | v.flags) & 0x8000u) + (static_cast<uint32_t>(ftoi(fog, 16.0f)) & 0xFFFFu)) & 0xFFFFu;
                }
                else
                    o.w = (clipBit | v.flags) & 0xFFFFu;
            }
        }

        bool readVertices(const packs::MeshObject &object, std::vector<In> &in, uint32_t tag[4])
        {
            const packs::MeshSection *section = object.section(packs::fourcc("SC3V"));
            if (!section || section->size < 24u)
                return false;
            uint32_t head[6];
            std::memcpy(head, section->data, sizeof(head));
            const uint32_t count = head[0];
            if (section->size < 24u + count * 64u)
                return false;
            std::memcpy(tag, head + 2, 16);
            in.resize(count);
            const uint8_t *at = section->data + 24u;
            for (uint32_t k = 0; k < count; ++k, at += 64u)
            {
                float w[16];
                std::memcpy(w, at, 64u);
                In &v = in[k];
                v.st[0] = w[0], v.st[1] = w[1];
                std::memcpy(v.colour, w + 4, 16);
                std::memcpy(v.position, w + 8, 12);
                uint32_t flags;
                std::memcpy(&flags, at + 44, 4);
                v.flags = flags;
                std::memcpy(v.normal, w + 12, 12);
            }
            return true;
        }

        void toTriangles(uint32_t prim, const std::vector<Out> &out, std::vector<GsVertex> &tris)
        {
            tris.clear();
            const uint32_t type = prim & 7u;
            GsVertex queue[3];
            uint32_t queued = 0;
            for (const Out &o : out)
            {
                GsVertex &v = queue[queued];
                v.s = o.s, v.t = o.t, v.q = o.q == 0.0f ? 1.0f : o.q;
                v.rgba = (static_cast<uint32_t>(o.rgba[0]) & 0xFFu) | ((static_cast<uint32_t>(o.rgba[1]) & 0xFFu) << 8) |
                         ((static_cast<uint32_t>(o.rgba[2]) & 0xFFu) << 16) | ((static_cast<uint32_t>(o.rgba[3]) & 0xFFu) << 24);
                v.x = static_cast<float>(static_cast<uint32_t>(o.xyz[0]) & 0xFFFFu) / 16.0f;
                v.y = static_cast<float>(static_cast<uint32_t>(o.xyz[1]) & 0xFFFFu) / 16.0f;
                v.z = (static_cast<uint32_t>(o.xyz[2]) >> 4) & 0xFFFFFFu;
                v.fog = static_cast<uint8_t>((o.w >> 4) & 0xFFu);
                if (++queued < 3u)
                    continue;
                if ((o.w & 0x8000u) == 0u)
                    tris.insert(tris.end(), queue, queue + 3);
                if (type == 3u)
                    queued = 0;
                else
                {
                    if (type == 4u)
                        queue[0] = queue[1];
                    queue[1] = queue[2];
                    queued = 2;
                }
            }
        }

        // ---- check mode: the lifted program on a VU memory image of the same inputs, its packet against the C++

        struct CheckKick
        {
            std::vector<std::vector<uint8_t>> packets;
        };

        // The setup always runs (A:0x10 with the header the C++ used, jumping to the part's routine): the lifted program's
        // registers then hold what the C++ read, whichever part last sent a header.
        void checkPart(const Header &h, uint32_t routine, const uint32_t tag[4], const std::vector<In> &in, const std::vector<Out> &out)
        {
            State &s = g_state;
            uint8_t *vu = s.checkMemory.data();
            auto put = [vu](uint32_t qword, const void *words) { std::memcpy(vu + (qword & 0x3FFu) * 16u, words, 16u); };
            const uint32_t pc = h.shine ? 0x1C80u : 0x10u;
            const int control = h.shine ? 14 : 17;
            Header setup = h;
            setup.q[control][1] = 0u;
            setup.q[control][2] = routine >> 3;
            for (int i = 0; i < control + 2 && i < 19; ++i)
                put(0x1ADu + static_cast<uint32_t>(i), setup.q[i]);
            put(0u, tag);
            for (size_t k = 0; k < in.size(); ++k)
            {
                float w[16] = {in[k].st[0], in[k].st[1], 0, 0, in[k].colour[0], in[k].colour[1], in[k].colour[2], in[k].colour[3],
                               in[k].position[0], in[k].position[1], in[k].position[2], 0, in[k].normal[0], in[k].normal[1], in[k].normal[2], 0};
                std::memcpy(&w[11], &in[k].flags, 4);
                for (int qd = 0; qd < 4; ++qd)
                    put(1u + 4u * static_cast<uint32_t>(k) + static_cast<uint32_t>(qd), w + 4 * qd);
            }
            CheckKick kick;
            runVu1Program(kProgramA, pc, s.checkState, vu, 0u, 0u,
                          [](void *user, const uint8_t *packet, uint32_t bytes)
                          { static_cast<CheckKick *>(user)->packets.emplace_back(packet, packet + bytes); },
                          &kick);
            ++s.stats.checked;
            const std::vector<uint8_t> *vertices = nullptr;
            for (const std::vector<uint8_t> &p : kick.packets)
                if (p.size() >= 16u + 48u * in.size() && p.size() >= 16u)
                {
                    uint64_t hi;
                    std::memcpy(&hi, p.data() + 8, 8);
                    if ((hi & 0xFFFu) == 0x412u)
                        vertices = &p;
                }
            if (!vertices)
            {
                ++s.stats.checkCountDiffs;
                return;
            }
            bool exact = true, close = true;
            for (size_t k = 0; k < out.size(); ++k)
            {
                float stq[4];
                int32_t rgba[4], xyzw[4];
                std::memcpy(stq, vertices->data() + 16u + 48u * k, 16);
                std::memcpy(rgba, vertices->data() + 32u + 48u * k, 16);
                std::memcpy(xyzw, vertices->data() + 48u + 48u * k, 16);
                const Out &o = out[k];
                const bool sameBits = std::memcmp(stq, &o.s, 12) == 0 && std::memcmp(rgba, o.rgba, 16) == 0 && std::memcmp(xyzw, o.xyz, 12) == 0 &&
                                      (static_cast<uint32_t>(xyzw[3]) & 0xFFFFu) == o.w;
                exact = exact && sameBits;
                if (!sameBits)
                {
                    bool near = (static_cast<uint32_t>(xyzw[3]) & 0x8000u) == (o.w & 0x8000u);
                    for (int i = 0; i < 3; ++i)
                        near = near && std::abs(xyzw[i] - o.xyz[i]) <= 1 && std::fabs(stq[i] - (&o.s)[i]) <= 1e-4f * std::max(1.0f, std::fabs(stq[i]));
                    for (int i = 0; i < 4; ++i)
                        near = near && std::abs((rgba[i] & 0xFF) - (o.rgba[i] & 0xFF)) <= 1;
                    near = near && std::abs(static_cast<int>((xyzw[3] >> 4) & 0xFF) - static_cast<int>((o.w >> 4) & 0xFF)) <= 1;
                    static std::map<uint32_t, int> s_printed;
                    const bool print = !near && s_printed[routine] < 4;
                    if (print)
                        ++s_printed[routine];
                    if (print && s_printed[routine] == 1)
                        std::fprintf(stderr, "[sc3:mesh] check header q13 %g %g %g %g q14 %g %g %g %g q15 %g %g %g %g q16 %g; position w %g\n", h.f(13, 0),
                                     h.f(13, 1), h.f(13, 2), h.f(13, 3), h.f(14, 0), h.f(14, 1), h.f(14, 2), h.f(14, 3), h.f(15, 0), h.f(15, 1),
                                     h.f(15, 2), h.f(15, 3), h.f(16, 0), 1.0f / o.q);
                    if (print)
                        std::fprintf(stderr,
                                     "[sc3:mesh] check %#x vertex %zu/%zu: lifted stq %g %g %g rgba %d %d %d %d xyzw %d %d %d %#x; C++ stq %g %g %g "
                                     "rgba %d %d %d %d xyzw %d %d %d %#x\n",
                                     routine, k, out.size(), stq[0], stq[1], stq[2], rgba[0], rgba[1], rgba[2], rgba[3], xyzw[0], xyzw[1], xyzw[2],
                                     static_cast<uint32_t>(xyzw[3]) & 0xFFFFu, o.s, o.t, o.q, o.rgba[0], o.rgba[1], o.rgba[2], o.rgba[3], o.xyz[0],
                                     o.xyz[1], o.xyz[2], o.w);
                    close = close && near;
                }
            }
            if (exact)
                ++s.stats.checkExact;
            else if (!close)
                ++s.stats.checkVertexDiffs;
        }

        // ---- the part hooks

        uint32_t setupOf(uint32_t flags)
        {
            if (flags & kFlagProgramA)
                return (flags & kFlagShineA) ? 0x1C80u : (flags & kFlagShineB) ? 0x2098u : 0x10u;
            return (flags & kFlagShineA) ? 0x1990u : (flags & kFlagShineB) ? 0x21E0u : 0x10u;
        }

        // A part left to VU1 after native ones: its header and material as the game's state has them, since the native
        // parts before it sent theirs to no one.
        void selfContained(uint32_t flags)
        {
            State &s = g_state;
            if (sp32(0x368u) == 0u)
                spSet(0x368u, flags);
            if (sp32(0x36Cu) == 0u && s.material.address != 0u)
            {
                spSet(0x36Cu, s.material.address);
                spSet(0x370u, s.material.qwc);
            }
        }

        void noteState(uint32_t flags, uint32_t jump)
        {
            State &s = g_state;
            if (sp32(0x368u) != 0u)
            {
                ++s.stats.headers;
                if (!buildHeader(flags, jump, s.header))
                    s.header.valid = false;
            }
            if (const uint32_t block = sp32(0x36Cu))
            {
                Material m;
                m.address = block;
                m.qwc = sp32(0x370u);
                if (readMaterial(block, m))
                    s.material = m;
                else
                    s.material = Material{0, 0, 0, 0x44, 0, 0, 0, 0, false, false, block, m.qwc};
            }
        }

        void packetFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

        // The placeholder the part leaves in the game's VIF1 list: a MARK where its draws belong (the GS stream's place
        // for them while the rest of the frame still goes through the GS front end), and the microprogram upload the
        // game asked for (parts left to VU1 after it need it).
        void writePlaceholder(R5900Context *ctx, PS2Runtime *runtime, uint16_t mark)
        {
            State &s = g_state;
            const bool program = sp32(0x384u) != 0u;
            const uint32_t qwords = program ? 2u : 1u;
            uint32_t cursor = sp32(0x378u);
            if (sp32(0x374u) + sp32(0x18u) - 0x10u < cursor + qwords * 16u)
            {
                const uint32_t ra = getRegU32(ctx, 31);
                setReg(ctx, 4, cursor);
                packetFlush(s.rdram, ctx, runtime);
                setReg(ctx, 31, ra);
                cursor = sp32(0x0u);
                spSet(0x378u, cursor);
                spSet(0x374u, cursor);
            }
            if (program)
            {
                poke32(cursor, 0x30000000u | sp32(0x388u));
                poke32(cursor + 4u, sp32(0x384u));
                poke32(cursor + 8u, 0u);
                poke32(cursor + 12u, 0u);
                cursor += 16u;
            }
            poke32(cursor, 0x10000000u);
            poke32(cursor + 4u, 0u);
            poke32(cursor + 8u, 0x07000000u | mark);
            poke32(cursor + 12u, 0u);
            cursor += 16u;
            spSet(0x378u, cursor);
            spSet(0x384u, 0u);
            spSet(0x368u, 0u);
            spSet(0x350u, 0u);
            spSet(0x36Cu, 0u);
        }

        bool drawRawPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t address = getRegU32(ctx, 4) & PS2_RAM_MASK, qwc = getRegU32(ctx, 5), flags = getRegU32(ctx, 6);
            const uint32_t routine = flags & 0xFFFFu;
            ++s.stats.parts;
            Shade mode;
            const bool header = sp32(0x368u) != 0u;
            noteState(flags, routine >> 3);
            if (s.frames == g_traceFrame)
                std::fprintf(stderr, "[sc3:mesh] trace part %#x qwc %u flags %#x ra %#x 368 %#x 36c %#x 384 %#x/%#x 350 %#x\n", address, qwc, flags,
                             getRegU32(ctx, 31), sp32(0x368u), sp32(0x36Cu), sp32(0x384u), sp32(0x388u), sp32(0x350u));
            if (!(flags & kFlagProgramA) || !routineShade(routine, mode) || !routineAllowed(routine) || (header && setupOf(flags) != (mode.environment ? 0x1C80u : 0x10u)) || sp32(0x350u) != 0u || !s.header.valid ||
                s.header.shine != mode.environment)
            {
                ++s.stats.unsupported;
                ++s.stats.routinesLeft[((flags & kFlagProgramA) ? 0u : 0x10000u) | routine];
                return false;
            }
            packs::LoadedImages::Image image;
            const packs::MeshObject *object = nullptr;
            if (s.loaded.find(address, image) && image.file)
                object = image.file->object(address - image.base);
            if (!object || object->kind != kRawPartKind || object->attribute<uint32_t>(0) != qwc * 16u)
            {
                ++s.stats.notInPack;
                return false;
            }
            if (s.material.tex0 == 0u)
            {
                ++s.stats.noMaterial;
                return false;
            }
            std::vector<In> in;
            uint32_t tag[4];
            if (!readVertices(*object, in, tag))
            {
                ++s.stats.notInPack;
                return false;
            }
            const uint32_t prim = (tag[1] >> 15) & 0x7FFu;
            GsMaterial material;
            material.prim = ps2x::gs::decodePrim(prim);
            material.tex0 = ps2x::gs::decodeTex0(s.material.tex0);
            material.tex1 = s.material.tex1;
            material.clamp = s.material.clamp;
            material.miptbp1 = s.material.miptbp1;
            material.miptbp2 = s.material.miptbp2;
            if (material.prim.tme)
            {
                const auto used = [](uint32_t size, uint32_t log2)
                {
                    const uint32_t whole = 1u << std::min<uint32_t>(log2, 10u);
                    return size != 0u && size < whole ? size : whole;
                };
                const uint32_t width = used(static_cast<uint32_t>(s.material.tagRegs >> 32) & 0xFFFFu, material.tex0.tw);
                const uint32_t height = used(static_cast<uint32_t>(s.material.tagRegs >> 48) & 0xFFFFu, material.tex0.th);
                material.texture = packTextureAtHook(s.material.tex0, width, height);
                if (!material.texture)
                {
                    ++s.stats.noTexture;
                    ++s.stats.textureMisses[s.material.tex0 & ((0x3FFFull) | (0x3Full << 20) | (0x3FFFull << 37))];
                    return false;
                }
            }
            std::vector<Out> out;
            shade(s.header, mode, in, out);
            if (s.mode == Mode::Check)
                checkPart(s.header, routine, tag, in, out);
            Sc3Draw draw;
            draw.screenSpace = true;
            draw.file = image.index;
            draw.offset = address - image.base;
            draw.object = address;
            MeshBatch batch;
            batch.gs = true;
            batch.gsMaterial = material;
            batch.setsAlpha = s.material.setsAlpha;
            batch.alpha = s.material.alpha;
            batch.setsTest = s.material.setsTest;
            batch.test = s.material.test;
            toTriangles(prim, out, batch.gsTriangles);
            s.stats.triangles += batch.gsTriangles.size() / 3u;
            draw.batches.push_back(std::move(batch));
            draw.mark = s.nextMark;
            s.nextMark = static_cast<uint16_t>(s.nextMark == 0xFFFFu ? 1u : s.nextMark + 1u);
            writePlaceholder(ctx, runtime, draw.mark);
            s.unflushed.push_back(s.pendingDraws.size());
            s.pendingDraws.push_back(std::move(draw));
            ++s.stats.native;
            return true;
        }

        void rawPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (drawRawPart(rdram, ctx, runtime))
            {
                ctx->pc = ra;
                return;
            }
            ++g_state.stats.leftToVu1;
            selfContained(getRegU32(ctx, 6));
            callOriginal(kRawPart, rdram, ctx, runtime);
        }

        // 0x109990 ends in `j 0x10A178`, a direct call that bypasses the table: the same choice made here.
        void rawPartTail(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t flags = sp32(0x1BFCu);
            if (flags == kSkipPartList || !(flags & kFlagRaw))
            {
                callOriginal(kRawPartTail, rdram, ctx, runtime);
                return;
            }
            const uint32_t command = getRegU32(ctx, 4);
            const uint32_t ra = getRegU32(ctx, 31);
            setReg(ctx, 4, peek32(command + 0x14u));
            setReg(ctx, 5, peek32(command) >> 16);
            setReg(ctx, 6, flags);
            rawPart(rdram, ctx, runtime);
            ctx->pc = ra;
        }

        void chainPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t flags = getRegU32(ctx, 5);
            ++s.stats.chainParts;
            if (s.frames == g_traceFrame)
                std::fprintf(stderr, "[sc3:mesh] trace chain %#x flags %#x 368 %#x 36c %#x 384 %#x/%#x 350 %#x\n", getRegU32(ctx, 4), flags,
                             sp32(0x368u), sp32(0x36Cu), sp32(0x384u), sp32(0x388u), sp32(0x350u));
            selfContained(flags);
            noteState(flags, (flags & 0xFFFFu) >> 3);
            callOriginal(kChainPart, rdram, ctx, runtime);
        }

        // Where the scratchpad packet the parts went into is linked: the VIF1 list in order (opaque), or an ordering
        // table bucket (its depth, as 0x10D9A0 computes it; 0x10DC90's fixed bucket).
        void packetFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            for (size_t i : s.unflushed)
                s.building.opaque.push_back(std::move(s.pendingDraws[i]));
            s.unflushed.clear();
            s.pendingDraws.clear();
            callOriginal(kPacketFlush, rdram, ctx, runtime);
        }

        void otInsert(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address, int bucket)
        {
            State &s = g_state;
            for (size_t i : s.unflushed)
            {
                s.building.pending = true;
                s.building.pendingAlpha = std::move(s.pendingDraws[i]);
                s.building.link(bucket);
            }
            s.unflushed.clear();
            s.pendingDraws.clear();
            callOriginal(address, rdram, ctx, runtime);
        }

        void otInsertDepth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float z = ctx->f[12];
            float scale;
            const uint32_t bits = peek32(0x005BE700u + 0x34u);
            std::memcpy(&scale, &bits, 4);
            const float clamped = std::min(z * 0.12f, 1.0f);
            const int bucket = (z < 0.0f || z > 12.5f) ? 0 : static_cast<int>(clamped * scale);
            otInsert(rdram, ctx, runtime, kOtInsertDepth, 8191 - std::clamp(bucket, 0, 8191));
        }

        void otInsertFixed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            otInsert(rdram, ctx, runtime, kOtInsertFixed, 0);
        }

        // The game's frame kick: the views built during the frame before are what it sends now, submitted in its order,
        // each draw a group of the GS stream its MARK places.
        void report();

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            for (size_t i : s.unflushed)
                s.building.opaque.push_back(std::move(s.pendingDraws[i]));
            s.unflushed.clear();
            s.pendingDraws.clear();
            std::vector<std::pair<uint16_t, uint64_t>> assigned;
            s.building.forEachInOrder([&](const Sc3Draw &d) { assigned.emplace_back(d.mark, s.nextGroup++); });
            size_t n = 0;
            std::vector<Sc3Draw *> order;
            for (Sc3Draw &d : s.building.opaque)
                order.push_back(&d);
            for (size_t b = s.building.buckets.size(); b-- > 0;)
                for (size_t i = s.building.buckets[b].size(); i-- > 0;)
                    order.push_back(&s.building.buckets[b][i]);
            for (Sc3Draw *d : order)
                d->group = assigned[n++].second;
            {
                std::lock_guard<std::mutex> lock(s.markMutex);
                for (const auto &[mark, group] : assigned)
                    s.markGroups[mark] = group;
            }
            s.submitter.submitInOrder(s.building);
            s.stats.drawsSubmitted += s.building.size();
            ++s.stats.kicks;
            s.building.clear();
            report();
            callOriginal(kFrameKick, rdram, ctx, runtime);
        }

        uint64_t markGate(uint16_t mark)
        {
            State &s = g_state;
            std::lock_guard<std::mutex> lock(s.markMutex);
            auto it = s.markGroups.find(mark);
            if (it == s.markGroups.end() || it->second == 0u)
            {
                s.stats.marksUnknown.fetch_add(1u, std::memory_order_relaxed);
                return 0u;
            }
            const uint64_t group = it->second;
            s.markGroups.erase(it);
            if (group != s.lastGroup + 1u && s.lastGroup != 0u)
                s.stats.marksOutOfOrder.fetch_add(1u, std::memory_order_relaxed);
            s.lastGroup = group;
            s.stats.marks.fetch_add(1u, std::memory_order_relaxed);
            return group;
        }

        void report()
        {
            State &s = g_state;
            followRoutines(s.frames + 1u);
            if (s.frames + 1u == g_traceFrame)
                std::fprintf(stderr, "[sc3:mesh] trace frame %llu kicked\n", static_cast<unsigned long long>(s.frames + 1u));
            if (++s.frames % 250u != 0u)
                return;
            Stats &t = s.stats;
            std::string left;
            for (const auto &[routine, calls] : t.routinesLeft)
            {
                char item[48];
                std::snprintf(item, sizeof(item), " %c:%#x x%llu", (routine & 0x10000u) ? 'B' : 'A', routine & 0xFFFFu,
                              static_cast<unsigned long long>(calls));
                left += item;
            }
            std::string misses;
            for (const auto &[tex0, calls] : t.textureMisses)
            {
                char item[80];
                std::snprintf(item, sizeof(item), " tbp %#x psm %#x cbp %#x x%llu", static_cast<unsigned>(tex0 & 0x3FFFu),
                              static_cast<unsigned>((tex0 >> 20) & 0x3Fu), static_cast<unsigned>((tex0 >> 37) & 0x3FFFu),
                              static_cast<unsigned long long>(calls));
                if (misses.size() < 600u)
                    misses += item;
            }
            std::fprintf(stderr,
                         "[sc3:mesh] 250 frames: raw parts %llu, drawn natively %llu (%llu triangles), left to VU1 %llu (routine not native %llu, "
                         "not in the pack %llu, no material %llu, no pack texture %llu); chain parts left to VU1 %llu; draws submitted %llu at %llu kicks; "
                         "MARKs reached %llu, out of order %llu, unknown %llu; leaves bound %llu, not matching %llu\n",
                         static_cast<unsigned long long>(t.parts), static_cast<unsigned long long>(t.native), static_cast<unsigned long long>(t.triangles),
                         static_cast<unsigned long long>(t.leftToVu1), static_cast<unsigned long long>(t.unsupported),
                         static_cast<unsigned long long>(t.notInPack), static_cast<unsigned long long>(t.noMaterial),
                         static_cast<unsigned long long>(t.noTexture), static_cast<unsigned long long>(t.chainParts),
                         static_cast<unsigned long long>(t.drawsSubmitted), static_cast<unsigned long long>(t.kicks),
                         static_cast<unsigned long long>(t.marks.exchange(0)), static_cast<unsigned long long>(t.marksOutOfOrder.exchange(0)),
                         static_cast<unsigned long long>(t.marksUnknown.exchange(0)), static_cast<unsigned long long>(t.bound),
                         static_cast<unsigned long long>(t.boundBad));
            if (!left.empty())
                std::fprintf(stderr, "[sc3:mesh] routines left to VU1:%s\n", left.c_str());
            if (!misses.empty())
                std::fprintf(stderr, "[sc3:mesh] texture places with no pack texture (left to VU1):%s\n", misses.c_str());
            if (s.mode == Mode::Check)
                std::fprintf(stderr, "[sc3:mesh] check: %llu parts against the lifted program, %llu bit-equal, %llu with a vertex off by more than one "
                             "unit, %llu without a vertex packet\n",
                             static_cast<unsigned long long>(t.checked), static_cast<unsigned long long>(t.checkExact),
                             static_cast<unsigned long long>(t.checkVertexDiffs), static_cast<unsigned long long>(t.checkCountDiffs));
            t.parts = t.native = t.triangles = t.leftToVu1 = t.unsupported = t.notInPack = t.noMaterial = t.noTexture = t.chainParts = 0;
            t.drawsSubmitted = t.kicks = t.checked = t.checkExact = t.checkVertexDiffs = t.checkCountDiffs = 0;
            t.routinesLeft.clear();
            t.textureMisses.clear();
        }

        void wrap(PS2Runtime &runtime, uint32_t address, Function hook)
        {
            Function original = runtime.lookupFunction(address);
            if (!original)
            {
                std::fprintf(stderr, "[sc3:mesh] no function at %#x\n", address);
                return;
            }
            g_state.originals[address] = original;
            runtime.registerFunction(address, hook);
        }

        bool readLeaves(const std::string &path)
        {
            std::ifstream file(path);
            std::string line;
            while (std::getline(file, line))
            {
                std::istringstream fields(line);
                Leaf leaf;
                std::string offset, size;
                if (!(fields >> leaf.name >> offset >> size))
                    continue;
                leaf.offset = std::strtoull(offset.c_str(), nullptr, 16);
                leaf.size = static_cast<uint32_t>(std::strtoul(size.c_str(), nullptr, 16));
                g_state.leaves.push_back(leaf);
            }
            std::sort(g_state.leaves.begin(), g_state.leaves.end(), [](const Leaf &a, const Leaf &b) { return a.offset < b.offset; });
            return !g_state.leaves.empty();
        }
    }
    }

    bool nativeMeshesOn() { return meshes::g_state.mode != meshes::Mode::Off; }

    void installNativeMeshes(PS2Runtime &runtime, bool nativeGs)
    {
        using namespace meshes;
        const char *mode = std::getenv("SC3_NATIVE_MESHES");
        const char *path = std::getenv("SC3_MESH_PACK");
        if (!mode || !*mode || *mode == '0')
            return;
        if (!nativeGs || !path || !*path)
        {
            std::fprintf(stderr, "[sc3:mesh] SC3_NATIVE_MESHES needs the native GS renderer and SC3_MESH_PACK, not used\n");
            return;
        }
        if (!texturePackReady())
        {
            std::fprintf(stderr, "[sc3:mesh] SC3_NATIVE_MESHES needs the texture pack (SC3_TEXTURE_PACK, SC3_TEXTURE_PACK_MODE), not used\n");
            return;
        }
        State &s = g_state;
        if (!s.pack.load(path))
        {
            std::fprintf(stderr, "[sc3:mesh] %s: %s\n", path, s.pack.error().c_str());
            return;
        }
        std::string leaves = path;
        const size_t dot = leaves.rfind('.');
        leaves = (dot == std::string::npos ? leaves : leaves.substr(0, dot)) + ".leaves";
        if (!readLeaves(leaves))
        {
            std::fprintf(stderr, "[sc3:mesh] %s: no leaves\n", leaves.c_str());
            return;
        }
        s.mode = std::strcmp(mode, "check") == 0 ? Mode::Check : Mode::On;
        if (const char *routines = std::getenv("SC3_NATIVE_MESHES_ROUTINES"); routines && *routines)
        {
            if (*routines == '@')
                g_routinesPath = routines + 1;
            else
                parseRoutines(routines);
        }
        if (const char *trace = std::getenv("SC3_MESH_TRACE"); trace && *trace)
            g_traceFrame = std::strtoull(trace, nullptr, 10);
        s.runtime = &runtime;
        s.rdram = runtime.memory().getRDRAM();
        s.scratchpad = runtime.memory().getScratchpad();
        s.submitter.tag = "sc3:mesh";
        s.checkState.status = 0x0C0u;
        wrap(runtime, kFileRequest, fileRequest);
        wrap(runtime, kCdRead, cdRead);
        wrap(runtime, kRawPart, rawPart);
        wrap(runtime, kRawPartTail, rawPartTail);
        wrap(runtime, kChainPart, chainPart);
        wrap(runtime, kPacketFlush, packetFlush);
        wrap(runtime, kOtInsertDepth, otInsertDepth);
        wrap(runtime, kOtInsertFixed, otInsertFixed);
        wrap(runtime, kFrameKick, frameKick);
        ps2x::native::setVif1MarkGate(markGate);
        std::fprintf(stderr, "[sc3:mesh] native meshes%s: %s, %zu leaves, %zu objects\n", s.mode == Mode::Check ? " (checked against the lifted programs)" : "",
                     path, s.pack.files().size(), s.pack.objectCount());
    }
}
