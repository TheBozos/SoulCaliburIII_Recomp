#include "sc3_2d.h"
#include "runtime/native/native_texture_pack.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_registers.h"
#include "runtime/native/native_gs_hook.h"
#include "runtime/native/native_mesh_pack.h"
#include "runtime/native/native_renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace sc3
{
    namespace native2d
    {
    namespace
    {
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kFileRequest = 0x00150418u;
        constexpr uint32_t kCdRead = 0x00424E28u;
        constexpr uint32_t kLabels = 0x001388E8u;
        constexpr uint32_t kTriangle = 0x00124C80u;
        constexpr uint32_t kInsertMode = 0x00468CB0u; // 0x120978's mode
        // Where 2D packets join the frame: the 2D insertion dispatcher (by the mode at 0x468CB0), fixed OT buckets, the
        // text layers' buckets, and the in-order GIF list.
        constexpr uint32_t kInserts[] = {0x00120978u, 0x0010DDF8u, 0x0010DF30u, 0x0010E068u, 0x0010E1C8u, 0x0010E368u};

        constexpr uint32_t kRenderer = 0x005BE700u;   // +0 frame not built, +0x2c heap qwords used, +0x30 heap cursor, +0x54 the OT
        constexpr uint32_t kOtLayerBase = 0x00467490u; // bucket offset of the current layer
        constexpr uint32_t kOtGroups = 0x00467488u;    // 64 bits: the 128-bucket groups with packets
        constexpr uint32_t k2d = 0x005BEDD0u;          // +0x4c the 2D texture file, +0x68 the 2D depth
        constexpr uint32_t kAlphaTable = 0x0046EFD8u;
        constexpr uint32_t kView = 0x0046DA88u;        // +0x460 the 2D view matrix
        constexpr uint32_t kScratchpad = 0x70000000u;
        constexpr uint32_t kSector = 2048u;
        constexpr uint32_t kTagRegs = 0x100u;
        constexpr uint32_t kTexelsReg = 0x101u, kPaletteReg = 0x102u; // the page's records in the 2D texture pack
        constexpr uint64_t kNoRecord = 0xFFFFFFFFu;
        constexpr uint32_t kMarkMagic = 0x44324353u;   // "SC2D", the marker tag's upper word
        constexpr int kLabelBucket = 0x1FF2;

        using Function = PS2Runtime::RecompiledFunction;
        using ps2x::native::GsMaterial;
        using ps2x::native::GsVertex;
        namespace packs = ps2x::native::packs;

        struct Leaf
        {
            std::string name;
            uint64_t offset = 0;
            uint32_t size = 0;
            uint32_t base = 0;
            uint32_t covered = 0;
            bool bound = false;
        };

        // A texture page's registers, as the offline pack recorded the game's block (TEX0, TEX1, CLAMP, used size).
        struct Page
        {
            uint64_t tex0 = 0, tex1 = 0, clamp = 0;
            uint32_t width = 0, height = 0;
            const ps2x::native::NativeTexture *texture = nullptr;
        };

        struct BuilderStats
        {
            uint64_t calls = 0, native = 0, sprites = 0, markers = 0, toGs = 0, gifQwords = 0;
            uint64_t noFile = 0, noBlock = 0, noTexture = 0;
            uint64_t checked = 0, checkSprites = 0, checkDiffs = 0, checkCountDiffs = 0;
        };

        enum class Mode
        {
            Off,
            On,
            Check, // the game's packets built and drawn as before, the C++ builders' sprites compared with them
        };

        // One sprite as a builder computes it (the words of its packet: colour, two corners' ST and XYZ).
        struct Sprite
        {
            uint32_t rgba = 0;
            float s0 = 0, t0 = 0, s1 = 0, t1 = 0;
            int32_t x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
        };

        struct State
        {
            bool installed = false;
            Mode mode = Mode::Off;
            std::string switchPath;
            PS2Runtime *runtime = nullptr;
            uint8_t *rdram = nullptr, *scratchpad = nullptr;
            std::map<uint32_t, Function> originals;
            packs::MeshPack pack;
            packs::LoadedImages loaded;
            std::vector<Leaf> leaves;
            int64_t olkLbn = -1;
            uint32_t requestDest = 0, requestPos = 0;
            bool requestPending = false;
            packs::TexturePack textures;
            struct Decoded
            {
                ps2x::native::TextureCache::Texture image;
                ps2x::native::NativeTexture native;
            };
            std::map<std::pair<uint32_t, uint32_t>, Decoded> decoded; // (pack file, page block offset)
            uint32_t nextKey = 1;
            std::atomic<uint64_t> markersRead{0};
            BuilderStats labels, triangles;
            std::map<uint64_t, uint64_t> textureMisses; // TEX0 -> draws left to the GS without a pack texture
            // Packets the game still builds for 2D, by the return address of their insertion (ra: the builder) and the
            // insertion function: calls and quadwords.
            std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>> inserted;
            uint64_t frames = 0, bound = 0, boundBad = 0;
        };

        State g_state;

        uint32_t peek32(uint32_t address)
        {
            uint32_t value = 0;
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(&value, g_state.scratchpad + (address & 0x3FFCu), 4);
            else
                std::memcpy(&value, g_state.rdram + (address & PS2_RAM_MASK & ~3u), 4);
            return value;
        }

        uint64_t peek64(uint32_t address)
        {
            uint64_t value = 0;
            std::memcpy(&value, g_state.rdram + (address & PS2_RAM_MASK & ~7u), 8);
            return value;
        }

        float peekFloat(uint32_t address)
        {
            const uint32_t bits = peek32(address);
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
        }

        uint8_t peek8(uint32_t address) { return g_state.rdram[address & PS2_RAM_MASK]; }

        void poke32(uint32_t address, uint32_t value)
        {
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(g_state.scratchpad + (address & 0x3FFCu), &value, 4);
            else
                std::memcpy(g_state.rdram + (address & PS2_RAM_MASK & ~3u), &value, 4);
        }

        void poke64(uint32_t address, uint64_t value) { std::memcpy(g_state.rdram + (address & PS2_RAM_MASK & ~7u), &value, 8); }

        void pokeFloat(uint32_t address, float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            poke32(address, bits);
        }

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_state.originals.at(address)(rdram, ctx, runtime);
        }

        void followSwitch()
        {
            State &s = g_state;
            if (s.switchPath.empty() || (s.frames & 15u) != 0u)
                return;
            std::ifstream file(s.switchPath);
            std::string value;
            if (!std::getline(file, value))
                return;
            const Mode mode = value == "check" ? Mode::Check : !value.empty() && value[0] != '0' ? Mode::On : Mode::Off;
            if (mode != s.mode)
                std::fprintf(stderr, "[sc3:2d] native 2D %s from the next frame\n", mode == Mode::On ? "on" : mode == Mode::Check ? "checked" : "off");
            s.mode = mode;
        }

        // ---- where the game loaded ROOT.OLK's leaves (the 2D pack's register blocks are keyed by leaf and offset)

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
                        ++s.bound;
                    else
                        ++s.boundBad;
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

        // ---- the game's texture files: 0x117760 (an address the file layer accepts), 0x117A38 (its upload CALLs)

        bool fileAddress(uint32_t address) { return address > 0x000FFFFFu && address <= 0x07FFFFFFu; }

        // The page `index` of a texture file (0x1177E0's choice: the last one past its count) from the 2D pack, its texture
        // the one the file's own uploads write (decoded on first use, kept for the program's life).
        bool page(uint32_t file, uint32_t index, Page &out, BuilderStats &stats)
        {
            State &s = g_state;
            if (!fileAddress(file))
            {
                ++stats.noFile;
                return false;
            }
            const uint32_t pages = peek8(file + 5u) + peek8(file + 7u) * 256u;
            if (pages == 0u)
            {
                ++stats.noFile;
                return false;
            }
            const uint32_t block = peek32(peek32(file + 0xCu) + std::min(index, pages - 1u) * 4u) & ~0xFu;
            packs::LoadedImages::Image image;
            const packs::RegisterBlock *registers = nullptr;
            if (s.loaded.find(block, image) && image.file)
                registers = image.file->block(block - image.base);
            if (!registers)
            {
                ++stats.noBlock;
                return false;
            }
            bool tex0 = false;
            uint64_t texels = kNoRecord, palette = kNoRecord;
            for (const auto &[reg, value] : registers->writes)
            {
                switch (reg & 0xFFu)
                {
                case 0x06:
                case 0x07:
                    out.tex0 = value;
                    tex0 = true;
                    break;
                case 0x14:
                case 0x15:
                    out.tex1 = value;
                    break;
                case 0x08:
                case 0x09:
                    out.clamp = value;
                    break;
                default:
                    break;
                }
                if (reg == kTexelsReg)
                    texels = value;
                if (reg == kPaletteReg)
                    palette = value;
                if (reg == kTagRegs)
                {
                    out.width = static_cast<uint32_t>((value >> 32) & 0xFFFFu);
                    out.height = static_cast<uint32_t>((value >> 48) & 0xFFFFu);
                }
            }
            if (!tex0)
            {
                ++stats.noBlock;
                return false;
            }
            const std::vector<packs::TextureRecord> *records = s.textures.file(image.file->name);
            if (records && texels < records->size())
            {
                auto [it, added] = s.decoded.try_emplace({image.index, block - image.base});
                State::Decoded &d = it->second;
                if (added)
                {
                    const packs::TextureRecord &t = (*records)[texels];
                    uint32_t colours[256] = {};
                    if (palette < records->size())
                        std::memcpy(colours, (*records)[palette].texels.data(), std::min<size_t>(sizeof(colours), (*records)[palette].texels.size()));
                    packs::decodeRecord(d.image, out.width ? out.width : t.width, out.height ? out.height : t.height, t.psm, t, colours, 0);
                    d.native.width = d.image.width;
                    d.native.height = d.image.height;
                    d.native.rgba = d.image.rgba.data();
                    d.native.key = (static_cast<uint64_t>(image.index) << 32) | (block - image.base);
                }
                if (d.native.rgba)
                {
                    out.texture = &d.native;
                    return true;
                }
            }
            ++stats.noTexture;
            ++s.textureMisses[out.tex0 & ((0x3FFFull) | (0x3Full << 20) | (0x3FFFull << 37))];
            return false;
        }

        // ---- the OT: 0x10DDF8's link of a packet into a fixed bucket, for the two quadwords a marker takes

        uint64_t markerGate(uint64_t upper)
        {
            if (static_cast<uint32_t>(upper >> 32) != kMarkMagic)
                return 0u;
            g_state.markersRead.fetch_add(1u, std::memory_order_relaxed);
            return upper;
        }

        // A CNT with no data (the GIF channel ignores the tag's upper 64 bits: the marker's key there), then a NEXT to the
        // bucket's previous head, at the frame heap's next place, linked as the bucket's new head.
        // fromLayer: a bucket of the current layer (0x10D9A0's, from +0x50), else of the whole table (0x10DDF8's, +0x54).
        bool linkMarker(uint64_t key, int bucket, bool fromLayer = false)
        {
            if (peek32(kRenderer) != 0u)
                return false;
            if (!fromLayer)
                bucket = bucket < 0 ? 0 : bucket >= 0x1FFF ? 0x1FFE : bucket;
            const uint32_t layer = (peek32(kOtLayerBase) + static_cast<uint32_t>(bucket)) >> 7;
            poke64(kOtGroups, peek64(kOtGroups) | (1ull << (layer & 63u)));
            const uint32_t entry = peek32(kRenderer + (fromLayer ? 0x50u : 0x54u)) + static_cast<uint32_t>(bucket) * 16u;
            const uint32_t head = peek32(entry + 4u);
            const uint32_t cursor = peek32(kRenderer + 0x30u);
            const uint32_t at = (cursor - 32u) & ~0x7Fu;
            poke32(kRenderer + 0x30u, at);
            poke32(kRenderer + 0x2Cu, peek32(kRenderer + 0x2Cu) + (cursor - at) / 16u);
            poke64(at, 0x10000000ull);
            poke64(at + 8u, key);
            poke64(at + 16u, 0x20000000ull | (static_cast<uint64_t>(head) << 32));
            poke64(at + 24u, 0u);
            poke32(entry + 4u, at);
            return true;
        }

        int32_t truncate(float v)
        {
            if (!(v == v))
                return 0;
            if (v >= 2147483648.0f)
                return std::numeric_limits<int32_t>::max();
            if (v <= -2147483648.0f)
                return std::numeric_limits<int32_t>::min();
            return static_cast<int32_t>(v);
        }

        // 0x120978 (end, start, qwc, depth f12) for a marker: by the mode word, 1 the depth-sorted layer bucket (0x10D9A0:
        // nothing outside 0..12.5), 2 and 3 a fixed bucket from the depth (0x10DDF8); 0 (the in-order GIF list, 0x10E1C8)
        // not done here. False: not linked (the builder sends its packet instead, or nothing as the game would).
        enum class Linked
        {
            Yes,
            Dropped, // the game links nothing there either
            NotHere,
        };

        Linked insert2d(uint64_t key, float depth)
        {
            const uint32_t mode = peek32(kInsertMode);
            if (mode == 1u)
            {
                if (peek32(kRenderer) != 0u || depth < 0.0f || depth > 12.5f)
                    return Linked::Dropped;
                const float z = std::min(depth * 0.12f, 1.0f);
                const int32_t bucket = truncate(z * peekFloat(kRenderer + 0x34u));
                if (static_cast<uint32_t>(bucket) >= 0x2000u)
                    return Linked::Dropped;
                return linkMarker(key, bucket, true) ? Linked::Yes : Linked::Dropped;
            }
            int bucket;
            if (mode == 2u)
                bucket = depth < 0.0f ? 0x1FEF : depth < 0.5f ? truncate(depth * 15.0f) + 0x1FEA : depth < 1.0f ? truncate(depth * 15.0f) + 0x1FEF : 0x1FFE;
            else if (mode == 3u)
                bucket = depth < 0.0f ? 0x1FEA : depth < 1.0f ? truncate(depth * 3.0f) + 0x1FF3 : 0x1FF6;
            else if (mode == 0u)
                return Linked::NotHere;
            else
                return Linked::Dropped;
            return linkMarker(key, bucket) ? Linked::Yes : Linked::Dropped;
        }

        uint64_t newKey()
        {
            State &s = g_state;
            if (s.nextKey == 0u)
                s.nextKey = 1u;
            return (static_cast<uint64_t>(kMarkMagic) << 32) | s.nextKey++;
        }

        // ---- 0x120DE8 (vertex to the GS, ftoi4 on): the 2D view matrix, depth over the point's z, clamped at 0

        int32_t ftoi4(float v)
        {
            const float scaled = v * 16.0f;
            if (!(scaled == scaled))
                return 0;
            if (scaled >= 2147483648.0f)
                return std::numeric_limits<int32_t>::max();
            if (scaled <= -2147483648.0f)
                return std::numeric_limits<int32_t>::min();
            return static_cast<int32_t>(scaled);
        }

        struct GsPoint
        {
            int32_t x = 0, y = 0, z = 0;
        };

        GsPoint toGs(const float m[16], float x, float y, float z)
        {
            const float q = 1.0f / z;
            float out[4];
            for (int i = 0; i < 3; ++i)
            {
                float acc = m[i] * x;
                acc = acc + m[4 + i] * y;
                acc = acc + m[8 + i] * z;
                out[i] = acc + m[12 + i];
            }
            out[2] = std::max(out[2] * q, 0.0f);
            for (int i = 0; i < 3; ++i)
                pokeFloat(kScratchpad + 0x50u + static_cast<uint32_t>(i) * 4u, q);
            return GsPoint{ftoi4(out[0]), ftoi4(out[1]), ftoi4(out[2])};
        }

        void viewMatrix(float m[16])
        {
            const uint32_t matrix = peek32(peek32(kView) + 0x460u);
            for (int i = 0; i < 16; ++i)
                m[i] = peekFloat(matrix + static_cast<uint32_t>(i) * 4u);
        }

        uint32_t halfColour(uint32_t rgba)
        {
            uint32_t out = 0;
            for (int c = 0; c < 4; ++c)
                out |= (((((rgba >> (c * 8)) & 0xFFu) + 1u) >> 1) & 0xFFu) << (c * 8);
            return out;
        }

        GsVertex vertex(const GsPoint &p, float s, float t, uint32_t rgba)
        {
            GsVertex v;
            v.x = static_cast<float>(p.x & 0xFFFF) / 16.0f;
            v.y = static_cast<float>(p.y & 0xFFFF) / 16.0f;
            v.z = static_cast<uint32_t>(p.z);
            v.s = s;
            v.t = t;
            v.q = 1.0f;
            v.rgba = rgba;
            return v;
        }

        // A GS sprite (PRIM 6, STQ) as its two triangles: corner 0 and corner 1, the colour and depth the GS takes from
        // corner 1.
        void spriteTriangles(std::vector<GsVertex> &out, const GsPoint &p0, const GsPoint &p1, float s0, float t0, float s1, float t1,
                             uint32_t rgba)
        {
            GsPoint a = p0, b = p1;
            a.z = b.z;
            const GsVertex v00 = vertex(a, s0, t0, rgba);
            const GsVertex v11 = vertex(b, s1, t1, rgba);
            GsVertex v10 = v11, v01 = v00;
            v10.y = v00.y;
            v10.t = t0;
            v01.y = v11.y;
            v01.t = t1;
            v01.z = v11.z;
            out.insert(out.end(), {v00, v10, v01, v10, v11, v01});
        }

        GsMaterial spriteMaterial(const Page &page, uint64_t alpha)
        {
            GsMaterial m;
            m.prim.type = GS_PRIM_TRIANGLE;
            m.prim.tme = true;
            m.prim.abe = true;
            m.prim.ctxt = true;
            m.tex0 = ps2x::gs::decodeTex0(page.tex0);
            m.tex1 = page.tex1;
            m.clamp = page.clamp;
            m.alpha = alpha;
            m.testFromTarget = true;
            m.texture = page.texture;
            return m;
        }

        // ---- 0x1388E8 (items, count, alpha index): the small 2D labels, two passes (an item's byte 0x24 is its pass, the
        // texture file's page of that number), sprites of 0x28-byte items into bucket 0x1FF2

        // The sprites of a packet the game linked in front of `stop` from `head` (diagnostic, check mode): each block's GIF
        // tag after its NEXT, its NLOOP sprites of five quadwords (RGBAQ, ST, XYZ2, ST, XYZ2).
        std::vector<std::vector<Sprite>> packetSprites(uint32_t head, uint32_t stop)
        {
            std::vector<std::vector<Sprite>> out;
            for (uint32_t block = head, guard = 0; block != stop && block != 0u && guard < 64u; ++guard)
            {
                uint32_t q = block, next = 0;
                for (uint32_t i = 0; i < 16u; ++i, q += 16u)
                    if (((peek64(q) >> 28) & 7u) == 2u)
                    {
                        next = static_cast<uint32_t>(peek64(q) >> 32);
                        break;
                    }
                if (next == 0u)
                    break;
                const uint64_t tag = peek64(q + 16u);
                std::vector<Sprite> &sprites = out.emplace_back();
                uint32_t at = q + 32u;
                for (uint32_t n = 0; n < (tag & 0x7FFFu); ++n, at += 80u)
                {
                    Sprite sp;
                    for (int c = 0; c < 4; ++c)
                        sp.rgba |= (peek32(at + static_cast<uint32_t>(c) * 4u) & 0xFFu) << (c * 8);
                    sp.s0 = peekFloat(at + 16u);
                    sp.t0 = peekFloat(at + 20u);
                    sp.x0 = static_cast<int32_t>(peek32(at + 32u));
                    sp.y0 = static_cast<int32_t>(peek32(at + 36u));
                    sp.z0 = static_cast<int32_t>(peek32(at + 40u));
                    sp.s1 = peekFloat(at + 48u);
                    sp.t1 = peekFloat(at + 52u);
                    sp.x1 = static_cast<int32_t>(peek32(at + 64u));
                    sp.y1 = static_cast<int32_t>(peek32(at + 68u));
                    sp.z1 = static_cast<int32_t>(peek32(at + 72u));
                    if ((peek32(at + 44u) & 0x8000u) == 0u)
                        sprites.push_back(sp);
                }
                block = next;
            }
            return out;
        }

        bool sameSprite(const Sprite &a, const Sprite &b)
        {
            return a.rgba == b.rgba && a.s0 == b.s0 && a.t0 == b.t0 && a.s1 == b.s1 && a.t1 == b.t1 && a.x0 == b.x0 && a.y0 == b.y0 &&
                   a.z0 == b.z0 && a.x1 == b.x1 && a.y1 == b.y1 && a.z1 == b.z1;
        }

        void compare(BuilderStats &stats, const std::vector<std::vector<Sprite>> &native, std::vector<std::vector<Sprite>> game, const char *name)
        {
            ++stats.checked;
            std::reverse(game.begin(), game.end());
            const auto countDiff = [&](size_t p)
            {
                if (++stats.checkCountDiffs > 4u)
                    return;
                std::string line;
                for (const std::vector<std::vector<Sprite>> *side : {&native, static_cast<const std::vector<std::vector<Sprite>> *>(&game)})
                {
                    line += side == &native ? " native" : "; game";
                    for (const auto &pass : *side)
                        line += " " + std::to_string(pass.size());
                }
                std::fprintf(stderr, "[sc3:2d] check %s pass %zu: sprites%s\n", name, p, line.c_str());
            };
            if (native.size() != game.size())
            {
                countDiff(0);
                return;
            }
            for (size_t p = 0; p < native.size(); ++p)
            {
                if (native[p].size() != game[p].size())
                {
                    countDiff(p);
                    continue;
                }
                for (size_t i = 0; i < native[p].size(); ++i)
                {
                    ++stats.checkSprites;
                    if (sameSprite(native[p][i], game[p][i]))
                        continue;
                    if (++stats.checkDiffs <= 8u)
                    {
                        const Sprite &a = native[p][i], &b = game[p][i];
                        std::fprintf(stderr,
                                     "[sc3:2d] check %s sprite %zu: native rgba %#x st %g,%g %g,%g xyz %d,%d,%d %d,%d,%d; game rgba %#x st %g,%g %g,%g "
                                     "xyz %d,%d,%d %d,%d,%d\n",
                                     name, i, a.rgba, a.s0, a.t0, a.s1, a.t1, a.x0, a.y0, a.z0, a.x1, a.y1, a.z1, b.rgba, b.s0, b.t0, b.s1, b.t1, b.x0,
                                     b.y0, b.z0, b.x1, b.y1, b.z1);
                    }
                }
            }
        }

        // ---- 0x1388E8 (items, count, alpha index): the small 2D labels, two passes (an item's byte 0x24 is its pass, the
        // texture file's page of that number), sprites of 0x28-byte items into bucket 0x1FF2

        void labels(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.labels;
            const uint32_t items = getRegU32(ctx, 4);
            const int32_t count = static_cast<int32_t>(getRegU32(ctx, 5));
            const uint32_t alphaIndex = getRegU32(ctx, 6);
            const uint32_t gameQwords = 12u + 5u * static_cast<uint32_t>(std::max(count, 0));
            ++stats.calls;
            if (s.mode == Mode::Off || count <= 0)
            {
                if (count > 0)
                    stats.gifQwords += gameQwords;
                callOriginal(kLabels, rdram, ctx, runtime);
                return;
            }
            const uint32_t file = peek32(k2d + 0x4Cu);
            Page pages[2];
            bool used[2] = {false, false};
            for (int32_t i = 0; i < count; ++i)
            {
                const int8_t pass = static_cast<int8_t>(peek8(items + static_cast<uint32_t>(i) * 0x28u + 0x24u));
                if (pass == 0 || pass == 1)
                    used[pass] = true;
            }
            for (uint32_t pass = 0; pass < 2u; ++pass)
                if (used[pass] && !page(file, pass, pages[pass], stats) && s.mode == Mode::On)
                {
                    ++stats.toGs;
                    stats.gifQwords += gameQwords;
                    callOriginal(kLabels, rdram, ctx, runtime);
                    return;
                }
            float m[16];
            viewMatrix(m);
            const float z = peekFloat(k2d + 0x68u);
            const uint64_t alpha = peek64(kAlphaTable + alphaIndex * 8u);
            std::vector<std::vector<Sprite>> sprites;
            std::vector<GsVertex> triangles;
            for (uint32_t pass = 0; pass < 2u; ++pass)
            {
                if (!used[pass])
                    continue;
                std::vector<Sprite> &list = sprites.emplace_back();
                for (int32_t i = 0; i < count; ++i)
                {
                    const uint32_t item = items + static_cast<uint32_t>(i) * 0x28u;
                    if (static_cast<int8_t>(peek8(item + 0x24u)) != static_cast<int8_t>(pass))
                        continue;
                    const float x = peekFloat(item), y = peekFloat(item + 4u);
                    const GsPoint p0 = toGs(m, x, y, z);
                    const GsPoint p1 = toGs(m, x + peekFloat(item + 0x1Cu) + 1.0f, y + peekFloat(item + 0x20u) + 1.0f, z);
                    if (((p0.x | p0.y | p1.x | p1.y) & 0xFFFF0000) != 0)
                        continue;
                    Sprite sp;
                    sp.rgba = halfColour(peek32(item + 0x18u));
                    sp.s0 = peekFloat(item + 8u);
                    sp.t0 = peekFloat(item + 0xCu);
                    sp.s1 = peekFloat(item + 0x10u);
                    sp.t1 = peekFloat(item + 0x14u);
                    sp.x0 = p0.x, sp.y0 = p0.y, sp.z0 = p0.z, sp.x1 = p1.x, sp.y1 = p1.y, sp.z1 = p1.z;
                    list.push_back(sp);
                }
                if (s.mode != Mode::On || list.empty())
                    continue;
                triangles.clear();
                for (const Sprite &sp : list)
                    spriteTriangles(triangles, GsPoint{sp.x0, sp.y0, sp.z0}, GsPoint{sp.x1, sp.y1, sp.z1}, sp.s0, sp.t0, sp.s1, sp.t1, sp.rgba);
                const uint64_t key = newKey();
                if (!linkMarker(key, kLabelBucket))
                    continue;
                ps2x::native::Renderer::instance().drawGsKeyed(key, spriteMaterial(pages[pass], alpha), triangles.data(),
                                                               static_cast<uint32_t>(triangles.size()));
                stats.sprites += list.size();
                ++stats.markers;
            }
            if (s.mode == Mode::Check)
            {
                const uint32_t entry = peek32(kRenderer + 0x54u) + static_cast<uint32_t>(kLabelBucket) * 16u;
                const uint32_t before = peek32(entry + 4u);
                stats.gifQwords += gameQwords;
                callOriginal(kLabels, rdram, ctx, runtime);
                if (peek32(kRenderer) == 0u)
                    compare(stats, sprites, packetSprites(peek32(entry + 4u), before), "labels");
                return;
            }
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        void word4(const R5900Context *ctx, int reg, uint32_t out[4]) { std::memcpy(out, &ctx->r[reg], 16); }

        GsVertex packedVertex(uint32_t rgba[4], uint32_t xyz[4])
        {
            GsVertex v;
            v.x = static_cast<float>(xyz[0] & 0xFFFFu) / 16.0f;
            v.y = static_cast<float>(xyz[1] & 0xFFFFu) / 16.0f;
            v.z = xyz[2];
            v.rgba = (rgba[0] & 0xFFu) | ((rgba[1] & 0xFFu) << 8) | ((rgba[2] & 0xFFu) << 16) | ((rgba[3] & 0xFFu) << 24);
            return v;
        }

        // ---- 0x124C80 (xyz0, xyz1, xyz2 pointers to packed XYZ2 quadwords, a3 / t0 / t1 their RGBAQ quadwords, t2 ALPHA_2,
        // f12 the depth): one Gouraud-shaded blended triangle, untextured, inserted through 0x120978

        void triangle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.triangles;
            ++stats.calls;
            if (s.mode != Mode::On)
            {
                stats.gifQwords += 9u;
                callOriginal(kTriangle, rdram, ctx, runtime);
                return;
            }
            uint32_t xyz[3][4], rgba[3][4];
            for (int i = 0; i < 3; ++i)
            {
                const uint32_t at = getRegU32(ctx, 4 + i);
                for (int w = 0; w < 4; ++w)
                    xyz[i][w] = peek32(at + static_cast<uint32_t>(w) * 4u);
                word4(ctx, 7 + i, rgba[i]);
            }
            const uint64_t key = newKey();
            const Linked linked = insert2d(key, ctx->f[12]);
            if (linked == Linked::NotHere)
            {
                ++stats.toGs;
                stats.gifQwords += 9u;
                callOriginal(kTriangle, rdram, ctx, runtime);
                return;
            }
            if (linked == Linked::Yes && (xyz[2][3] & 0x8000u) == 0u)
            {
                GsMaterial m;
                m.prim.type = GS_PRIM_TRIANGLE;
                m.prim.iip = true;
                m.prim.abe = true;
                m.prim.ctxt = true;
                m.alpha = getRegU32(ctx, 10) | (static_cast<uint64_t>(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[10], 1))) << 32);
                m.testFromTarget = true;
                const GsVertex v[3] = {packedVertex(rgba[0], xyz[0]), packedVertex(rgba[1], xyz[1]), packedVertex(rgba[2], xyz[2])};
                ps2x::native::Renderer::instance().drawGsKeyed(key, m, v, 3u);
                ++stats.sprites;
                ++stats.markers;
            }
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        void insertion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t at = ctx->pc;
            const uint32_t base = peek32(kScratchpad);
            const uint32_t end = getRegU32(ctx, 4);
            auto &counted = s.inserted[{getRegU32(ctx, 31), at}];
            ++counted.first;
            if (end > base && end - base < 0x4000u)
                counted.second += (end - base) / 16u + 1u;
            callOriginal(at, rdram, ctx, runtime);
        }

        // ---- frame

        void reportBuilder(const char *name, BuilderStats &b)
        {
            std::fprintf(stderr,
                         "[sc3:2d]   %s: %llu calls, %llu native (%llu sprites, %llu markers), %llu to the GS (no file %llu, no block %llu, "
                         "no texture %llu), GIF qwords built %llu; checked %llu (%llu sprites, %llu differing, %llu counts differing)\n",
                         name, static_cast<unsigned long long>(b.calls), static_cast<unsigned long long>(b.native),
                         static_cast<unsigned long long>(b.sprites), static_cast<unsigned long long>(b.markers),
                         static_cast<unsigned long long>(b.toGs), static_cast<unsigned long long>(b.noFile),
                         static_cast<unsigned long long>(b.noBlock), static_cast<unsigned long long>(b.noTexture),
                         static_cast<unsigned long long>(b.gifQwords), static_cast<unsigned long long>(b.checked),
                         static_cast<unsigned long long>(b.checkSprites), static_cast<unsigned long long>(b.checkDiffs),
                         static_cast<unsigned long long>(b.checkCountDiffs));
            b = BuilderStats{};
        }

        void report()
        {
            State &s = g_state;
            if (++s.frames % 250u != 0u)
                return;
            std::string misses;
            char item[96];
            for (const auto &[tex0, n] : s.textureMisses)
            {
                std::snprintf(item, sizeof(item), " tbp %#x psm %#x cbp %#x x%llu", static_cast<unsigned>(tex0 & 0x3FFFu),
                              static_cast<unsigned>((tex0 >> 20) & 0x3Fu), static_cast<unsigned>((tex0 >> 37) & 0x3FFFu),
                              static_cast<unsigned long long>(n));
                misses += item;
            }
            s.textureMisses.clear();
            std::fprintf(stderr, "[sc3:2d] 250 frames, native 2D %s: markers read in the GIF chain %llu, leaves bound %llu (not the pack's %llu);%s\n",
                         s.mode == Mode::On ? "on" : s.mode == Mode::Check ? "checked" : "off", static_cast<unsigned long long>(s.markersRead.exchange(0)),
                         static_cast<unsigned long long>(s.bound), static_cast<unsigned long long>(s.boundBad),
                         misses.empty() ? " no texture misses" : misses.c_str());
            reportBuilder("labels 0x1388E8", s.labels);
            reportBuilder("triangles 0x124C80", s.triangles);
            std::vector<std::pair<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>>> sorted(s.inserted.begin(), s.inserted.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
            uint64_t qwords = 0;
            for (const auto &entry : sorted)
                qwords += entry.second.second;
            std::string top;
            for (size_t i = 0; i < sorted.size() && i < 16u; ++i)
            {
                std::snprintf(item, sizeof(item), " %#x>%#x:%llu/%llu", sorted[i].first.first, sorted[i].first.second,
                              static_cast<unsigned long long>(sorted[i].second.first), static_cast<unsigned long long>(sorted[i].second.second));
                top += item;
            }
            std::fprintf(stderr, "[sc3:2d]   2D packets the game still builds: %llu qwords (ra>insert:calls/qwords)%s\n",
                         static_cast<unsigned long long>(qwords), top.c_str());
            s.inserted.clear();
        }

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            followSwitch();
            report();
            callOriginal(kFrameKick, rdram, ctx, runtime);
        }

        void wrap(PS2Runtime &runtime, uint32_t address, Function hook)
        {
            Function original = runtime.lookupFunction(address);
            if (!original)
            {
                std::fprintf(stderr, "[sc3:2d] no function at %#x\n", address);
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

    void installNative2d(PS2Runtime &runtime, bool nativeGs)
    {
        using namespace native2d;
        const char *mode = std::getenv("SC3_NATIVE_2D");
        const char *path = std::getenv("SC3_2D_PACK");
        if (!mode || !*mode || *mode == '0')
            return;
        if (!nativeGs || !path || !*path)
        {
            std::fprintf(stderr, "[sc3:2d] SC3_NATIVE_2D needs the native GS renderer and SC3_2D_PACK, not used\n");
            return;
        }
        State &s = g_state;
        if (!s.pack.load(path))
        {
            std::fprintf(stderr, "[sc3:2d] %s: %s\n", path, s.pack.error().c_str());
            return;
        }
        std::string stem = path;
        const size_t dot = stem.rfind('.');
        stem = dot == std::string::npos ? stem : stem.substr(0, dot);
        const std::string leaves = stem + ".leaves";
        if (!s.textures.load(stem + "_textures.pack"))
        {
            std::fprintf(stderr, "[sc3:2d] %s_textures.pack: not a PS2XTEX1 pack\n", stem.c_str());
            return;
        }
        if (!readLeaves(leaves))
        {
            std::fprintf(stderr, "[sc3:2d] %s: no leaves\n", leaves.c_str());
            return;
        }
        if (*mode == '@')
            s.switchPath = mode + 1;
        else
            s.mode = std::strcmp(mode, "check") == 0 ? Mode::Check : Mode::On;
        s.runtime = &runtime;
        s.rdram = runtime.memory().getRDRAM();
        s.scratchpad = runtime.memory().getScratchpad();
        wrap(runtime, kFileRequest, fileRequest);
        wrap(runtime, kCdRead, cdRead);
        wrap(runtime, kLabels, labels);
        wrap(runtime, kTriangle, triangle);
        wrap(runtime, kFrameKick, frameKick);
        for (const uint32_t address : kInserts)
            wrap(runtime, address, insertion);
        ps2x::native::setGifChainMarkGate(markerGate);
        s.installed = true;
        std::fprintf(stderr, "[sc3:2d] native 2D%s: %s, %zu leaves with texture pages\n", s.switchPath.empty() ? "" : " (as its switch file says)",
                     path, s.pack.files().size());
    }
}
