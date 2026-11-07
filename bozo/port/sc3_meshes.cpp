// SC3's 3D parts in plain C++: the hooks at the game's per-part calls find each part in the mesh pack and shade it under the
// header and material the game sends (the microprograms' arithmetic), and the draw joins the native ordering table where the
// game links the part's packet. RECOMP_NOTES "Native meshes". No VIF or GIF packet is read or needed: there is no VU1 path.
#include "sc3_draw.h"
#include "sc3_meshes.h"
#include "sc3_textures.h"

#include "bozo/render/mesh_pack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace sc3
{
    namespace meshes
    {
    namespace
    {
        using namespace sc3::draw;
        using namespace sc3::fp;
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kFileRequest = 0x00150418u;
        constexpr uint32_t kCdRead = 0x00424E28u;
        constexpr uint32_t kRawPart = 0x0010A178u;
        constexpr uint32_t kRawPartTail = 0x00109990u;
        constexpr uint32_t kChainPart = 0x0010A4E8u;
        constexpr uint32_t kPacketFlush = 0x0010F8B8u;
        constexpr uint32_t kRegisterPacket = 0x001276E8u;
        constexpr uint32_t kOtInsertDepth = 0x0010D9A0u;
        constexpr uint32_t kOtInsertFixed = 0x0010DC90u;
        constexpr uint32_t kClipper = 0x0010B970u;
        constexpr uint32_t kClipEmit = 0x0010B7E0u;
        constexpr uint32_t kClipVertexStride = 0x50u;
        constexpr uint32_t kRelocate = 0x0014A7D8u;
        constexpr uint32_t kCopy = 0x0012FE98u;
        constexpr uint32_t kModelCopyMin = 0x1000u;
        constexpr uint32_t kScratchpad = 0x70000000u;
        constexpr uint32_t kSector = 2048u;
        constexpr uint32_t kRawPartKind = 1u;
        constexpr uint32_t kChainPartKind = 2u;
        constexpr uint32_t kVuBlockKind = 3u;
        constexpr uint32_t kTagRegs = 0x100u;
        constexpr uint32_t kSkipPartList = 0x0C290019u;

        constexpr uint32_t kFlagProgramA = 0x40000u, kFlagRaw = 0x20000u, kFlagShineA = 0x1000000u, kFlagShineB = 0x2000000u;
        constexpr uint32_t kFlagLights = 0x800000u, kFlagNoLights = 0x8000000u, kFlagLightRows = 0x20000000u, kFlagColourScale = 0x400000u;

        namespace packs = bozo::render::packs;

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

        // A part's draw, in the Renderer's terms.
        struct Sc3Draw
        {
            bozo::render::Material material;
            std::vector<bozo::render::Vertex> vertices;
            std::shared_ptr<const bozo::render::Texture> texture;
            bool secondContext = false;
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
            uint64_t chainParts = 0, chainNative = 0, chainBatches = 0, headers = 0, bound = 0, boundBad = 0, copies = 0, kicks = 0, drawsSubmitted = 0;
            uint64_t inheritedAlpha = 0, inheritedTest = 0, unknownAlpha = 0, unknownTest = 0;
            uint64_t clipped = 0, clippedNative = 0, clippedToVu1 = 0, clippedDropped = 0;
            std::map<uint32_t, uint64_t> routinesLeft;
            std::map<std::string, uint64_t> refused; // "<routine> <reason>"
            std::map<uint64_t, uint64_t> textureMisses;
        };

        struct State
        {
            uint8_t *rdram = nullptr, *scratchpad = nullptr;
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
            // The raw parts and chain lists whose GIF tags 0x14A7D8 cleared PRIM.FGE in when it relocated their model.
            std::set<uint32_t> noFog;
            uint64_t gameAlpha = 0, gameTest = 0;
            bool gameAlphaKnown = false, gameTestKnown = false;
            struct Clip
            {
                uint32_t flags = 0, address = 0, qwc = 0;
                bool valid = false;
            } clip;
            std::map<uint32_t, uint32_t> listBases; // SC3_MESH_TRACE_LISTS: a chain list -> its model's relocation
            std::set<uint32_t> tracedLists;
            struct Copy
            {
                packs::LoadedImages::Image image; // base: where the leaf's start would be
                uint32_t to = 0, size = 0;
            };
            std::vector<Copy> copies; // copies of a loaded leaf's model (0x12FE98's), newest last
            std::vector<size_t> unflushed; // indices into the draws since the last packet flush
            std::vector<Sc3Draw> pendingDraws;
            Stats stats;
            uint64_t frames = 0;
        };

        std::optional<State> g_owner{std::in_place};
        State &g_state = *g_owner;

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (address == kRawPart)
                func_0x10a178(rdram, ctx, runtime);
            else if (address == kRawPartTail)
                func_0x109990(rdram, ctx, runtime);
            else if (address == kChainPart)
                func_0x10a4e8(rdram, ctx, runtime);
            else if (address == kRelocate)
                func_0x14a7d8(rdram, ctx, runtime);
            else if (address == kClipper)
                func_0x10b970(rdram, ctx, runtime);
            else
                func_0x10b7e0(rdram, ctx, runtime);
        }

        void packetFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        void queueDraw(Sc3Draw &&draw);

        void dropCopies(uint32_t address, uint32_t size)
        {
            auto &copies = g_state.copies;
            copies.erase(std::remove_if(copies.begin(), copies.end(), [&](const State::Copy &c) { return c.to < address + size && address < c.to + c.size; }), copies.end());
        }

        bool findImage(uint32_t address, packs::LoadedImages::Image &image)
        {
            const auto &copies = g_state.copies;
            for (size_t i = copies.size(); i-- > 0;)
                if (address >= copies[i].to && address - copies[i].to < copies[i].size)
                {
                    image = copies[i].image;
                    return true;
                }
            return g_state.loaded.find(address, image);
        }

        bool g_skinning = true; // SC3_NATIVE_SKINNING=0: program B's chain parts left to the lifted programs

        uint32_t peek32(uint32_t address)
        {
            uint32_t value = 0;
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(&value, g_state.scratchpad + (address & 0x3FFCu), 4);
            else
                std::memcpy(&value, g_state.rdram + (address & kRamMask & ~3u), 4);
            return value;
        }

        void poke32(uint32_t address, uint32_t value)
        {
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(g_state.scratchpad + (address & 0x3FFCu), &value, 4);
            else
                std::memcpy(g_state.rdram + (address & kRamMask & ~3u), &value, 4);
        }

        uint32_t sp32(uint32_t offset) { return peek32(kScratchpad + offset); }
        void spSet(uint32_t offset, uint32_t value) { poke32(kScratchpad + offset, value); }

        void setReg(R5900Context *ctx, int reg, uint32_t value)
        {
            setRegU32(ctx, reg, value);
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
                    dropCopies(base, leaf->size);
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

        // memcpy: a model the game copies out of a loaded leaf (character select's, creation's parts with the size rounded up past
        // the leaf's end) is found at the copy.
        void copy(R5900Context *ctx)
        {
            State &s = g_state;
            const uint32_t to = getRegU32(ctx, 4) & kRamMask, from = getRegU32(ctx, 5) & kRamMask, size = getRegU32(ctx, 6);
            packs::LoadedImages::Image image;
            if (size >= kModelCopyMin && size < 0x2000000u && findImage(from, image) && image.file && from >= image.base && from < image.base + image.size)
            {
                const uint32_t kept = std::min(size, image.base + image.size - from);
                s.noFog.erase(s.noFog.lower_bound(to), s.noFog.lower_bound(to + size));
                dropCopies(to, size);
                State::Copy bound;
                bound.image = image;
                bound.image.base = to - (from - image.base);
                bound.to = to;
                bound.size = kept;
                s.copies.push_back(bound);
                ++s.stats.copies;
            }
        }

        void fileRequest(R5900Context *ctx)
        {
            State &s = g_state;
            if (getRegU32(ctx, 5) == 1u && s.olkLbn < 0)
            {
                s.requestDest = getRegU32(ctx, 8);
                s.requestPos = getRegU32(ctx, 6);
                s.requestPending = true;
            }
        }

        void cdReadBefore(R5900Context *ctx)
        {
            State &s = g_state;
            const uint32_t lbn = getRegU32(ctx, 4), sectors = getRegU32(ctx, 5), buffer = getRegU32(ctx, 6) & kRamMask;
            if (s.olkLbn < 0 && s.requestPending && buffer == (s.requestDest & kRamMask) && (s.requestPos % kSector) == 0u)
                s.olkLbn = static_cast<int64_t>(lbn) - s.requestPos / kSector;
        }

        void cdReadAfter(R5900Context *ctx)
        {
            State &s = g_state;
            const uint32_t lbn = getRegU32(ctx, 4), sectors = getRegU32(ctx, 5), buffer = getRegU32(ctx, 6) & kRamMask;
            if (s.olkLbn >= 0 && lbn >= s.olkLbn && sectors != 0u)
                noteRead(buffer, static_cast<uint64_t>(lbn - s.olkLbn) * kSector, sectors * kSector);
        }

        void relocate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            if (std::getenv("SC3_MESH_TRACE_LISTS"))
            {
                const uint32_t offset = getRegU32(ctx, 6);
                for (uint32_t at = getRegU32(ctx, 4) & kRamMask, n = 0; n < 65536u; ++n)
                {
                    const uint32_t word = peek32(at);
                    const uint32_t command = word & 0xFFu, size = (word >> 8) & 0xFFu;
                    if (command == 0u || size == 0u)
                        break;
                    if (command == 5u)
                        s.listBases[(peek32(at + 4u) + offset) & kRamMask] = offset & kRamMask;
                    at += size;
                }
            }
            if ((getRegU32(ctx, 7) & 2u) == 0u && (getRegU32(ctx, 9) & 1u) != 0u)
            {
                const uint32_t offset = getRegU32(ctx, 6);
                for (uint32_t at = getRegU32(ctx, 4) & kRamMask, n = 0; n < 65536u; ++n)
                {
                    const uint32_t word = peek32(at);
                    const uint32_t command = word & 0xFFu, size = (word >> 8) & 0xFFu;
                    if (command == 0u || size == 0u)
                        break;
                    if (command == 2u)
                        s.noFog.insert((peek32(at + 0x14u) + offset) & kRamMask);
                    else if (command == 5u)
                        s.noFog.insert((peek32(at + 4u) + offset) & kRamMask);
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
            if (!findImage(address & kRamMask, image) || !image.file)
                return false;
            const packs::RegisterBlock *block = image.file->block((address & kRamMask) - image.base);
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
            // A:0x450 / A:0xF68: a vertex also gets bit 15 when the triangle it ends faces away (as B:0x15E8).
            bool cull = false;
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
            case 0x450u: shade = {true, true, false, false, false, true}; return true;
            case 0xF68u: shade = {false, true, false, false, false, true}; return true;
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

        // Back faces as the culling routines find them: the screen-space cross product (P1 - P0) x (P2 - P0) of the triangle
        // vertex k ends, negative in z, the winding alternating along the strip and starting reversed when asked.
        struct BackFaces
        {
            float screen[3][2] = {};
            bool reversed = false;
            uint32_t next(float x, float y, size_t k)
            {
                screen[0][0] = screen[1][0], screen[0][1] = screen[1][1];
                screen[1][0] = screen[2][0], screen[1][1] = screen[2][1];
                screen[2][0] = x, screen[2][1] = y;
                const float a[2] = {screen[1][0] - screen[0][0], screen[1][1] - screen[0][1]};
                const float b[2] = {screen[2][0] - screen[0][0], screen[2][1] - screen[0][1]};
                const bool odd = ((k & 1u) != 0u) != reversed;
                const float z = odd ? b[0] * a[1] - a[0] * b[1] : a[0] * b[1] - b[0] * a[1];
                return std::signbit(z) ? 0x8000u : 0u;
            }
        };

        void shade(const Header &h, Shade mode, const uint32_t tag[4], const std::vector<In> &in, std::vector<Out> &out)
        {
            if (mode.environment)
            {
                shadeEnvironment(h, mode.fog, in, out);
                return;
            }
            out.resize(in.size());
            bool outside[3] = {false, false, false};
            BackFaces faces;
            faces.reversed = (tag[3] & 1u) != 0u;
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
                const uint32_t clipBit = (clipped ? 0x8000u : 0u) | (mode.cull ? faces.next(xs, ys, k) : 0u);
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
            tris.reserve(out.size() * 3u);
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
                v.fog = static_cast<float>((o.w >> 4) & 0xFFu) / 255.0f;
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

        // ---- the part hooks

        uint32_t setupOf(uint32_t flags)
        {
            if (flags & kFlagProgramA)
                return (flags & kFlagShineA) ? 0x1C80u : (flags & kFlagShineB) ? 0x2098u : 0x10u;
            return (flags & kFlagShineA) ? 0x1990u : (flags & kFlagShineB) ? 0x21E0u : 0x10u;
        }

        uint32_t usedSize(uint32_t size, uint32_t log2)
        {
            const uint32_t whole = 1u << std::min<uint32_t>(log2, 10u);
            return size != 0u && size < whole ? size : whole;
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


        struct PartSource
        {
            uint32_t file = 0, offset = 0, object = 0;
            bool noFog = false;
        };

        bool fogCleared(uint32_t address) { return g_state.noFog.count(address & kRamMask) != 0u; }

        // FOGCOL as 0x10AB10 writes it: the scratchpad's three floats times 255.
        uint32_t fogColour()
        {
            uint32_t rgb = 0;
            for (uint32_t i = 0; i < 3u; ++i)
                rgb |= (static_cast<uint32_t>(ps2FpuCvtWS(ps2_fmul(fp::flt(sp32(0x1D0u + i * 4u)), 255.0f))) & 0xFFu) << (i * 8u);
            return rgb;
        }

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
            material.prim = decodePrim(prim);
            if (material.prim.tme)
                material.tex0 = decodeTex0(s.material.tex0);
            material.tex1 = s.material.tex1;
            material.clamp = s.material.clamp;
            if (material.prim.tme)
            {
                const auto used = [](uint32_t size, uint32_t log2)
                {
                    const uint32_t whole = 1u << std::min<uint32_t>(log2, 10u);
                    return size != 0u && size < whole ? size : whole;
                };
                const uint32_t width = used(static_cast<uint32_t>(s.material.tagRegs >> 32) & 0xFFFFu, material.tex0.tw);
                const uint32_t height = used(static_cast<uint32_t>(s.material.tagRegs >> 48) & 0xFFFFu, material.tex0.th);
                material.texture = textures::atHook(s.material.tex0, width, height, source.object & kRamMask);
                if (!material.texture)
                {
                    ++s.stats.noTexture;
                    ++s.stats.textureMisses[s.material.tex0 & ((0x3FFFull) | (0x3Full << 20) | (0x3FFFull << 37))];
                    return false;
                }
            }
            std::vector<Out> own;
            const std::vector<Out> *out = shaded;
            if (!out)
            {
                shade(s.header, mode, tag, in, own);
                out = &own;
            }
            material.alpha = s.material.setsAlpha ? s.material.alpha : s.gameAlphaKnown ? s.gameAlpha : material.alpha;
            material.test = s.material.setsTest ? s.material.test : s.gameTestKnown ? s.gameTest : material.test;
            if (!s.material.setsAlpha)
                ++(s.gameAlphaKnown ? s.stats.inheritedAlpha : s.stats.unknownAlpha);
            if (!s.material.setsTest)
                ++(s.gameTestKnown ? s.stats.inheritedTest : s.stats.unknownTest);
            std::vector<GsVertex> triangles;
            toTriangles(prim, *out, triangles);
            s.stats.triangles += triangles.size() / 3u;
            material.prim.type = GS_PRIM_TRIANGLE;
            toRenderer(material, triangles.data(), static_cast<uint32_t>(triangles.size()), draw.material, draw.vertices);
            draw.texture = material.texture;
            draw.secondContext = material.prim.ctxt;
            draw.material.fogColour = fogColour();
            static const uint64_t dumpFrame = std::getenv("SC3_MESH_DUMP") ? std::strtoull(std::getenv("SC3_MESH_DUMP"), nullptr, 10) : ~0ull;
            if (s.frames == dumpFrame && !draw.vertices.empty())
            {
                float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f, q0 = 1e9f, q1 = -1e9f;
                for (const bozo::render::Vertex &v : draw.vertices)
                    u0 = std::min(u0, v.u), u1 = std::max(u1, v.u), v0 = std::min(v0, v.v), v1 = std::max(v1, v.v), q0 = std::min(q0, v.q), q1 = std::max(q1, v.q);
                uint64_t sum[4] = {};
                if (draw.texture)
                    for (uint32_t texel : draw.texture->rgba)
                        for (int c = 0; c < 4; ++c)
                            sum[c] += (texel >> (c * 8)) & 0xFFu;
                const size_t texels = draw.texture && !draw.texture->rgba.empty() ? draw.texture->rgba.size() : 1u;
                std::fprintf(stderr, "[sc3:mesh] dump object %#x file %s avg %llu %llu %llu %llu key %#llx\n", source.object, s.pack.files().size() > source.file ? s.pack.files()[source.file].name.c_str() : "?",
                             static_cast<unsigned long long>(sum[0] / texels), static_cast<unsigned long long>(sum[1] / texels), static_cast<unsigned long long>(sum[2] / texels), static_cast<unsigned long long>(sum[3] / texels),
                             static_cast<unsigned long long>(draw.texture ? draw.texture->key : 0u));
                std::fprintf(stderr, "[sc3:mesh] dump routine %#x prim %#x tex0 %#llx tw %u th %u psm %#x tex %ux%u clamp %#llx tex1 %#llx alpha %#llx test %#llx u %g..%g v %g..%g q %g..%g verts %zu\n", routine, prim,
                             static_cast<unsigned long long>(s.material.tex0), material.tex0.tw, material.tex0.th, material.tex0.psm, draw.texture ? draw.texture->width : 0u, draw.texture ? draw.texture->height : 0u,
                             static_cast<unsigned long long>(material.clamp), static_cast<unsigned long long>(material.tex1), static_cast<unsigned long long>(material.alpha), static_cast<unsigned long long>(material.test),
                             u0, u1, v0, v1, q0, q1, draw.vertices.size());
            }
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
            // B:0x910: the colour times h12 with no lights, the texture coordinates scrolled by h4; B:0x1A40: B:0x1CA8's
            // environment map. Both fogged: the guard's w clamped (h16.x / h15.y, or h9.x / h9.y) in place of vi3. B:0xB78:
            // B:0x910 with no fog.
            bool fog = false, unlit = false;
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
            case 0x910u: k = {false, false, false, false, true, true}; return true;
            case 0xB78u: k = {false, false, false, false, false, true}; return true;
            case 0x1A40u: k = {false, false, false, true, true, false}; return true;
            default: return false;
            }
        }

        uint32_t fogWord(float guard, float most, float least)
        {
            return static_cast<uint32_t>(ftoi(std::max(std::min(guard, most), least), 16.0f)) & 0xFFFFu;
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
            out.reserve(static_cast<size_t>(count));
            uint32_t q = 1u;
            bool outside[3] = {false, false, false};
            const uint32_t vi3 = (kind.environment ? h.q[9][3] : h.q[17][3]) & 0xFFFFu;
            const bool reversed = (tag[3] & 0xFFFFu) != 0u;
            float screen[3][2] = {};
            const auto finish = [&](Out &o, bool clipped, uint32_t flags, const float *position, int k, uint32_t vi3)
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
                    const float guarded[4] = {position[0], position[1], position[2], p[3]};
                    float g[4];
                    for (int i = 0; i < 4; ++i)
                        g[i] = sat(static_cast<double>(sat(static_cast<double>(h.f(7, i)) * h.f(8, i))) + static_cast<double>(sat(static_cast<double>(guarded[i]) * h.f(8, i))));
                    outside[k % 3] = std::fabs(g[0]) > 1.0f || std::fabs(g[1]) > 1.0f || std::fabs(g[2]) > 1.0f;
                    const bool clipped = outside[0] || outside[1] || outside[2];
                    for (int i = 0; i < 4; ++i)
                        o.rgba[i] = static_cast<int32_t>(h.q[10][i]);
                    for (int i = 0; i < 3; ++i)
                        o.xyz[i] = ftoi(position[i], 16.0f);
                    o.s = (h.f(13, 0) + r[0] * h.f(12, 0)) * qv;
                    o.t = (h.f(13, 1) + r[1] * h.f(12, 1)) * qv;
                    o.q = kind.cull ? qv : (h.f(13, 2) + r[2] * h.f(12, 2)) * qv;
                    finish(o, clipped, flags, position, k, kind.fog ? fogWord(g[3], h.f(9, 0), h.f(9, 1)) : vi3);
                    continue;
                }
                if (kind.unlit)
                {
                    for (int i = 0; i < 4; ++i)
                        o.rgba[i] = ftoi(colour[i] * h.f(12, i), 1.0f);
                }
                else if (kind.highlight)
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
                float g[4];
                for (int i = 0; i < 4; ++i)
                    g[i] = sat(static_cast<double>(sat(static_cast<double>(h.f(13, i)) * h.f(14, i))) + static_cast<double>(sat(static_cast<double>(position[i]) * h.f(14, i))));
                outside[k % 3] = std::fabs(g[0]) > 1.0f || std::fabs(g[1]) > 1.0f || std::fabs(g[2]) > 1.0f;
                const bool clipped = outside[0] || outside[1] || outside[2];
                for (int i = 0; i < 3; ++i)
                    o.xyz[i] = ftoi(position[i], 16.0f);
                o.s = (kind.unlit ? h.f(4, 0) + st[0] * h.f(18, 0) : st[0] * h.f(18, 0)) * qv;
                o.t = (kind.unlit ? h.f(4, 1) + st[1] * h.f(18, 1) : st[1] * h.f(18, 1)) * qv;
                o.q = qv;
                finish(o, clipped, flags, position, k, kind.fog ? fogWord(g[3], h.f(16, 0), h.f(15, 1)) : vi3);
            }
            return true;
        }

        void queueDraw(Sc3Draw &&draw)
        {
            State &s = g_state;
            s.unflushed.push_back(s.pendingDraws.size());
            s.pendingDraws.push_back(std::move(draw));
        }

        // The part's place: its scratchpad packet is linked by the game after it, and only then is it known where the draw
        // goes. A CNT stands for the part's data so that the game's flush has a packet to move.
        void place(uint8_t *rdram, R5900Context *ctx, Runtime *runtime, std::vector<Sc3Draw> &&draws, bool part)
        {
            State &s = g_state;
            if (part)
            {
                spSet(0x384u, 0u);
                spSet(0x368u, 0u);
                spSet(0x350u, 0u);
                spSet(0x36Cu, 0u);
            }
            const uint32_t ra = getRegU32(ctx, 31);
            const uint32_t qwords = static_cast<uint32_t>(draws.size());
            if (sp32(0x374u) + sp32(0x18u) - 0x10u < sp32(0x378u) + qwords * 16u)
            {
                setReg(ctx, 4, sp32(0x378u));
                packetFlush(rdram, ctx, runtime);
                const uint32_t start = sp32(0x0u);
                spSet(0x374u, start);
                spSet(0x378u, start);
            }
            uint32_t cursor = sp32(0x378u);
            for (Sc3Draw &draw : draws)
            {
                poke32(cursor, 0x10000000u);
                poke32(cursor + 4u, 0u);
                poke32(cursor + 8u, 0u);
                poke32(cursor + 12u, 0u);
                cursor += 16u;
                queueDraw(std::move(draw));
            }
            spSet(0x378u, cursor);
            ctx->pc = ra;
        }

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

        const char *refusal(uint32_t flags, Shade &mode, bool header)
        {
            const State &s = g_state;
            if (!(flags & kFlagProgramA))
                return "program B";
            if (!routineShade(flags & 0xFFFFu, mode))
                return "routine not written";
            if (header && setupOf(flags) != (mode.environment ? 0x1C80u : 0x10u))
                return "setup";
            if (sp32(0x350u) != 0u)
                return "bones sent";
            if (!s.header.valid)
                return "no header";
            if (s.header.shine != mode.environment)
                return "header layout";
            return nullptr;
        }

        void noteRefusal(uint32_t flags, const char *why)
        {
            char key[64];
            std::snprintf(key, sizeof(key), "%c:%#x/%#x %s", (flags & kFlagProgramA) ? 'A' : 'B', flags & 0xFFFFu, setupOf(flags), why);
            ++g_state.stats.refused[key];
        }

        bool drawRawPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t address = getRegU32(ctx, 4) & kRamMask, qwc = getRegU32(ctx, 5), flags = getRegU32(ctx, 6);
            const uint32_t routine = flags & 0xFFFFu;
            ++s.stats.parts;
            Shade mode;
            const bool header = sp32(0x368u) != 0u, materialSent = sp32(0x36Cu) != 0u;
            noteState(flags, routine >> 3);
            noteBones();
            if (false)
                std::fprintf(stderr, "[sc3:mesh] trace part %#x qwc %u flags %#x ra %#x 368 %#x 36c %#x 384 %#x/%#x 350 %#x\n", address, qwc, flags,
                             getRegU32(ctx, 31), sp32(0x368u), sp32(0x36Cu), sp32(0x384u), sp32(0x388u), sp32(0x350u));
            if (const char *why = refusal(flags, mode, header))
            {
                noteRefusal(flags, why);
                ++s.stats.unsupported;
                ++s.stats.routinesLeft[((flags & kFlagProgramA) ? 0u : 0x10000u) | routine];
                return false;
            }
            packs::LoadedImages::Image image;
            const packs::MeshObject *object = nullptr;
            if (findImage(address, image) && image.file)
                object = image.file->object(address - image.base);
            if (!object || object->kind != kRawPartKind || object->attribute<uint32_t>(0) != qwc * 16u)
            {
                ++s.stats.notInPack;
                if (std::getenv("SC3_MESH_TRACE_LISTS") && s.tracedLists.insert(address).second)
                    std::fprintf(stderr, "[sc3:mesh] raw part not in the pack: %#x %s +%#x qwc %u flags %#x ra %#x object %s kind %u size %u\n", address, image.file ? image.file->name.c_str() : "(no leaf)", address - image.base, qwc, flags,
                                 getRegU32(ctx, 31), object ? "yes" : "no", object ? object->kind : 0u, object ? object->attribute<uint32_t>(0) : 0u);
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
            ++s.stats.native;
            std::vector<Sc3Draw> draws;
            draws.push_back(std::move(draw));
            place(rdram, ctx, runtime, std::move(draws), true);
            return true;
        }

        void rawPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (drawRawPart(rdram, ctx, runtime))
                return;
            ++g_state.stats.leftToVu1;
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
            s.clip.address = peek32(entry + 4u) & kRamMask;
            s.clip.qwc = peek32(entry + 8u);
            s.clip.valid = s.clip.flags == getRegU32(ctx, 4);
            callOriginal(kClipper, rdram, ctx, runtime);
        }

        bool drawClipped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t vertices = getRegU32(ctx, 4), count = getRegU32(ctx, 5), flags = getRegU32(ctx, 7);
            const uint32_t routine = flags & 0xFFFFu;
            Shade mode;
            if (!s.clip.valid || flags != s.clip.flags || !(flags & kFlagProgramA) || !routineShade(routine, mode) ||
                !s.header.valid || s.header.shine != mode.environment)
                return false;
            packs::LoadedImages::Image image;
            const packs::MeshObject *object = nullptr;
            if (findImage(s.clip.address, image) && image.file)
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
            place(rdram, ctx, runtime, std::move(draws), false);
            return true;
        }

        void clipEmit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t ra = getRegU32(ctx, 31);
            ++s.stats.clipped;
            if (drawClipped(rdram, ctx, runtime))
            {
                ++s.stats.clippedNative;
                return;
            }
            ++s.stats.clippedDropped;
            ctx->pc = ra;
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
        bool drawSkinnedPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t list, uint32_t flags, bool header, bool materialSent)
        {
            State &s = g_state;
            const bool bonesSent = sp32(0x350u) != 0u;
            const uint32_t routine = flags & 0xFFFFu;
            Skinning kind;
            const char *why = !skinRoutine(routine, kind) ? "routine not written" : (flags & kFlagShineB) || kind.environment != ((flags & kFlagShineA) != 0u) ? "shine flags"
                              : header && setupOf(flags) != (kind.environment ? 0x1990u : 0x10u) ? "setup"
                              : !s.header.valid ? "no header" : s.header.shine != kind.environment ? "header layout" : nullptr;
            if (why)
            {
                noteRefusal(flags, why);
                return false;
            }
            packs::LoadedImages::Image image;
            const packs::MeshObject *chain = nullptr;
            if (findImage(list, image) && image.file)
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
                if (!drawVertices(routine, Shade{}, tag, {}, PartSource{image.index, entry[0], image.base + entry[0], fogCleared(list)}, draws[i], &out))
                    return false;
            }
            ++s.stats.chainNative;
            s.stats.chainBatches += n;
            place(rdram, ctx, runtime, std::move(draws), true);
            return true;
        }

        // A chain part (0x10A4E8): the list the game CALLs, a REF to a block of VU data and an MSCAL of the part's routine per
        // batch, from the pack. A program A part whose blocks are all rigid parts the pack knows is drawn like raw parts, one
        // draw a batch.
        bool drawChainPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t list, uint32_t flags, bool header, bool materialSent)
        {
            State &s = g_state;
            const uint32_t routine = flags & 0xFFFFu;
            if (!(flags & kFlagProgramA))
                return drawSkinnedPart(rdram, ctx, runtime, list, flags, header, materialSent);
            Shade mode;
            if (const char *why = refusal(flags, mode, header))
            {
                noteRefusal(flags, why);
                return false;
            }
            packs::LoadedImages::Image image;
            const packs::MeshObject *chain = nullptr;
            if (findImage(list, image) && image.file)
                chain = image.file->object(list - image.base);
            const packs::MeshSection *batches = chain && chain->kind == kChainPartKind ? chain->section(packs::fourcc("SC3C")) : nullptr;
            if (!batches || batches->size < 8u)
            {
                ++s.stats.chainNotInPack;
                if (std::getenv("SC3_MESH_TRACE_LISTS") && s.tracedLists.insert(list).second)
                {
                    const auto base = s.listBases.find(list);
                    std::fprintf(stderr, "[sc3:mesh] chain list not in the pack: %#x %s +%#x%s, model base %s%#x, chain object %s kind %u\n", list, image.file ? image.file->name.c_str() : "(no leaf)", list - image.base,
                                 chain ? " (object)" : "", base == s.listBases.end() ? "unknown " : "+", base == s.listBases.end() ? 0u : base->second - image.base, chain ? "yes" : "no", chain ? chain->kind : 0u);
                }
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
            ++s.stats.chainNative;
            s.stats.chainBatches += parts.size();
            place(rdram, ctx, runtime, std::move(draws), true);
            return true;
        }

        void chainPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t list = getRegU32(ctx, 4) & kRamMask, flags = getRegU32(ctx, 5);
            ++s.stats.chainParts;
            if (false)
                std::fprintf(stderr, "[sc3:mesh] trace chain %#x flags %#x 368 %#x 36c %#x 384 %#x/%#x 350 %#x\n", list, flags,
                             sp32(0x368u), sp32(0x36Cu), sp32(0x384u), sp32(0x388u), sp32(0x350u));
            const bool header = sp32(0x368u) != 0u, materialSent = sp32(0x36Cu) != 0u;
            noteState(flags, (flags & 0xFFFFu) >> 3);
            noteBones();
            if (drawChainPart(rdram, ctx, runtime, list, flags, header, materialSent))
                return;
            ++s.stats.routinesLeft[((flags & kFlagProgramA) ? 0u : 0x10000u) | (flags & 0xFFFFu)];
            ++s.stats.leftToVu1;
            callOriginal(kChainPart, rdram, ctx, runtime);
        }

        // ZBUF_1's ZMSK where the game's list writes it (0x1276E8, in the VIF1 list), for the draws played after it.
        bool g_depthMasked = false;

        void registerPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t reg = getRegU32(ctx, 4);
            const uint64_t value = getRegU32(ctx, 5) | (static_cast<uint64_t>(ctx->r[5].uw[1]) << 32);
            func_0x1276e8(rdram, ctx, runtime);
            if (reg == 0x4Eu)
            {
                const bool masked = ((value >> 32) & 1u) != 0u;
                ot::appendVif(rdram, [masked](bozo::render::Renderer &) { g_depthMasked = masked; });
            }
        }

        ot::Draw asDraw(Sc3Draw &&part)
        {
            auto held = std::make_shared<Sc3Draw>(std::move(part));
            return [held](bozo::render::Renderer &renderer)
            {
                if (held->vertices.empty())
                    return;
                held->material.depthWrite = !held->secondContext && !g_depthMasked;
                if (held->texture)
                    renderer.draw(held->material, kIdentity, held->vertices.data(), static_cast<uint32_t>(held->vertices.size()), held->texture);
                else
                    renderer.draw(held->material, kIdentity, held->vertices.data(), static_cast<uint32_t>(held->vertices.size()));
            };
        }

        // Where the scratchpad packet the parts went into is linked: the VIF1 list in order, or an ordering table bucket.
        void packetFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            for (size_t i : s.unflushed)
                if (ot::appendVif(rdram, asDraw(std::move(s.pendingDraws[i]))))
                    ++s.stats.drawsSubmitted;
            s.unflushed.clear();
            s.pendingDraws.clear();
            func_0x10f8b8(rdram, ctx, runtime);
        }

        void linkUnflushed(uint8_t *rdram, int bucket)
        {
            State &s = g_state;
            for (size_t i : s.unflushed)
                if (ot::linkLayerBucket(rdram, bucket, asDraw(std::move(s.pendingDraws[i]))))
                    ++s.stats.drawsSubmitted;
            s.unflushed.clear();
            s.pendingDraws.clear();
        }

        float otScale()
        {
            const uint32_t bits = peek32(0x005BE700u + 0x34u);
            float scale;
            std::memcpy(&scale, &bits, 4);
            return scale;
        }

        void otInsertDepth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            textures::noteDepthLink(ctx);
            const float z = ctx->f[12];
            if (!(z < 0.0f || z > 12.5f))
                linkUnflushed(rdram, ps2FpuCvtWS(ps2_fmul(std::min(ps2_fmul(z, 0.12f), 1.0f), otScale())));
            else
            {
                s.unflushed.clear();
                s.pendingDraws.clear();
            }
            packet::closeDepth(rdram, ctx, runtime);
        }

        void otInsertFixed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float z = ps2_fmul(ctx->f[12], 0.12f);
            linkUnflushed(rdram, ps2FpuCvtWS(z < 1.0f ? ps2_fmul(z, otScale()) : otScale()));
            packet::closeFixed(rdram, ctx, runtime);
        }
        void report()
        {
            State &s = g_state;
            if (++s.frames % 250u != 0u)
                return;
            Stats &t = s.stats;
            std::string left, miss;
            char item[96];
            for (const auto &[routine, n] : t.routinesLeft)
            {
                std::snprintf(item, sizeof(item), " %c:%#x x%llu", routine >= 0x10000u ? 'B' : 'A', routine & 0xFFFFu, static_cast<unsigned long long>(n));
                left += item;
            }
            for (const auto &[why, n] : t.refused)
            {
                std::snprintf(item, sizeof(item), "; %s x%llu", why.c_str(), static_cast<unsigned long long>(n));
                left += item;
            }
            for (const auto &[tex0, n] : t.textureMisses)
            {
                std::snprintf(item, sizeof(item), " tbp %#x psm %#x cbp %#x x%llu", static_cast<unsigned>(tex0 & 0x3FFFu), static_cast<unsigned>((tex0 >> 20) & 0x3Fu),
                              static_cast<unsigned>((tex0 >> 37) & 0x3FFFu), static_cast<unsigned long long>(n));
                miss += item;
            }
            std::fprintf(stderr, "[sc3:mesh] 250 frames: parts %llu native %llu (triangles %llu), chain parts %llu native %llu, LOST %llu (not in the pack %llu, chain %llu, no material %llu, no texture %llu, routine %llu); clipped %llu native %llu dropped %llu; drawn %llu; leaves bound %llu (not the pack's %llu), copies %llu;%s;%s\n",
                         static_cast<unsigned long long>(t.parts), static_cast<unsigned long long>(t.native), static_cast<unsigned long long>(t.triangles),
                         static_cast<unsigned long long>(t.chainParts), static_cast<unsigned long long>(t.chainNative), static_cast<unsigned long long>(t.leftToVu1),
                         static_cast<unsigned long long>(t.notInPack), static_cast<unsigned long long>(t.chainNotInPack), static_cast<unsigned long long>(t.noMaterial),
                         static_cast<unsigned long long>(t.noTexture), static_cast<unsigned long long>(t.unsupported), static_cast<unsigned long long>(t.clipped),
                         static_cast<unsigned long long>(t.clippedNative), static_cast<unsigned long long>(t.clippedDropped), static_cast<unsigned long long>(t.drawsSubmitted),
                         static_cast<unsigned long long>(t.bound), static_cast<unsigned long long>(t.boundBad), static_cast<unsigned long long>(t.copies), left.c_str(), miss.c_str());
            const uint64_t bound = t.bound, bad = t.boundBad;
            t = Stats{};
            t.bound = bound;
            t.boundBad = bad;
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

        template <void (*Hook)(uint8_t *, R5900Context *, PS2Runtime *)>
        void hooked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_state.rdram = rdram;
            g_state.scratchpad = bozo::port::scratchpad(runtime);
            Hook(rdram, ctx, runtime);
        }

        bool g_ready = false;
    }
    }

    void meshes::frameStart() { g_depthMasked = false; }

    void meshes::frameDone()
    {
        if (g_ready)
            report();
    }

    void meshes::noteRequest(R5900Context *ctx)
    {
        if (g_ready)
            fileRequest(ctx);
    }

    void meshes::noteReadBefore(uint8_t *rdram, R5900Context *ctx)
    {
        if (!g_ready)
            return;
        g_state.rdram = rdram;
        cdReadBefore(ctx);
    }

    void meshes::noteReadAfter(uint8_t *rdram, R5900Context *ctx)
    {
        if (g_ready)
            cdReadAfter(ctx);
    }

    void meshes::noteCopy(uint8_t *rdram, R5900Context *ctx)
    {
        if (!g_ready)
            return;
        g_state.rdram = rdram;
        copy(ctx);
    }

    void meshes::shutdown()
    {
        g_ready = false;
        g_owner.reset();
    }

    void meshes::install(Registry &runtime)
    {
        const char *setting = std::getenv("SC3_NATIVE_MESHES");
        const char *path = std::getenv("SC3_MESH_PACK");
        if ((setting && *setting == '0') || !path || !*path)
        {
            std::fprintf(stderr, "[sc3:mesh] no SC3_MESH_PACK: no 3D is drawn\n");
            return;
        }
        textures::install(runtime);
        if (!textures::ready())
        {
            std::fprintf(stderr, "[sc3:mesh] no texture pack (SC3_TEXTURE_PACK): no 3D is drawn\n");
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
        g_ready = true;
        runtime.registerFunction(kRawPart, &hooked<rawPart>);
        runtime.registerFunction(kRawPartTail, &hooked<rawPartTail>);
        runtime.registerFunction(kChainPart, &hooked<chainPart>);
        runtime.registerFunction(kRelocate, &hooked<relocate>);
        runtime.registerFunction(kClipper, &hooked<clipper>);
        runtime.registerFunction(kClipEmit, &hooked<clipEmit>);
        runtime.registerFunction(kPacketFlush, &hooked<packetFlush>);
        runtime.registerFunction(kRegisterPacket, &hooked<registerPacket>);
        runtime.registerFunction(kOtInsertDepth, &hooked<otInsertDepth>);
        runtime.registerFunction(kOtInsertFixed, &hooked<otInsertFixed>);
        std::fprintf(stderr, "[sc3:mesh] native meshes: %s, %zu leaves, %zu objects\n", path, s.pack.files().size(), s.pack.objectCount());
    }
}
