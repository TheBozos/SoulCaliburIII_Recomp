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
#include <set>
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
        constexpr uint32_t kClipper = 0x0010B970u;
        constexpr uint32_t kClipEmit = 0x0010B7E0u;
        constexpr uint32_t kClipVertexStride = 0x50u;
        constexpr uint32_t kRelocate = 0x0014A7D8u;
        constexpr uint32_t kScratchpad = 0x70000000u;
        constexpr uint32_t kSector = 2048u;
        constexpr uint32_t kRawPartKind = 1u;
        constexpr uint32_t kChainPartKind = 2u;
        constexpr uint32_t kVuBlockKind = 3u;
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
            bool setsAlpha = false, setsTest = false, known = false;
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
            uint64_t chainNotInPack = 0;
            uint64_t chainParts = 0, chainNative = 0, chainBatches = 0, headers = 0, bound = 0, boundBad = 0, kicks = 0, drawsSubmitted = 0;
            uint64_t inheritedAlpha = 0, inheritedTest = 0, unknownAlpha = 0, unknownTest = 0;
            uint64_t clipped = 0, clippedNative = 0, clippedToVu1 = 0, clippedDropped = 0;
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
            // ALPHA_1 / TEST_1 as the materials the game sent left them, in the order it sent them (every part's, native or
            // not): what a material without its own write draws with.
            // The bone matrices 0x10A4E8 last sent VU1 (scratchpad 0x350 / 0x354 / 0x358), by VU address / 4.
            float bones[256][16] = {};
            uint32_t boneBase = 0, boneIndices = 0, boneCount = 0;
            bool bonesOnVu1 = true;
            // The raw parts and chain lists whose GIF tags 0x14A7D8 cleared PRIM.FGE in when it relocated their model.
            std::set<uint32_t> noFog;
            uint64_t gameAlpha = 0, gameTest = 0;
            bool gameAlphaKnown = false, gameTestKnown = false;
            bool headerOnVu1 = true, materialOnVu1 = true; // VU1 holds the header / material the game sent last
            struct Clip
            {
                uint32_t flags = 0, address = 0, qwc = 0;
                bool valid = false;
            } clip;
            struct Placement
            {
                bool pending = false;
                uint32_t ra = 0, program = 0, programQwc = 0;
                std::vector<Sc3Draw> draws;
            } placement;
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
        bool g_skinning = false; // SC3_NATIVE_SKINNING=1: B:0x1388 chain parts too (unfinished: the run stops at character select)
        std::string g_traceRequest; // SC3_MESH_TRACE=@file: the frame after the file appears (it is removed)

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
                    s.noFog.erase(s.noFog.lower_bound(base), s.noFog.lower_bound(base + leaf->size));
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

        // 0x14A7D8 relocates a model's command list in place; with a5 bit 0 set (and a3 bit 1 clear) it also clears PRIM.FGE
        // in the GIF tag of every raw part (command 2) and of every block a chain list (command 5) REFs.
        void relocate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            if ((getRegU32(ctx, 7) & 2u) == 0u && (getRegU32(ctx, 9) & 1u) != 0u)
            {
                const uint32_t offset = getRegU32(ctx, 6);
                for (uint32_t at = getRegU32(ctx, 4) & PS2_RAM_MASK, n = 0; n < 65536u; ++n)
                {
                    const uint32_t word = peek32(at);
                    const uint32_t command = word & 0xFFu, size = (word >> 8) & 0xFFu;
                    if (command == 0u || size == 0u)
                        break;
                    if (command == 2u)
                        s.noFog.insert((peek32(at + 0x14u) + offset) & PS2_RAM_MASK);
                    else if (command == 5u)
                        s.noFog.insert((peek32(at + 4u) + offset) & PS2_RAM_MASK);
                    at += size;
                }
            }
            callOriginal(kRelocate, rdram, ctx, runtime);
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
            if (flags & kFlagShineA)
            {
                Header out = h;
                copyQwords(out, 0, (flags & kFlagProgramA) ? 0x100u : 0x140u, 4);
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
            // A:0x1AB8: three lights (vf20-22) and, instead of a fourth, a highlight from the fourth normal lane
            // (vf24-26.w) offset by vf15.zw, clamped, scaled by vf16.zw (w at most vf16.y) and added as vf23 * 128.
            bool highlight = false;
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
            case 0x1AB8u: shade = {true, false, false, false, true}; return true;
            case 0x18C0u: shade = {true, true, false, false, true}; return true;
            case 0x1F00u: shade = {false, false, false, true}; return true;
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
        void shadeEnvironment(const Header &h, bool fog, const std::vector<In> &in, std::vector<Out> &out)
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
                if (fog)
                {
                    const float f = std::max(std::min(g[3], h.f(9, 0)), h.f(9, 1));
                    o.w = ((((clipped ? 0x8000u : 0u) | v.flags) & 0x8000u) + (static_cast<uint32_t>(ftoi(f, 16.0f)) & 0xFFFFu)) & 0xFFFFu;
                }
                else
                    o.w = ((clipped ? 0x8000u : 0u) | v.flags) & 0xFFFFu;
            }
        }

        void shade(const Header &h, Shade mode, const std::vector<In> &in, std::vector<Out> &out)
        {
            if (mode.environment)
            {
                shadeEnvironment(h, mode.fog, in, out);
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
                else if (mode.highlight)
                {
                    float l[4];
                    for (int i = 0; i < 4; ++i)
                        l[i] = h.f(4, i) * v.normal[0] + h.f(5, i) * v.normal[1] + h.f(6, i) * v.normal[2];
                    for (int i = 0; i < 3; ++i)
                        l[i] = std::max(l[i], 0.0f);
                    float e[2] = {std::max(h.f(15, 2) + l[3], 0.0f) * h.f(16, 2), std::max(h.f(15, 3) + l[3], 0.0f) * h.f(16, 3)};
                    e[1] = std::min(e[1], h.f(16, 1));
                    for (int i = 0; i < 4; ++i)
                    {
                        const float lit = h.f(12, i) + h.f(8, i) * l[0] + h.f(9, i) * l[1] + h.f(10, i) * l[2];
                        const float highlight = h.f(11, i) * e[0] + h.f(11, i) * e[1];
                        c[i] = std::min(lit * v.colour[i] + highlight * 128.0f, h.f(16, 0));
                    }
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
        void compareKicks(uint32_t routine, const Header &h, const std::vector<Out> &out, uint32_t program, uint32_t pc, uint32_t top = 0u);

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
            compareKicks(routine, h, out, kProgramA, pc);
        }

        void compareKicks(uint32_t routine, const Header &h, const std::vector<Out> &out, uint32_t program, uint32_t pc, uint32_t top)
        {
            State &s = g_state;
            uint8_t *vu = s.checkMemory.data();
            CheckKick kick;
            runVu1Program(program, pc, s.checkState, vu, top, top,
                          [](void *user, const uint8_t *packet, uint32_t bytes)
                          { static_cast<CheckKick *>(user)->packets.emplace_back(packet, packet + bytes); },
                          &kick);
            ++s.stats.checked;
            const std::vector<uint8_t> *vertices = nullptr;
            for (const std::vector<uint8_t> &p : kick.packets)
                if (p.size() >= 16u + 48u * out.size() && p.size() >= 16u)
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
            if (!s.headerOnVu1 && sp32(0x368u) == 0u)
                spSet(0x368u, flags);
            if (!s.materialOnVu1 && sp32(0x36Cu) == 0u && s.material.address != 0u)
            {
                spSet(0x36Cu, s.material.address);
                spSet(0x370u, s.material.qwc);
            }
            if (!s.bonesOnVu1 && sp32(0x350u) == 0u && s.boneBase != 0u && !(flags & kFlagProgramA))
            {
                spSet(0x350u, s.boneBase);
                spSet(0x354u, s.boneIndices);
                spSet(0x358u, s.boneCount);
            }
            if (sp32(0x350u) != 0u)
                s.bonesOnVu1 = true;
            s.headerOnVu1 = s.materialOnVu1 = true;
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
                {
                    m.known = true;
                    s.material = m;
                    if (m.setsAlpha)
                        s.gameAlpha = m.alpha, s.gameAlphaKnown = true;
                    if (m.setsTest)
                        s.gameTest = m.test, s.gameTestKnown = true;
                }
                else
                    s.material = Material{0, 0, 0, 0x44, 0, 0, 0, 0, false, false, false, block, m.qwc};
            }
        }

        void packetFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

        // The placeholder the part leaves in the game's VIF1 list: a MARK where its draws belong (the GS stream's place
        // for them while the rest of the frame still goes through the GS front end), and the microprogram upload the
        // game asked for (parts left to VU1 after it need it).
        uint32_t writeMark(uint32_t cursor, uint16_t mark)
        {
            poke32(cursor, 0x10000000u);
            poke32(cursor + 4u, 0u);
            poke32(cursor + 8u, 0x07000000u | mark);
            poke32(cursor + 12u, 0u);
            return cursor + 16u;
        }

        void queueDraw(Sc3Draw &&draw);

        void emitPlacement(R5900Context *ctx)
        {
            State &s = g_state;
            State::Placement &p = s.placement;
            uint32_t cursor = sp32(0x378u);
            if (p.program != 0u)
            {
                poke32(cursor, 0x30000000u | p.programQwc);
                poke32(cursor + 4u, p.program);
                poke32(cursor + 8u, 0u);
                poke32(cursor + 12u, 0u);
                cursor += 16u;
            }
            for (Sc3Draw &draw : p.draws)
            {
                cursor = writeMark(cursor, draw.mark);
                queueDraw(std::move(draw));
            }
            spSet(0x378u, cursor);
            p.draws.clear();
            p.pending = false;
            p.program = 0u;
            setReg(ctx, 31, p.ra);
            ctx->pc = p.ra;
        }

        // A native part's placeholder (the microprogram upload the game asked for, then a MARK per draw) goes into the scratchpad
        // packet and its draws are queued. With no room left, the game's flush runs first as a guest call returning into
        // `entry` (the part's hook, which then finishes here): a recompiled function called from C++ may return before its end.
        void place(R5900Context *ctx, uint32_t entry, std::vector<Sc3Draw> &&draws, bool part)
        {
            State &s = g_state;
            State::Placement &p = s.placement;
            p.program = part ? sp32(0x384u) : 0u;
            p.programQwc = sp32(0x388u);
            if (part)
            {
                spSet(0x384u, 0u);
                spSet(0x368u, 0u);
                spSet(0x350u, 0u);
                spSet(0x36Cu, 0u);
            }
            p.draws = std::move(draws);
            p.ra = getRegU32(ctx, 31);
            const uint32_t qwords = (p.program != 0u ? 1u : 0u) + static_cast<uint32_t>(p.draws.size());
            const uint32_t cursor = sp32(0x378u);
            if (sp32(0x374u) + sp32(0x18u) - 0x10u < cursor + qwords * 16u)
            {
                p.pending = true;
                setReg(ctx, 4, cursor);
                setReg(ctx, 31, entry);
                ctx->pc = kPacketFlush;
                return;
            }
            emitPlacement(ctx);
        }

        bool resumePlacement(R5900Context *ctx, uint32_t entry)
        {
            State &s = g_state;
            if (!s.placement.pending || getRegU32(ctx, 31) != entry)
                return false;
            const uint32_t start = sp32(0x0u);
            spSet(0x374u, start);
            spSet(0x378u, start);
            emitPlacement(ctx);
            return true;
        }

        struct PartSource
        {
            uint32_t file = 0, offset = 0, object = 0;
            bool noFog = false;
        };

        bool fogCleared(uint32_t address) { return g_state.noFog.count(address & PS2_RAM_MASK) != 0u; }

        // A part's draw from its vertices as its routine finds them in VU memory (the GIF tag, four quadwords a vertex), under
        // the header and material the game sent last. False: nothing drawn, the reason counted.
        bool drawVertices(uint32_t routine, const Shade &mode, const uint32_t tag[4], const std::vector<In> &in, const PartSource &source, Sc3Draw &draw,
                          const std::vector<Out> *shaded = nullptr)
        {
            State &s = g_state;
            const uint32_t prim = ((tag[1] >> 15) & 0x7FFu) & (source.noFog ? ~0x20u : ~0u);
            if (!s.material.known || (((prim >> 4) & 1u) != 0u && s.material.tex0 == 0u))
            {
                ++s.stats.noMaterial;
                return false;
            }
            GsMaterial material;
            material.prim = ps2x::gs::decodePrim(prim);
            if (material.prim.tme)
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
            if (shaded)
                out = *shaded;
            else
            {
                shade(s.header, mode, in, out);
                if (s.mode == Mode::Check)
                    checkPart(s.header, routine, tag, in, out);
            }
            draw.screenSpace = true;
            draw.file = source.file;
            draw.offset = source.offset;
            draw.object = source.object;
            MeshBatch batch;
            batch.gs = true;
            batch.gsMaterial = material;
            batch.setsAlpha = s.material.setsAlpha || s.gameAlphaKnown;
            batch.alpha = s.material.setsAlpha ? s.material.alpha : s.gameAlpha;
            batch.setsTest = s.material.setsTest || s.gameTestKnown;
            batch.test = s.material.setsTest ? s.material.test : s.gameTest;
            if (!s.material.setsAlpha)
                ++(s.gameAlphaKnown ? s.stats.inheritedAlpha : s.stats.unknownAlpha);
            if (!s.material.setsTest)
                ++(s.gameTestKnown ? s.stats.inheritedTest : s.stats.unknownTest);
            toTriangles(prim, out, batch.gsTriangles);
            s.stats.triangles += batch.gsTriangles.size() / 3u;
            if (s.frames == g_traceFrame)
            {
                uint32_t z0 = ~0u, z1 = 0u, a0 = 255u, a1 = 0u;
                for (const GsVertex &v : batch.gsTriangles)
                    z0 = std::min(z0, v.z), z1 = std::max(z1, v.z), a0 = std::min(a0, v.rgba >> 24), a1 = std::max(a1, v.rgba >> 24);
                std::fprintf(stderr, "[sc3:mesh] trace draw %#x routine %#x prim %#x tex0 %#llx key %#llx alpha %#llx/%d test %#llx/%d tris %zu of %zu z %#x..%#x va %u..%u mark %u\n",
                             source.object, routine, prim, static_cast<unsigned long long>(s.material.tex0),
                             static_cast<unsigned long long>(material.texture ? material.texture->key : 0u), static_cast<unsigned long long>(s.material.alpha),
                             s.material.setsAlpha, static_cast<unsigned long long>(s.material.test), s.material.setsTest, batch.gsTriangles.size() / 3u,
                             out.size(), z0, z1, a0, a1, s.nextMark);
                if (a1 == 0u && !in.empty())
                {
                    std::fprintf(stderr, "[sc3:mesh] trace   in0 st %g %g col %g %g %g %g pos %g %g %g flags %#x; q12 %g %g %g %g\n", in[0].st[0], in[0].st[1],
                                 in[0].colour[0], in[0].colour[1], in[0].colour[2], in[0].colour[3], in[0].position[0], in[0].position[1], in[0].position[2],
                                 in[0].flags, s.header.f(12, 0), s.header.f(12, 1), s.header.f(12, 2), s.header.f(12, 3));
                    std::string codes;
                    for (uint32_t at = source.object, end = source.object + 0x40u * 16u; at < end && codes.size() < 400u;)
                    {
                        const uint32_t code = peek32(at);
                        char item[24];
                        std::snprintf(item, sizeof(item), " %08x", code);
                        codes += item;
                        const uint32_t cmd = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu;
                        at += 4u;
                        if (cmd >= 0x60u)
                        {
                            const uint32_t vl = cmd & 3u, vn = (cmd >> 2) & 3u;
                            const uint32_t bits = (32u >> vl) * (vn + 1u) * (num ? num : 256u);
                            at += ((bits + 31u) / 32u) * 4u;
                        }
                    }
                    std::fprintf(stderr, "[sc3:mesh] trace   vif%s\n", codes.c_str());
                }
            }
            draw.batches.push_back(std::move(batch));
            draw.mark = s.nextMark;
            s.nextMark = static_cast<uint16_t>(s.nextMark == 0xFFFFu ? 1u : s.nextMark + 1u);
            return true;
        }

        // B:0x1388 in plain C++. A vertex in its block: per bone influence a position quadword (x, y, z, weight) and a
        // normal quadword (x, y, z, the VU address of the bone's matrix, bit 15 on the last influence), then the texture
        // coordinates (w: the vertex flags) and the colour. Position and normal are the influences' sum through their bone
        // matrices, the position then through the header's matrix; the shading is A:0x1AB8's (three lights and the
        // highlight); the XYZF word's bit 15 when the guard test fails or the flags are not 0, plus the header's vi3.
        // The other skinned routines of program B: B:0x15E8 culls back faces (a vertex also gets bit 15 when the screen-space
        // cross product of the triangle it ends, (P1 - P0) x (P2 - P0), is negative in z, the winding alternating along the
        // strip and starting reversed when the tag's last word is not 0); B:0x350 and B:0x5B0 (culled) shade like A:0x2B0
        // (four lights, no highlight); B:0x1CA8 and B:0x1EF0 (culled), after B:0x1990 and 0x10AFE8's header, map the
        // environment like A:0x1D28 from the skinned normal alone, one colour for all.
        struct Skinning
        {
            bool highlight = true, cull = false, orVi3 = false, environment = false;
        };

        bool skinRoutine(uint32_t routine, Skinning &k)
        {
            switch (routine)
            {
            case 0x1388u: k = {true, false, false}; return true;
            case 0x15E8u: k = {true, true, true}; return true;
            case 0x350u: k = {false, false, false}; return true;
            case 0x5B0u: k = {false, true, false}; return true;
            case 0x1CA8u: k = {false, false, false, true}; return true;
            case 0x1EF0u: k = {false, true, true, true}; return true;
            default: return false;
            }
        }

        bool skinBlock(const Header &h, const uint8_t *data, uint32_t qwords, uint32_t tag[4], std::vector<Out> &out, Skinning kind)
        {
            State &s = g_state;
            const auto word = [&](uint32_t q, int lane) { uint32_t w; std::memcpy(&w, data + 16u * q + 4u * static_cast<uint32_t>(lane), 4); return w; };
            const auto flt = [&](uint32_t q, int lane) { float f; std::memcpy(&f, data + 16u * q + 4u * static_cast<uint32_t>(lane), 4); return f; };
            if (qwords < 1u)
                return false;
            for (int i = 0; i < 4; ++i)
                tag[i] = word(0, i);
            const int control = kind.environment ? 14 : 17;
            const uint32_t vi12 = h.q[control][0] & 0xFFFFu;
            const int count = static_cast<int16_t>(tag[0] & 0xFFFFu) - static_cast<int16_t>(vi12);
            if (count <= 0)
                return false;
            out.clear();
            uint32_t q = 1u;
            bool outside[3] = {false, false, false};
            const uint32_t vi3 = (kind.environment ? h.q[9][3] : h.q[17][3]) & 0xFFFFu;
            const bool reversed = (tag[3] & 0xFFFFu) != 0u;
            float screen[3][2] = {};
            const auto finish = [&](Out &o, bool clipped, uint32_t flags, const float *position, int k)
            {
                uint32_t back = 0u;
                if (kind.cull)
                {
                    screen[0][0] = screen[1][0], screen[0][1] = screen[1][1];
                    screen[1][0] = screen[2][0], screen[1][1] = screen[2][1];
                    screen[2][0] = position[0], screen[2][1] = position[1];
                    const float a[2] = {screen[1][0] - screen[0][0], screen[1][1] - screen[0][1]};
                    const float b[2] = {screen[2][0] - screen[0][0], screen[2][1] - screen[0][1]};
                    const bool odd = ((k & 1) != 0) != reversed;
                    const float z = odd ? b[0] * a[1] - a[0] * b[1] : a[0] * b[1] - b[0] * a[1];
                    back = std::signbit(z) ? 0x8000u : 0u;
                }
                const uint32_t bit = ((((clipped ? 1u : 0u) | flags) + 0x7FFFu) | back) & vi12;
                o.w = (kind.orVi3 ? bit | vi3 : bit + vi3) & 0xFFFFu;
                out.push_back(o);
            };
            for (int k = 0; k < count; ++k)
            {
                float pos[4] = {0, 0, 0, 0}, nrm[4] = {0, 0, 0, 0};
                for (int influence = 0;; ++influence)
                {
                    if (q + 2u > qwords || influence > 16)
                        return false;
                    const uint32_t pointer = word(q + 1u, 3) & 0xFFFFu;
                    const float *m = s.bones[(pointer & 0x3FFu) >> 2];
                    for (int c = 0; c < 4; ++c)
                    {
                        pos[c] = pos[c] + m[c] * flt(q, 0) + m[4 + c] * flt(q, 1) + m[8 + c] * flt(q, 2) + m[12 + c] * flt(q, 3);
                        nrm[c] = nrm[c] + m[c] * flt(q + 1u, 0) + m[4 + c] * flt(q + 1u, 1) + m[8 + c] * flt(q + 1u, 2);
                    }
                    q += 2u;
                    if (pointer & 0x8000u)
                        break;
                }
                if (q + 2u > qwords)
                    return false;
                const uint32_t flags = word(q, 3) & 0xFFFFu;
                const float st[2] = {flt(q, 0), flt(q, 1)};
                const float colour[4] = {flt(q + 1u, 0), flt(q + 1u, 1), flt(q + 1u, 2), flt(q + 1u, 3)};
                q += 2u;
                float p[4];
                for (int c = 0; c < 4; ++c)
                    p[c] = h.f(0, c) * pos[0] + h.f(1, c) * pos[1] + h.f(2, c) * pos[2] + h.f(3, c);
                float l[4];
                for (int i = 0; i < 4; ++i)
                    l[i] = h.f(4, i) * nrm[0] + h.f(5, i) * nrm[1] + h.f(6, i) * nrm[2];
                Out o;
                if (kind.environment)
                {
                    const float qv = 1.0f / p[3];
                    float r[3];
                    for (int c = 0; c < 3; ++c)
                        r[c] = h.f(4, c) * nrm[0] + h.f(5, c) * nrm[1] + h.f(6, c) * nrm[2];
                    const float position[3] = {p[0] * qv, p[1] * qv, p[2] * qv};
                    float g[3];
                    for (int i = 0; i < 3; ++i)
                        g[i] = sat(static_cast<double>(sat(static_cast<double>(h.f(7, i)) * h.f(8, i))) + static_cast<double>(sat(static_cast<double>(position[i]) * h.f(8, i))));
                    outside[k % 3] = std::fabs(g[0]) > 1.0f || std::fabs(g[1]) > 1.0f || std::fabs(g[2]) > 1.0f;
                    const bool clipped = outside[0] || outside[1] || outside[2];
                    for (int i = 0; i < 4; ++i)
                        o.rgba[i] = static_cast<int32_t>(h.q[10][i]);
                    for (int i = 0; i < 3; ++i)
                        o.xyz[i] = ftoi(position[i], 16.0f);
                    o.s = (h.f(13, 0) + r[0] * h.f(12, 0)) * qv;
                    o.t = (h.f(13, 1) + r[1] * h.f(12, 1)) * qv;
                    o.q = kind.cull ? qv : (h.f(13, 2) + r[2] * h.f(12, 2)) * qv;
                    finish(o, clipped, flags, position, k);
                    continue;
                }
                if (kind.highlight)
                {
                    float e[2] = {std::max(h.f(15, 2) + l[3], 0.0f) * h.f(16, 2), std::max(h.f(15, 3) + l[3], 0.0f) * h.f(16, 3)};
                    e[1] = std::min(e[1], h.f(16, 1));
                    for (int i = 0; i < 4; ++i)
                    {
                        const float lit = h.f(12, i) + h.f(8, i) * std::max(l[0], 0.0f) + h.f(9, i) * std::max(l[1], 0.0f) + h.f(10, i) * std::max(l[2], 0.0f);
                        const float highlight = h.f(11, i) * e[0] + h.f(11, i) * e[1];
                        o.rgba[i] = ftoi(std::min(lit * colour[i] + highlight * 128.0f, h.f(16, 0)), 1.0f);
                    }
                }
                else
                {
                    for (int i = 0; i < 4; ++i)
                        l[i] = std::max(l[i], 0.0f);
                    for (int i = 0; i < 4; ++i)
                    {
                        const float lit = h.f(12, i) + h.f(8, i) * l[0] + h.f(9, i) * l[1] + h.f(10, i) * l[2] + h.f(11, i) * l[3];
                        o.rgba[i] = ftoi(std::min(lit * colour[i], h.f(16, 0)), 1.0f);
                    }
                }
                const float qv = 1.0f / p[3];
                const float position[4] = {p[0] * qv, p[1] * qv, p[2] * qv, p[3]};
                float g[3];
                for (int i = 0; i < 3; ++i)
                    g[i] = sat(static_cast<double>(sat(static_cast<double>(h.f(13, i)) * h.f(14, i))) + static_cast<double>(sat(static_cast<double>(position[i]) * h.f(14, i))));
                outside[k % 3] = std::fabs(g[0]) > 1.0f || std::fabs(g[1]) > 1.0f || std::fabs(g[2]) > 1.0f;
                const bool clipped = outside[0] || outside[1] || outside[2];
                for (int i = 0; i < 3; ++i)
                    o.xyz[i] = ftoi(position[i], 16.0f);
                o.s = st[0] * h.f(18, 0) * qv;
                o.t = st[1] * h.f(18, 1) * qv;
                o.q = qv;
                finish(o, clipped, flags, position, k);
            }
            return true;
        }

        // Check mode: the lifted program B on the same block, bones and header.
        void checkSkinned(const Header &h, uint32_t routine, const uint8_t *data, uint32_t qwords, const std::vector<Out> &out)
        {
            State &s = g_state;
            uint8_t *vu = s.checkMemory.data();
            std::memset(vu, 0, s.checkMemory.size());
            for (uint32_t i = 0; i < 256u; ++i)
                std::memcpy(vu + 64u * i, s.bones[i], 64u);
            constexpr uint32_t kTop = 0x200u;
            std::memcpy(vu + kTop * 16u, data, std::min<size_t>(16u * qwords, 0x1ADu * 16u));
            Header setup = h;
            const int control = h.shine ? 14 : 17;
            setup.q[control][1] = 0u;
            setup.q[control][2] = routine >> 3;
            for (int i = 0; i < 19; ++i)
                std::memcpy(vu + (kTop + 0x1ADu + static_cast<uint32_t>(i)) * 16u, setup.q[i], 16u);
            compareKicks(routine, h, out, kProgramB, h.shine ? 0x1990u : 0x10u, kTop);
        }

        void queueDraw(Sc3Draw &&draw)
        {
            State &s = g_state;
            s.unflushed.push_back(s.pendingDraws.size());
            s.pendingDraws.push_back(std::move(draw));
        }

        void noteBones();

        bool drawRawPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t address = getRegU32(ctx, 4) & PS2_RAM_MASK, qwc = getRegU32(ctx, 5), flags = getRegU32(ctx, 6);
            const uint32_t routine = flags & 0xFFFFu;
            ++s.stats.parts;
            Shade mode;
            const bool header = sp32(0x368u) != 0u, materialSent = sp32(0x36Cu) != 0u;
            noteState(flags, routine >> 3);
            noteBones();
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
            std::vector<In> in;
            uint32_t tag[4];
            if (!readVertices(*object, in, tag))
            {
                ++s.stats.notInPack;
                return false;
            }
            Sc3Draw draw;
            if (!drawVertices(routine, mode, tag, in, PartSource{image.index, address - image.base, address, fogCleared(address)}, draw))
                return false;
            if (header)
                s.headerOnVu1 = false;
            if (materialSent)
                s.materialOnVu1 = false;
            ++s.stats.native;
            std::vector<Sc3Draw> draws;
            draws.push_back(std::move(draw));
            place(ctx, kRawPart, std::move(draws), true);
            return true;
        }

        void rawPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (resumePlacement(ctx, kRawPart) || drawRawPart(rdram, ctx, runtime))
                return;
            ++g_state.stats.leftToVu1;
            selfContained(getRegU32(ctx, 6));
            callOriginal(kRawPart, rdram, ctx, runtime);
        }

        // 0x1088A0 drains the parts deferred for clipping against the near plane: each one's raw data goes to the scratchpad,
        // 0x10B970 clips it on the EE and 0x10B7E0 sends the clipped polygon as a fan with the part's routine and no header or material
        // (the VU keeps those of the last part). Its loop's s0 is one entry past the part's {flags, address, qwc}.
        void clipper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t entry = getRegU32(ctx, 16) - 0x10u;
            s.clip.flags = peek32(entry);
            s.clip.address = peek32(entry + 4u) & PS2_RAM_MASK;
            s.clip.qwc = peek32(entry + 8u);
            s.clip.valid = s.clip.flags == getRegU32(ctx, 4);
            callOriginal(kClipper, rdram, ctx, runtime);
        }

        bool drawClipped(R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t vertices = getRegU32(ctx, 4), count = getRegU32(ctx, 5), flags = getRegU32(ctx, 7);
            const uint32_t routine = flags & 0xFFFFu;
            Shade mode;
            if (!s.clip.valid || flags != s.clip.flags || !(flags & kFlagProgramA) || !routineShade(routine, mode) || !routineAllowed(routine) ||
                !s.header.valid || s.header.shine != mode.environment)
                return false;
            packs::LoadedImages::Image image;
            const packs::MeshObject *object = nullptr;
            if (s.loaded.find(s.clip.address, image) && image.file)
                object = image.file->object(s.clip.address - image.base);
            std::vector<In> in;
            uint32_t tag[4];
            if (!object || object->kind != kRawPartKind || object->attribute<uint32_t>(0) != s.clip.qwc * 16u || !readVertices(*object, in, tag))
                return false;
            tag[0] = (tag[0] & 0xFFFF8000u) | count;
            tag[1] = (tag[1] & 0xFFFC7FFFu) | 0x28000u;
            in.resize(count);
            for (uint32_t k = 0; k < count; ++k)
            {
                float w[16];
                const uint32_t at = vertices + k * kClipVertexStride;
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    const uint32_t word = peek32(at + i * 4u);
                    std::memcpy(&w[i], &word, 4);
                }
                In &v = in[k];
                v.st[0] = w[0], v.st[1] = w[1];
                std::memcpy(v.colour, w + 4, 16);
                std::memcpy(v.position, w + 8, 12);
                std::memcpy(&v.flags, w + 11, 4);
                std::memcpy(v.normal, w + 12, 12);
            }
            Sc3Draw draw;
            if (!drawVertices(routine, mode, tag, in, PartSource{image.index, s.clip.address - image.base, s.clip.address, fogCleared(s.clip.address)}, draw))
                return false;
            std::vector<Sc3Draw> draws;
            draws.push_back(std::move(draw));
            place(ctx, kClipEmit, std::move(draws), false);
            return true;
        }

        void clipEmit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t ra = getRegU32(ctx, 31);
            if (resumePlacement(ctx, kClipEmit))
                return;
            ++s.stats.clipped;
            if (drawClipped(ctx, runtime))
            {
                ++s.stats.clippedNative;
                return;
            }
            if (!s.headerOnVu1 || !s.materialOnVu1) // VU1 would run it with registers no part sent it
            {
                ++s.stats.clippedDropped;
                ctx->pc = ra;
                return;
            }
            ++s.stats.clippedToVu1;
            callOriginal(kClipEmit, rdram, ctx, runtime);
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
            setReg(ctx, 4, peek32(command + 0x14u));
            setReg(ctx, 5, peek32(command) >> 16);
            setReg(ctx, 6, flags);
            rawPart(rdram, ctx, runtime);
        }

        // A program B chain part with B:0x1388 (skinned bodies): its blocks from the pack, the bones the game sent.
        bool drawSkinnedPart(R5900Context *ctx, PS2Runtime *runtime, uint32_t list, uint32_t flags, bool header, bool materialSent)
        {
            State &s = g_state;
            const bool bonesSent = sp32(0x350u) != 0u;
            const uint32_t routine = flags & 0xFFFFu;
            Skinning kind;
            if (!skinRoutine(routine, kind) || !routineAllowed(0x10000u | routine) || (flags & kFlagShineB) ||
                (kind.environment != ((flags & kFlagShineA) != 0u)) || (header && setupOf(flags) != (kind.environment ? 0x1990u : 0x10u)) ||
                !s.header.valid || s.header.shine != kind.environment)
                return false;
            packs::LoadedImages::Image image;
            const packs::MeshObject *chain = nullptr;
            if (s.loaded.find(list, image) && image.file)
                chain = image.file->object(list - image.base);
            const packs::MeshSection *batches = chain && chain->kind == kChainPartKind ? chain->section(packs::fourcc("SC3C")) : nullptr;
            if (!batches || batches->size < 8u)
                return false;
            const size_t n = batches->size / 8u;
            std::vector<Sc3Draw> draws(n);
            for (size_t i = 0; i < n; ++i)
            {
                uint32_t entry[2];
                std::memcpy(entry, batches->data + 8u * i, 8);
                const packs::MeshObject *object = image.file->object(entry[0]);
                const packs::MeshSection *block = object && object->kind == kVuBlockKind ? object->section(packs::fourcc("SC3B")) : nullptr;
                if (!block || object->attribute<uint32_t>(0) != entry[1] * 16u)
                    return false;
                uint32_t tag[4];
                std::vector<Out> out;
                if (!skinBlock(s.header, block->data, block->size / 16u, tag, out, kind))
                    return false;
                if (s.mode == Mode::Check)
                    checkSkinned(s.header, routine, block->data, block->size / 16u, out);
                if (!drawVertices(routine, Shade{}, tag, {}, PartSource{image.index, entry[0], image.base + entry[0], fogCleared(list)}, draws[i], &out))
                    return false;
            }
            if (header)
                s.headerOnVu1 = false;
            if (materialSent)
                s.materialOnVu1 = false;
            if (bonesSent)
                s.bonesOnVu1 = false;
            ++s.stats.chainNative;
            s.stats.chainBatches += n;
            place(ctx, kChainPart, std::move(draws), true);
            return true;
        }

        // A chain part (0x10A4E8): the list the game CALLs, a REF to a block of VU data and an MSCAL of the part's routine per
        // batch, from the pack. A program A part whose blocks are all rigid parts the pack knows is drawn like raw parts, one
        // draw a batch.
        bool drawChainPart(R5900Context *ctx, PS2Runtime *runtime, uint32_t list, uint32_t flags, bool header, bool materialSent)
        {
            State &s = g_state;
            const uint32_t routine = flags & 0xFFFFu;
            if (!(flags & kFlagProgramA))
                return g_skinning && drawSkinnedPart(ctx, runtime, list, flags, header, materialSent);
            Shade mode;
            if (!routineShade(routine, mode) || !routineAllowed(routine) ||
                (header && setupOf(flags) != (mode.environment ? 0x1C80u : 0x10u)) || sp32(0x350u) != 0u || !s.header.valid ||
                s.header.shine != mode.environment)
                return false;
            packs::LoadedImages::Image image;
            const packs::MeshObject *chain = nullptr;
            if (s.loaded.find(list, image) && image.file)
                chain = image.file->object(list - image.base);
            const packs::MeshSection *batches = chain && chain->kind == kChainPartKind ? chain->section(packs::fourcc("SC3C")) : nullptr;
            if (!batches || batches->size < 8u)
            {
                ++s.stats.chainNotInPack;
                return false;
            }
            struct Batch
            {
                std::vector<In> in;
                uint32_t tag[4];
                uint32_t offset;
            };
            std::vector<Batch> parts(batches->size / 8u);
            for (size_t i = 0; i < parts.size(); ++i)
            {
                uint32_t entry[2];
                std::memcpy(entry, batches->data + 8u * i, 8);
                const packs::MeshObject *object = image.file->object(entry[0]);
                if (!object || object->kind != kRawPartKind || object->attribute<uint32_t>(0) != entry[1] * 16u ||
                    !readVertices(*object, parts[i].in, parts[i].tag))
                {
                    ++s.stats.chainNotInPack;
                    return false;
                }
                parts[i].offset = entry[0];
            }
            std::vector<Sc3Draw> draws(parts.size());
            for (size_t i = 0; i < parts.size(); ++i)
                if (!drawVertices(routine, mode, parts[i].tag, parts[i].in, PartSource{image.index, parts[i].offset, image.base + parts[i].offset, fogCleared(list)}, draws[i]))
                    return false;
            if (header)
                s.headerOnVu1 = false;
            if (materialSent)
                s.materialOnVu1 = false;
            ++s.stats.chainNative;
            s.stats.chainBatches += parts.size();
            place(ctx, kChainPart, std::move(draws), true);
            return true;
        }

        // The bone matrices 0x10A4E8 sends with a part: matrix i (i < scratchpad 0x358) from 0x350 + 64 * byte[0x354 + i]
        // to VU address 4i, where later parts without bones still find them.
        void noteBones()
        {
            State &s = g_state;
            const uint32_t base = sp32(0x350u);
            if (base == 0u)
                return;
            const uint32_t count = sp32(0x358u), indices = sp32(0x354u);
            s.boneBase = base, s.boneIndices = indices, s.boneCount = count;
            for (uint32_t i = 0; i < count && i < 256u; ++i)
            {
                const uint32_t index = (peek32(indices + i) >> (8u * ((indices + i) & 3u))) & 0xFFu;
                for (uint32_t w = 0; w < 16u; ++w)
                {
                    const uint32_t bits = peek32(base + 64u * index + 4u * w);
                    std::memcpy(&s.bones[i][w], &bits, 4);
                }
            }
        }

        void chainPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            if (resumePlacement(ctx, kChainPart))
                return;
            const uint32_t list = getRegU32(ctx, 4) & PS2_RAM_MASK, flags = getRegU32(ctx, 5);
            ++s.stats.chainParts;
            if (s.frames == g_traceFrame)
                std::fprintf(stderr, "[sc3:mesh] trace chain %#x flags %#x 368 %#x 36c %#x 384 %#x/%#x 350 %#x\n", list, flags,
                             sp32(0x368u), sp32(0x36Cu), sp32(0x384u), sp32(0x388u), sp32(0x350u));
            const bool header = sp32(0x368u) != 0u, materialSent = sp32(0x36Cu) != 0u;
            noteState(flags, (flags & 0xFFFFu) >> 3);
            noteBones();
            if (drawChainPart(ctx, runtime, list, flags, header, materialSent))
                return;
            ++s.stats.routinesLeft[((flags & kFlagProgramA) ? 0u : 0x10000u) | (flags & 0xFFFFu)];
            selfContained(flags);
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
            if (!g_traceRequest.empty())
                if (std::FILE *f = std::fopen(g_traceRequest.c_str(), "rb"))
                {
                    std::fclose(f);
                    std::remove(g_traceRequest.c_str());
                    g_traceFrame = s.frames + 2u;
                }
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
                         "not in the pack %llu, no material %llu, no pack texture %llu); chain parts %llu (drawn natively %llu, %llu batches); draws submitted %llu at %llu kicks; "
                         "MARKs reached %llu, out of order %llu, unknown %llu; leaves bound %llu, not matching %llu; clipped strips %llu (native %llu, "
                         "VU1 %llu, not drawn %llu)\n",
                         static_cast<unsigned long long>(t.parts), static_cast<unsigned long long>(t.native), static_cast<unsigned long long>(t.triangles),
                         static_cast<unsigned long long>(t.leftToVu1), static_cast<unsigned long long>(t.unsupported),
                         static_cast<unsigned long long>(t.notInPack), static_cast<unsigned long long>(t.noMaterial),
                         static_cast<unsigned long long>(t.noTexture), static_cast<unsigned long long>(t.chainParts),
                         static_cast<unsigned long long>(t.chainNative), static_cast<unsigned long long>(t.chainBatches),
                         static_cast<unsigned long long>(t.drawsSubmitted), static_cast<unsigned long long>(t.kicks),
                         static_cast<unsigned long long>(t.marks.exchange(0)), static_cast<unsigned long long>(t.marksOutOfOrder.exchange(0)),
                         static_cast<unsigned long long>(t.marksUnknown.exchange(0)), static_cast<unsigned long long>(t.bound),
                         static_cast<unsigned long long>(t.boundBad), static_cast<unsigned long long>(t.clipped),
                         static_cast<unsigned long long>(t.clippedNative), static_cast<unsigned long long>(t.clippedToVu1),
                         static_cast<unsigned long long>(t.clippedDropped));
            if (!left.empty())
                std::fprintf(stderr, "[sc3:mesh] routines left to VU1:%s (chain parts not in the pack %llu)\n", left.c_str(),
                             static_cast<unsigned long long>(t.chainNotInPack));
            t.chainNotInPack = 0;
            if (t.inheritedAlpha + t.inheritedTest + t.unknownAlpha + t.unknownTest != 0u)
                std::fprintf(stderr, "[sc3:mesh] native parts whose material sets no ALPHA %llu (no earlier one %llu), no TEST %llu (no earlier one %llu)\n",
                             static_cast<unsigned long long>(t.inheritedAlpha + t.unknownAlpha), static_cast<unsigned long long>(t.unknownAlpha),
                             static_cast<unsigned long long>(t.inheritedTest + t.unknownTest), static_cast<unsigned long long>(t.unknownTest));
            t.inheritedAlpha = t.inheritedTest = t.unknownAlpha = t.unknownTest = 0;
            if (!misses.empty())
                std::fprintf(stderr, "[sc3:mesh] texture places with no pack texture (left to VU1):%s\n", misses.c_str());
            if (s.mode == Mode::Check)
                std::fprintf(stderr, "[sc3:mesh] check: %llu parts against the lifted program, %llu bit-equal, %llu with a vertex off by more than one "
                             "unit, %llu without a vertex packet\n",
                             static_cast<unsigned long long>(t.checked), static_cast<unsigned long long>(t.checkExact),
                             static_cast<unsigned long long>(t.checkVertexDiffs), static_cast<unsigned long long>(t.checkCountDiffs));
            t.chainNative = t.chainBatches = 0;
            t.parts = t.native = t.triangles = t.leftToVu1 = t.unsupported = t.notInPack = t.noMaterial = t.noTexture = t.chainParts = 0;
            t.drawsSubmitted = t.kicks = t.checked = t.checkExact = t.checkVertexDiffs = t.checkCountDiffs = 0;
            t.clipped = t.clippedNative = t.clippedToVu1 = t.clippedDropped = 0;
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
        if (const char *skinning = std::getenv("SC3_NATIVE_SKINNING"); skinning && *skinning == '1')
            g_skinning = true;
        if (const char *trace = std::getenv("SC3_MESH_TRACE"); trace && *trace)
        {
            if (*trace == '@')
                g_traceRequest = trace + 1;
            else
                g_traceFrame = std::strtoull(trace, nullptr, 10);
        }
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
        wrap(runtime, kRelocate, relocate);
        wrap(runtime, kClipper, clipper);
        wrap(runtime, kClipEmit, clipEmit);
        wrap(runtime, kPacketFlush, packetFlush);
        wrap(runtime, kOtInsertDepth, otInsertDepth);
        wrap(runtime, kOtInsertFixed, otInsertFixed);
        wrap(runtime, kFrameKick, frameKick);
        ps2x::native::setVif1MarkGate(markGate);
        std::fprintf(stderr, "[sc3:mesh] native meshes%s: %s, %zu leaves, %zu objects\n", s.mode == Mode::Check ? " (checked against the lifted programs)" : "",
                     path, s.pack.files().size(), s.pack.objectCount());
    }
}
