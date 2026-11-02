#include "sc3_2d.h"
#include "runtime/native/native_texture_pack.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_registers.h"
#include "runtime/native/native_gs_hook.h"
#include "runtime/native/native_mesh_pack.h"
#include "runtime/native/native_renderer.h"

#include <algorithm>
#include <array>
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
#include <tuple>
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
        constexpr uint32_t kInserts[] = {0x00120978u, 0x0010DDF8u, 0x0010DF30u, 0x0010E068u, 0x0010E1C8u, 0x0010E368u,
                                         0x0010D9A0u, 0x0010DC90u, 0x0010E1F8u, 0x0010D2F0u};

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
        constexpr uint32_t kClutReg = 0x103u;                          // another CLUT the file uploads: CBP << 32 | record
        constexpr uint64_t kTex2Fields = (0x3Full << 20) | (~0ull << 37); // what TEX2 changes of TEX0: PSM, CBP .. CLD
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

        // One A+D write or one glyph (its six REGLIST words) of a packet, as the game would have put it in the scratchpad.
        struct TextWrite
        {
            uint32_t reg = 0; // kGlyph: a glyph
            uint64_t w[6] = {};
        };
        constexpr uint32_t kGlyph = 0xFFFFu;

        struct TextPacket
        {
            uint32_t insert = 0, layer = 0;
            std::vector<TextWrite> writes;
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
            std::vector<Leaf> elfFiles; // texture files inside the ELF: offset is their address
            int64_t olkLbn = -1;
            uint32_t requestDest = 0, requestPos = 0;
            bool requestPending = false;
            packs::TexturePack textures;
            struct Decoded
            {
                ps2x::native::TextureCache::Texture image;
                ps2x::native::NativeTexture native;
            };
            std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Decoded> decoded; // (pack file, page block offset, CBP)
            uint32_t nextKey = 1;
            std::atomic<uint64_t> markersRead{0};
            BuilderStats labels, triangles, text, quads, lines, icons, effects;
            uint64_t uploadsDropped = 0;
            Function format = nullptr; // 0x11CBC8, the text renderer's formatter
            bool capture = false;      // check mode: the game's text packets kept as its insertions get them
            std::vector<TextPacket> captured;
            std::map<uint64_t, uint64_t> textureMisses; // TEX0 -> draws left to the GS without a pack texture
            // Packets the game still builds for 2D, by the return address of their insertion (ra: the builder) and the
            // insertion function: calls and quadwords.
            std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>> inserted;
            uint64_t frames = 0, bound = 0, boundBad = 0;
        };

        State g_state;

        // EE memory or the scratchpad (the game keeps stacks and packets there), aligned to the access.
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
        uint64_t peek64(uint32_t address) { return peek<uint64_t>(address); }
        float peekFloat(uint32_t address) { return peek<float>(address); }
        void poke8(uint32_t address, uint8_t value) { poke(address, value); }
        void poke16(uint32_t address, uint16_t value) { poke(address, value); }
        void poke32(uint32_t address, uint32_t value) { poke(address, value); }
        void poke64(uint32_t address, uint64_t value) { poke(address, value); }

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

        // A game function called from C++ and run to its return: generated code goes back to the dispatcher at a call when
        // an EE checkpoint is due (ctx->pc the callee), so the rest runs here, function by function, until the return
        // address is reached with the caller's stack pointer.
        void runToReturn(PS2Runtime::RecompiledFunction function, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
            function(rdram, ctx, runtime);
            while (ctx->pc != ra || getRegU32(ctx, 29) != sp)
                runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
        }

        void runOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            runToReturn(g_state.originals.at(address), rdram, ctx, runtime);
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
        bool blockPage(uint32_t block, Page &out, BuilderStats &stats, uint32_t cbp);

        bool page(uint32_t file, uint32_t index, Page &out, BuilderStats &stats, uint32_t cbp = ~0u)
        {
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
            return blockPage(block, out, stats, cbp);
        }

        // A register block inside a loaded leaf, as the 2D pack has it (a page's TEX0 / TEX1 / CLAMP and its texture).
        bool blockPage(uint32_t block, Page &out, BuilderStats &stats, uint32_t cbp)
        {
            State &s = g_state;
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
            uint64_t texels = kNoRecord, palette = kNoRecord, other = kNoRecord;
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
                if (reg == kClutReg && static_cast<uint32_t>(value >> 32) == cbp)
                    other = value & 0xFFFFFFFFu;
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
            if (cbp != ~0u && cbp != ((out.tex0 >> 37) & 0x3FFFu))
            {
                if (other == kNoRecord)
                {
                    ++stats.noTexture;
                    ++s.textureMisses[(out.tex0 & ~(0x3FFFull << 37)) | (static_cast<uint64_t>(cbp) << 37)];
                    return false;
                }
                palette = other;
            }
            else
                cbp = static_cast<uint32_t>((out.tex0 >> 37) & 0x3FFFu);
            const std::vector<packs::TextureRecord> *records = s.textures.file(image.file->name);
            if (records && texels < records->size())
            {
                auto [it, added] = s.decoded.try_emplace({image.index, block - image.base, cbp});
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
                    d.native.key = (static_cast<uint64_t>(image.index) << 40) | (static_cast<uint64_t>(cbp) << 26) | (block - image.base);
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
        // The game's insertions end by switching the scratchpad's packet half (0x70000004 ^= 1, the packet base
        // *(0x70000000) = that half's start): the next builder's packet goes in the other half.
        void switchScratchHalf()
        {
            const uint32_t half = peek32(kScratchpad + 4u) ^ 1u;
            poke32(kScratchpad + 4u, half);
            poke32(kScratchpad, peek32(kScratchpad + 8u + half * 4u));
        }

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
            switchScratchHalf();
            return true;
        }

        // A GIF-list marker (0x10E1C8 + 0x10F808 / 0x10EF78 for a packet of one quadword): at the list's cursor (+8, going
        // up), the list's quadword count (*(+0x10)) one more.
        bool linkGifListMarker(uint64_t key)
        {
            if (peek32(kRenderer) != 0u)
                return false;
            const uint32_t at = peek32(kRenderer + 8u);
            poke64(at, 0x10000000ull);
            poke64(at + 8u, key);
            poke32(kRenderer + 8u, at + 16u);
            const uint32_t count = peek32(kRenderer + 0x10u);
            poke32(count, peek32(count) + 1u);
            switchScratchHalf();
            return true;
        }

        // cvt.w.s / vftoi0 as the EE does them: no NaN or infinity there, an exponent of 255 is a huge value (saturates).
        int32_t truncate(float v) { return ps2FpuCvtWS(v); }

        // 0x120978 (end, start, qwc, depth f12) for a marker: by the mode word, 1 the depth-sorted layer bucket (0x10D9A0:
        // nothing outside 0..12.5), 2 and 3 a fixed bucket from the depth (0x10DDF8); 0 the in-order GIF list (0x10E1C8),
        // a marker appended to it. Dropped: the game links nothing there either.
        enum class Linked
        {
            Yes,
            Dropped, // the game links nothing there either
            NotHere,
        };

        Linked insert2d(uint64_t key, uint32_t mode, float depth)
        {
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
            {
                return linkGifListMarker(key) ? Linked::Yes : Linked::Dropped;
            }
            else
                return Linked::Dropped;
            return linkMarker(key, bucket) ? Linked::Yes : Linked::Dropped;
        }

        Linked insert2d(uint64_t key, float depth) { return insert2d(key, peek32(kInsertMode), depth); }

        // A key's batches dropped (their marker could not be linked). Batches go in before the marker: the GIF list can be
        // read while the frame is still being built.
        void discardKeyed(uint64_t key)
        {
            ps2x::native::Renderer::instance().takeGsKeyed(key, [](void *, const GsMaterial &, const GsVertex *, uint32_t) {}, nullptr);
        }

        uint64_t newKey()
        {
            State &s = g_state;
            if (s.nextKey == 0u)
                s.nextKey = 1u;
            return (static_cast<uint64_t>(kMarkMagic) << 32) | s.nextKey++;
        }

        // ---- 0x120DE8 (vertex to the GS, ftoi4 on): the 2D view matrix, depth over the point's z, clamped at 0

        int32_t ftoi4(float v) { return ps2FpuCvtWS(v * 16.0f); }

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

        // A GS sprite (PRIM 6): its two corners, drawn by the GS renderer as the GS draws a sprite.
        void spriteCorners(std::vector<GsVertex> &out, const GsPoint &p0, const GsPoint &p1, float s0, float t0, float s1, float t1,
                           uint32_t rgba)
        {
            out.push_back(vertex(p0, s0, t0, rgba));
            out.push_back(vertex(p1, s1, t1, rgba));
        }

        GsMaterial spriteMaterial(const Page &page, uint64_t alpha)
        {
            GsMaterial m;
            m.prim.type = GS_PRIM_SPRITE;
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
                    spriteCorners(triangles, GsPoint{sp.x0, sp.y0, sp.z0}, GsPoint{sp.x1, sp.y1, sp.z1}, sp.s0, sp.t0, sp.s1, sp.t1, sp.rgba);
                const uint64_t key = newKey();
                ps2x::native::Renderer::instance().drawGsKeyed(key, spriteMaterial(pages[pass], alpha), triangles.data(),
                                                               static_cast<uint32_t>(triangles.size()));
                if (!linkMarker(key, kLabelBucket))
                {
                    discardKeyed(key);
                    continue;
                }
                stats.sprites += list.size();
                ++stats.markers;
            }
            if (s.mode == Mode::Check)
            {
                const uint32_t entry = peek32(kRenderer + 0x54u) + static_cast<uint32_t>(kLabelBucket) * 16u;
                const uint32_t before = peek32(entry + 4u);
                stats.gifQwords += gameQwords;
                runOriginal(kLabels, rdram, ctx, runtime);
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
            const bool drawn = (xyz[2][3] & 0x8000u) == 0u;
            if (drawn)
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
            }
            const Linked linked = insert2d(key, ctx->f[12]);
            if (linked != Linked::Yes && drawn)
                discardKeyed(key);
            if (linked == Linked::NotHere)
            {
                ++stats.toGs;
                stats.gifQwords += 9u;
                callOriginal(kTriangle, rdram, ctx, runtime);
                return;
            }
            if (linked == Linked::Yes && drawn)
            {
                ++stats.sprites;
                ++stats.markers;
            }
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- 0x11EA80 (mode, string, argument): the text renderer. Mode 0 measures (the text state's cursor moves, nothing
        // drawn), else it draws (mode < 0: the cursor placed by the text state first); mode & 7 picks the insertion (2 the
        // in-order GIF list 0x10E1C8, 4 a bucket by layer 0x10E068, else the text layers 0x10DF30), the text state's byte 6
        // the layer. Glyphs are REGLIST sprites (PRIM, ST, RGBAQ, XYZF3, ST, XYZF2) after A+D writes of TEX2_2 (colour:
        // one of the font's CLUTs) and ALPHA_2; a scratchpad packet is inserted when it fills and at the end. Codes 0x23 ..
        // 0x25 go to the game's own formatter 0x11CBC8 (no packet: it formats into the call's buffer and changes the state),
        // which may ask for an ALPHA (1) or TEX2 (2) write. The call frame (the formatter takes pointers into it) is laid
        // out on the guest stack as the game's.

        // The packet the game builds in a scratchpad half, by byte offsets from its start (0x11DDD8's bookkeeping): the
        // cursor, the limit, the open GIF tag and its kind (0 none, 1 A+D, 2 REGLIST).
        struct TextRun
        {
            uint32_t cursor = 0, limit = 0, tag = 0, insert = 0;
            int kind = 0;
            uint32_t adCount = 0;
            std::vector<TextWrite> writes;
            std::vector<TextPacket> packets; // inserted, in the order the game inserts them
        };

        constexpr uint32_t kText = 0x0011EA80u;
        constexpr uint32_t kTextFormat = 0x0011CBC8u;
        constexpr uint32_t kTextFormatReturn = 0x0011ED0Cu; // where 0x11EA80 returns from it
        constexpr uint32_t kTextState = 0x00468540u;
        constexpr uint32_t kTextStateSize = 0x68u;
        constexpr uint32_t kTextGlobals = 0x00468400u, kTextGlobalsSize = 0xC00u; // the state and the formatter's tables around it
        constexpr uint32_t kFontFile = 0x004763B0u;
        constexpr uint32_t kFontDescs = 0x005BE840u;   // 28 bytes a font
        constexpr uint32_t kFontSt = 0x00468648u;      // per font: a glyph's two ST words, 16 bytes a code
        constexpr uint32_t kFontKerning = 0x004684A8u; // per font: two bytes a code (offset * 16, advance)
        constexpr uint32_t kGlyphShift = 0x00468A60u;  // a byte a code: > 0 raised, < 0 lowered by the state's offset
        constexpr uint32_t kAccents = 0x00467848u;     // pointer: the accent drawn over codes 0xC0 and up
        constexpr uint32_t kTextTex2 = 0x005BE8E8u;    // TEX2_2 by font * 16 + colour
        constexpr uint32_t kTextAlphas = 0x004685C0u;  // ALPHA_2 by the state's byte 5
        constexpr uint32_t kScreenMode = 0x00469230u;  // pointer: +0x50 non-zero, widths scaled
        constexpr uint32_t kInsertText = 0x0010DF30u, kInsertLayer = 0x0010E068u, kInsertList = 0x0010E1C8u;
        constexpr uint32_t kPacketLimit = 0x1F40u;
        constexpr uint64_t kGlyphTag = 0x6400000000008000ull, kGlyphRegs = 0x42C120ull;

        struct TextCall
        {
            uint32_t frame = 0, c = 0, buffer = 0;
            int32_t mode = 0;
            bool dry = false;
            TextRun run;
        };

        float f(uint32_t address) { return peekFloat(address); }
        void setF(uint32_t address, float v) { pokeFloat(address, v); }

        // 0x11AAF8 (colour, 128): the float colour times 128, to integers, each at most 255, as RGBA bytes.
        uint32_t textColour(uint32_t colour)
        {
            uint32_t out = 0;
            for (int i = 0; i < 4; ++i)
            {
                int32_t v = ps2FpuCvtWS(ps2_fmul(f(colour + static_cast<uint32_t>(i) * 4u), 128.0f));
                v = std::min(v, 255);
                out |= (static_cast<uint32_t>(v) & 0xFFu) << (i * 8);
            }
            return out;
        }

        uint32_t zWord(float z)
        {
            if (z >= 2147483648.0f)
                return (static_cast<uint32_t>(ps2FpuCvtWS(ps2_fsub(z, 2147483648.0f))) | 0x80000000u) >> 4;
            return static_cast<uint32_t>(ps2FpuCvtWS(z)) >> 4;
        }

        // 0x11E068 (call, state): the state's cursor through the 2D view to the GS (times 16), the call's corners.
        void textPlace(uint32_t c)
        {
            const uint32_t t = kTextState;
            float m[16];
            viewMatrix(m);
            const float x = f(t + 0x10u), y = f(t + 0x14u), z = f(t + 0x18u);
            const float q = 1.0f / z;
            float out[3];
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
            for (int i = 0; i < 3; ++i)
                setF(t + 0x30u + static_cast<uint32_t>(i) * 4u, out[i] * 16.0f);
            poke32(t + 0x3Cu, 0u);
            const float px = f(t + 0x30u), py = f(t + 0x34u);
            setF(c + 0x0u, px);
            setF(c + 0x4u, py);
            poke32(c + 0x40u, zWord(f(t + 0x38u)));
            setF(c + 0x8u, ps2_fadd(px, f(t + 0x48u)));
            setF(c + 0xCu, ps2_fadd(py, f(t + 0x4Cu)));
            poke16(c + 0x4Cu, static_cast<uint16_t>(ps2FpuCvtWS(f(t + 0x60u))));
        }

        // 0x11DD68: the open REGLIST tag closed (0), or what is open: nothing (1), an empty REGLIST tag (2), an A+D tag (3).
        int textClose(TextRun &r, uint32_t c)
        {
            const uint16_t glyphs = peek16(c + 0x44u);
            if (glyphs != 0u)
            {
                poke16(c + 0x44u, 0u);
                r.kind = 0;
                return 0;
            }
            if (r.kind == 0)
                return 1;
            return r.kind == 1 ? 3 : 2;
        }

        void textInsert(TextCall &call);

        // 0x11DDD8 + an A+D register (0x11DF40 TEX2_2, 0x11DFC8 ALPHA_2): a new packet when none is open, else the open A+D
        // tag one longer or a new one.
        void textAd(TextCall &call, uint32_t reg, uint64_t value)
        {
            TextRun &r = call.run;
            const uint32_t c = call.c;
            int out = 0;
            if (peek16(c + 0x46u) == 0u)
            {
                r.kind = 0;
                r.limit = kPacketLimit;
                r.cursor = 16u;
                r.writes.clear();
                r.insert = 0;
            }
            else
            {
                const int k = textClose(r, c);
                if (k == 2)
                {
                    out -= 1;
                    r.cursor = r.tag;
                    r.kind = 0;
                }
                else if (k != 3)
                    r.kind = 0;
            }
            if (r.kind == 0)
            {
                out += 1;
                r.tag = r.cursor;
                r.kind = 1;
                r.cursor += 16u;
            }
            poke16(c + 0x46u, static_cast<uint16_t>(peek16(c + 0x46u) + out));
            TextWrite w;
            w.reg = reg;
            w.w[0] = value;
            r.writes.push_back(w);
            r.cursor += 16u;
            poke16(c + 0x46u, static_cast<uint16_t>(peek16(c + 0x46u) + 1u));
        }

        void textTex2(TextCall &call)
        {
            const uint32_t t = kTextState;
            textAd(call, 0x17u, peek64(kTextTex2 + (peek8(t) * 16u + peek8(t + 1u)) * 8u));
        }

        void textAlpha(TextCall &call)
        {
            const uint32_t t = kTextState;
            const uint32_t index = peek8(t + 5u);
            textAd(call, 0x43u, peek64(kTextAlphas + index * 8u));
            poke32(t + 0x58u, textColour(t + 0x20u));
            if (peek8(t + 5u) == 0u)
                poke8(t + 0x5Bu, 0x80u);
        }

        // 0x11EA10 (call, state, what the formatter asked for).
        void textState(TextCall &call, uint32_t what)
        {
            if (what == 1u)
                textAlpha(call);
            else if (what == 2u)
                textTex2(call);
        }

        uint32_t textInsertion(int32_t mode)
        {
            const int32_t kind = mode & 7;
            return kind == 2 ? kInsertList : kind == 4 ? kInsertLayer : kInsertText;
        }

        // The packet inserted (the game's insertion through the call's +0x48 with the state's layer).
        void textFlush(TextCall &call)
        {
            TextPacket packet;
            packet.insert = textInsertion(call.mode);
            packet.layer = peek8(kTextState + 6u);
            packet.writes = std::move(call.run.writes);
            call.run.writes.clear();
            call.run.packets.push_back(std::move(packet));
            textInsert(call);
        }

        // 0x11E740 (buffer, call, state): a run measured, the cursor moved over it.
        void textMeasure(TextCall &call)
        {
            const uint32_t c = call.c, t = kTextState;
            if (peek32(c + 0x40u) == 0u)
                textPlace(c);
            const float spacing = f(c + 0x10u);
            const bool spaced = spacing != 0.0f;
            const float advance = f(t + 0x50u);
            const float kernScale = f(t + 8u);
            const bool kerned = peek8(t + 4u) != 0u;
            float x = f(t + 0x10u);
            for (uint32_t at = call.buffer;; ++at)
            {
                const uint8_t code = peek8(at);
                if (code == 0u)
                    break;
                if (code == 0x0Au)
                {
                    if (f(c + 0x58u) < x)
                        setF(c + 0x58u, x);
                    poke32(c + 0x54u, 0u);
                    setF(t + 0x10u, f(c + 0x30u));
                    setF(t + 0x14u, ps2_fadd(f(t + 0x14u), f(t + 0x54u)));
                    textPlace(c);
                    x = f(t + 0x10u);
                    continue;
                }
                if (kerned)
                    x = ps2_fadd(x, ps2_fmul(static_cast<float>(peek8(peek32(c + 0x1Cu) + code * 2u + 1u)), kernScale));
                else
                    x = ps2_fadd(x, advance);
                if (spaced)
                {
                    x = ps2_fadd(x, spacing);
                    poke32(c + 0x54u, 1u);
                }
            }
            setF(t + 0x10u, x);
        }

        void textGlyph(TextCall &call, uint32_t code, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
        {
            const uint32_t c = call.c, t = kTextState;
            poke32(c + 0x3Cu, peek32(c + 0x3Cu) + 1u);
            const uint32_t st = peek32(c + 0x18u) + code * 16u;
            const uint64_t z = peek32(c + 0x40u);
            TextWrite w;
            w.reg = kGlyph;
            w.w[0] = 0x256u;
            w.w[1] = peek64(st);
            w.w[2] = peek64(t + 0x58u);
            w.w[3] = (static_cast<uint64_t>(x0) & 0xFFFFu) | ((static_cast<uint64_t>(y0) & 0xFFFFu) << 16) | (z << 32);
            w.w[4] = peek64(st + 8u);
            w.w[5] = (static_cast<uint64_t>(x1) & 0xFFFFu) | ((static_cast<uint64_t>(y1) & 0xFFFFu) << 16) | (z << 32);
            call.run.writes.push_back(w);
            call.run.cursor += 0x30u;
            poke16(c + 0x44u, static_cast<uint16_t>(peek16(c + 0x44u) + 1u));
            poke16(c + 0x46u, static_cast<uint16_t>(peek16(c + 0x46u) + 3u));
        }

        // 0x11E160 (buffer, call, state): a run of glyphs drawn, the cursor moved over it.
        void textDraw(TextCall &call)
        {
            const uint32_t c = call.c, t = kTextState;
            TextRun &r = call.run;
            if (peek32(c + 0x40u) == 0u)
                textPlace(c);
            const float spacing = f(c + 0x10u);
            const float spacing16 = ps2_fmul(spacing, 16.0f);
            bool spaced = spacing != 0.0f;
            const float advance = f(t + 0x40u), worldAdvance = f(t + 0x50u), kernScale = f(t + 8u);
            float x0 = f(c + 0x0u), y0f = f(c + 0x4u), x1 = f(c + 0x8u), y1f = f(c + 0xCu), world = f(t + 0x10u);
            const bool kerned = peek8(t + 4u) != 0u;
            uint32_t at = call.buffer;
            if (peek8(at) == 0u)
                return;
            while (true)
            {
                if (peek16(c + 0x46u) == 0u)
                {
                    textTex2(call);
                    textAlpha(call);
                    r.tag = r.cursor;
                    r.kind = 2;
                    r.cursor += 16u;
                    poke16(c + 0x44u, 0u);
                    poke16(c + 0x46u, static_cast<uint16_t>(peek16(c + 0x46u) + 1u));
                }
                else if (r.kind == 1)
                {
                    r.tag = r.cursor;
                    r.kind = 2;
                    r.cursor += 16u;
                    poke16(c + 0x44u, 0u);
                    poke16(c + 0x46u, static_cast<uint16_t>(peek16(c + 0x46u) + 1u));
                }
                int32_t y0 = ps2FpuCvtWS(y0f), y1 = ps2FpuCvtWS(y1f);
                const uint32_t accented = peek8(peek32(c + 0x14u) + 0x19u);
                const int32_t offset = static_cast<int16_t>(peek16(c + 0x4Cu));
                const bool shifted = offset != 0;
                int32_t raised0 = y0 - offset, raised1 = y1 - offset, lowered0 = y0 + offset, lowered1 = y1 + offset;
                if (!shifted)
                    raised0 = lowered0 = y0, raised1 = lowered1 = y1;
                bool full = false;
                if (((static_cast<uint32_t>(y0) | static_cast<uint32_t>(lowered1)) & 0xFFFF0000u) != 0u)
                {
                    while (peek8(at) != 0u)
                        ++at;
                    break;
                }
                for (uint8_t code = peek8(at); code != 0u; code = peek8(++at))
                {
                    if (r.limit - r.cursor < 6u)
                    {
                        full = true;
                        break;
                    }
                    float kernAdvance = 0.0f;
                    bool drawn = false;
                    if (code == 0x20u)
                    {
                        if (kerned)
                            kernAdvance = ps2_fmul(static_cast<float>(peek8(peek32(c + 0x1Cu) + 0x41u)), kernScale);
                        drawn = true;
                    }
                    else if (code == 0x0Au)
                    {
                        setF(t + 0x10u, f(c + 0x30u));
                        setF(t + 0x14u, ps2_fadd(f(t + 0x14u), f(t + 0x54u)));
                        poke32(c + 0x54u, 0u);
                        textPlace(c);
                        x0 = f(c + 0x0u);
                        y0f = f(c + 0x4u);
                        y1f = f(c + 0xCu);
                        y0 = ps2FpuCvtWS(y0f);
                        y1 = ps2FpuCvtWS(y1f);
                        x1 = f(c + 0x8u);
                        if (shifted)
                        {
                            const int32_t o = static_cast<int16_t>(peek16(c + 0x4Cu));
                            lowered1 = y1 + o, raised0 = y0 - o, raised1 = y1 - o, lowered0 = y0 + o;
                        }
                        else
                            raised0 = lowered0 = y0, raised1 = lowered1 = y1;
                        world = f(t + 0x10u);
                        continue;
                    }
                    else
                    {
                        int32_t gx0 = ps2FpuCvtWS(x0), gx1 = ps2FpuCvtWS(x1);
                        if (kerned)
                        {
                            const uint32_t k = peek32(c + 0x1Cu) + code * 2u;
                            const float shift = ps2_fmul(static_cast<float>(static_cast<int32_t>(peek8(k)) << 4), kernScale);
                            kernAdvance = ps2_fmul(static_cast<float>(peek8(k + 1u)), kernScale);
                            const int32_t dx = ps2FpuCvtWS(shift);
                            gx1 -= dx;
                            gx0 -= dx;
                        }
                        if (((static_cast<uint32_t>(gx0) | static_cast<uint32_t>(gx1)) & 0xFFFF0000u) == 0u)
                        {
                            int32_t gy0 = y0, gy1 = y1;
                            if (shifted)
                            {
                                const int8_t how = static_cast<int8_t>(peek8(kGlyphShift + code));
                                if (how > 0)
                                    gy0 = raised0, gy1 = raised1;
                                else if (how < 0 && accented != 0u)
                                    gy0 = lowered0, gy1 = lowered1;
                            }
                            textGlyph(call, code, gx0, gy0, gx1, gy1);
                            if (code >= 0xC0u && accented != 0u)
                                if (const uint8_t accent = peek8(peek32(kAccents) + (code - 0xC0u)); accent != 0u)
                                    textGlyph(call, accent, gx0, y0, gx1, y1);
                        }
                        drawn = true;
                    }
                    if (drawn)
                    {
                        if (kerned)
                        {
                            world = ps2_fadd(world, kernAdvance);
                            const float a = static_cast<float>(ps2FpuCvtWS(ps2_fmul(kernAdvance, 16.0f)));
                            x1 = ps2_fadd(x1, a);
                            x0 = ps2_fadd(x0, a);
                        }
                        else
                        {
                            x0 = ps2_fadd(x0, advance);
                            x1 = ps2_fadd(x1, advance);
                            world = ps2_fadd(world, worldAdvance);
                        }
                        if (spaced)
                        {
                            world = ps2_fadd(world, spacing);
                            x0 = ps2_fadd(x0, spacing16);
                            poke32(c + 0x54u, 1u);
                            x1 = ps2_fadd(x1, spacing16);
                        }
                    }
                }
                if (!full && r.limit - r.cursor < 3u)
                    full = true;
                if (!full)
                    break;
                if (peek16(c + 0x44u) != 0u)
                {
                    textClose(r, c);
                    textFlush(call);
                    poke16(c + 0x46u, 0u);
                }
                if (peek8(at) == 0u)
                    break;
            }
            setF(c + 0x0u, x0);
            setF(c + 0x4u, y0f);
            setF(t + 0x30u, x0);
            setF(t + 0x34u, y0f);
            setF(c + 0xCu, y1f);
            setF(c + 0x8u, x1);
            setF(t + 0x10u, world);
        }

        // 0x11E898 (buffer, call, state): the last packet inserted, the cursor where the text ends.
        void textEnd(TextCall &call)
        {
            const uint32_t c = call.c, t = kTextState;
            TextRun &r = call.run;
            if (peek16(c + 0x44u) != 0u)
            {
                textClose(r, c);
                textFlush(call);
                poke16(c + 0x46u, 0u);
            }
            else if (peek32(c + 0x3Cu) != 0u && r.kind == 1)
                textFlush(call);
            if (peek32(c + 0x54u) != 0u)
            {
                setF(t + 0x10u, ps2_fsub(f(t + 0x10u), f(c + 0x10u)));
                poke32(c + 0x40u, 0u);
            }
            if (peek32(c + 0x40u) == 0u)
                textPlace(c);
        }

        // 0x11E9B0: the end of a measure, the widest line kept.
        void textMeasureEnd(TextCall &call)
        {
            const uint32_t c = call.c, t = kTextState;
            if (f(t + 0x10u) < f(c + 0x58u))
                setF(t + 0x10u, f(c + 0x58u));
            if (peek32(c + 0x54u) != 0u)
            {
                setF(t + 0x10u, ps2_fsub(f(t + 0x10u), f(c + 0x10u)));
                poke32(c + 0x40u, 0u);
            }
            textPlace(c);
        }

        void textRunBuffer(TextCall &call)
        {
            if (call.mode != 0)
                textDraw(call);
            else
                textMeasure(call);
        }

        // The whole call on the guest stack frame the game's would use; false: the formatter left the call (not seen).
        void textCall(TextCall &call, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t string, uint32_t argument)
        {
            State &s = g_state;
            const uint32_t t = kTextState;
            const uint32_t sp = getRegU32(ctx, 29) - 0x4D0u;
            call.frame = sp;
            call.c = sp + 0x410u;
            call.buffer = sp + 0x10u;
            const uint32_t c = call.c;
            poke32(sp, string);
            poke32(sp + 4u, argument);
            const uint32_t font = peek8(t);
            poke32(c + 0x14u, kFontDescs + font * 28u);
            poke32(c + 0x18u, peek32(kFontSt + font * 4u));
            poke32(c + 0x1Cu, peek32(kFontKerning + font * 4u));
            poke64(c + 0x38u, peek64(t + 0x18u));
            poke64(c + 0x30u, peek64(t + 0x10u));
            poke32(c + 0x28u, 0u);
            poke32(c + 0x2Cu, 0u);
            poke16(c + 0x44u, 0u);
            poke16(c + 0x46u, 0u);
            poke32(c + 0x3Cu, 0u);
            poke32(c + 0x10u, 0u);
            poke32(c + 0x50u, 0u);
            poke32(c + 0x54u, 0u);
            float scale = 1.0f;
            if (peek32(peek32(kScreenMode) + 0x50u) != 0u)
            {
                const uint32_t bits = 0x3F4DB9CCu;
                float k;
                std::memcpy(&k, &bits, 4);
                scale = ps2_fmul(scale, k);
            }
            setF(c + 0x5Cu, scale);
            if (call.mode <= 0)
            {
                textPlace(c);
                call.mode = -call.mode;
            }
            else
            {
                const float px = f(t + 0x30u), py = f(t + 0x34u);
                setF(c + 0x0u, px);
                setF(c + 0x4u, py);
                poke32(c + 0x40u, zWord(f(t + 0x38u)));
                setF(c + 0x8u, ps2_fadd(px, f(t + 0x48u)));
                setF(c + 0xCu, ps2_fadd(py, f(t + 0x4Cu)));
                poke16(c + 0x4Cu, static_cast<uint16_t>(ps2FpuCvtWS(f(t + 0x60u))));
            }
            poke32(c + 0x60u, call.buffer);
            poke64(call.buffer, 0u);
            if (call.mode != 0)
            {
                textState(call, 1u);
                textState(call, 2u);
            }
            else
            {
                const uint32_t bits = 0xCCBEBC20u;
                float v;
                std::memcpy(&v, &bits, 4);
                setF(c + 0x58u, v);
            }
            for (;;)
            {
                const uint32_t str = peek32(sp);
                const uint8_t code = peek8(str);
                poke32(sp, str + 1u);
                if (code == 0u)
                    break;
                if (code < 0x23u || code >= 0x26u)
                {
                    const uint32_t out = peek32(c + 0x60u);
                    poke8(out, code);
                    poke32(c + 0x60u, out + 1u);
                    continue;
                }
                if (peek8(call.buffer) != 0u)
                {
                    poke8(peek32(c + 0x60u), 0u);
                    textRunBuffer(call);
                    poke32(c + 0x60u, call.buffer);
                    poke64(call.buffer, 0u);
                }
                poke32(sp, peek32(sp) - 1u);
                const uint32_t savedSp = getRegU32(ctx, 29), savedRa = getRegU32(ctx, 31);
                const uint32_t savedPc = ctx->pc;
                SET_GPR_U32(ctx, 29, sp);
                SET_GPR_U32(ctx, 4, sp);
                SET_GPR_U32(ctx, 5, c + 0x60u);
                SET_GPR_U32(ctx, 6, sp + 4u);
                SET_GPR_U32(ctx, 7, c);
                SET_GPR_U32(ctx, 8, t);
                SET_GPR_U32(ctx, 31, kTextFormatReturn);
                ctx->pc = kTextFormat;
                runToReturn(s.format, rdram, ctx, runtime);
                const uint32_t what = getRegU32(ctx, 2);
                SET_GPR_U32(ctx, 29, savedSp);
                SET_GPR_U32(ctx, 31, savedRa);
                ctx->pc = savedPc;
                if (what != 0u && call.mode != 0)
                    textState(call, what);
            }
            poke8(peek32(c + 0x60u), 0u);
            if (peek8(call.buffer) != 0u)
                textRunBuffer(call);
            if (call.mode != 0)
                textEnd(call);
            else
                textMeasureEnd(call);
        }

        bool linkTextMarker(uint64_t key, uint32_t insert, uint32_t layer)
        {
            if (insert == kInsertList)
                return linkGifListMarker(key);
            int32_t l = static_cast<int32_t>(layer);
            if (insert == kInsertText)
            {
                l -= 1;
                l = l < 0 ? 0 : l >= 4 ? 3 : l;
                return linkMarker(key, 0x1FF3 + l);
            }
            l = l < 0 ? 0 : l >= 255 ? 255 : l;
            return linkMarker(key, ps2FpuCvtWS(ps2_fmul(ps2_fmul(static_cast<float>(l), 8191.0f), 0.00390625f)));
        }

        GsVertex glyphVertex(uint64_t st, uint64_t rgbaq, uint64_t xyz)
        {
            GsVertex v;
            v.x = static_cast<float>(xyz & 0xFFFFu) / 16.0f;
            v.y = static_cast<float>((xyz >> 16) & 0xFFFFu) / 16.0f;
            v.z = static_cast<uint32_t>(xyz >> 32) & 0xFFFFFFu;
            uint32_t bits = static_cast<uint32_t>(st);
            std::memcpy(&v.s, &bits, 4);
            bits = static_cast<uint32_t>(st >> 32);
            std::memcpy(&v.t, &bits, 4);
            bits = static_cast<uint32_t>(rgbaq >> 32);
            std::memcpy(&v.q, &bits, 4);
            v.rgba = static_cast<uint32_t>(rgbaq);
            return v;
        }

        // A packet's glyphs drawn at a marker: batches split where TEX2 or ALPHA changes; false: a texture missing (the
        // whole text then goes to the GS).
        bool textMaterials(const TextPacket &packet, uint64_t &tex2, uint64_t &alpha, std::vector<std::pair<GsMaterial, std::vector<GsVertex>>> &out)
        {
            State &s = g_state;
            for (const TextWrite &w : packet.writes)
            {
                if (w.reg == 0x17u)
                {
                    tex2 = w.w[0];
                    continue;
                }
                if (w.reg == 0x43u)
                {
                    alpha = w.w[0];
                    continue;
                }
                if (w.reg != kGlyph)
                    continue;
                if (out.empty() || out.back().first.alpha != alpha || out.back().first.tex0.cbp != ((tex2 >> 37) & 0x3FFFu))
                {
                    Page p;
                    if (!page(kFontFile, 0u, p, s.text, static_cast<uint32_t>((tex2 >> 37) & 0x3FFFu)))
                        return false;
                    p.tex0 = (p.tex0 & ~kTex2Fields) | (tex2 & kTex2Fields);
                    out.emplace_back(spriteMaterial(p, alpha), std::vector<GsVertex>{});
                }
                out.back().second.push_back(glyphVertex(w.w[1], w.w[2], w.w[3]));
                out.back().second.push_back(glyphVertex(w.w[4], w.w[2], w.w[5]));
            }
            return true;
        }

        void textInsert(TextCall &call)
        {
            State &s = g_state;
            if (call.dry)
                return;
            const TextPacket &packet = call.run.packets.back();
            std::vector<std::pair<GsMaterial, std::vector<GsVertex>>> batches;
            uint64_t tex2 = 0, alpha = 0;
            for (const TextPacket &p : call.run.packets)
                if (&p != &packet)
                    for (const TextWrite &w : p.writes)
                    {
                        if (w.reg == 0x17u)
                            tex2 = w.w[0];
                        if (w.reg == 0x43u)
                            alpha = w.w[0];
                    }
            if (!textMaterials(packet, tex2, alpha, batches))
            {
                ++s.text.toGs;
                return;
            }
            const uint64_t key = newKey();
            size_t glyphs = 0;
            for (auto &[material, vertices] : batches)
            {
                ps2x::native::Renderer::instance().drawGsKeyed(key, material, vertices.data(), static_cast<uint32_t>(vertices.size()));
                glyphs += vertices.size() / 2u;
            }
            if (!linkTextMarker(key, packet.insert, packet.layer))
            {
                discardKeyed(key);
                return;
            }
            s.text.sprites += glyphs;
            ++s.text.markers;
        }

        // The game's packet as its insertion gets it (check mode): its A+D writes and glyphs, from the scratchpad.
        TextPacket capturedPacket(uint32_t end, uint32_t start, uint32_t insert, uint32_t layer)
        {
            TextPacket p;
            p.insert = insert;
            p.layer = layer;
            for (uint32_t q = start + 16u; q + 16u <= end;)
            {
                const uint64_t tag = peek64(q), regs = peek64(q + 8u);
                const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu), flg = static_cast<uint32_t>((tag >> 58) & 3u);
                q += 16u;
                if (flg == 0u && (regs & 0xFu) == 0xEu)
                    for (uint32_t i = 0; i < nloop; ++i, q += 16u)
                    {
                        TextWrite w;
                        w.reg = static_cast<uint32_t>(peek64(q + 8u) & 0xFFu);
                        w.w[0] = peek64(q);
                        p.writes.push_back(w);
                    }
                else if (flg == 1u && regs == kGlyphRegs && (tag >> 60) == 6u)
                    for (uint32_t i = 0; i < nloop; ++i, q += 48u)
                    {
                        TextWrite w;
                        w.reg = kGlyph;
                        for (uint32_t k = 0; k < 6u; ++k)
                            w.w[k] = peek64(q + k * 8u);
                        p.writes.push_back(w);
                    }
                else
                {
                    TextWrite w;
                    w.reg = 0xFFFEu;
                    w.w[0] = tag;
                    w.w[1] = regs;
                    p.writes.push_back(w);
                    break;
                }
            }
            return p;
        }

        void compareText(const TextCall &call, uint32_t string, const std::vector<uint8_t> &before, const std::vector<uint8_t> &nativeState,
                         const std::vector<TextPacket> &game)
        {
            State &s = g_state;
            BuilderStats &stats = s.text;
            ++stats.checked;
            std::vector<uint8_t> after(kTextStateSize);
            std::memcpy(after.data(), s.rdram + kTextState, kTextStateSize);
            const auto log = [&](const std::string &what)
            {
                if (++stats.checkDiffs > 12u)
                    return;
                if (stats.checkDiffs <= 3u)
                {
                    std::string text, words;
                    for (uint32_t i = 0; i < 48u && peek8(string + i) != 0u; ++i)
                    {
                        const uint8_t ch = peek8(string + i);
                        char item[8];
                        std::snprintf(item, sizeof(item), ch >= 0x20u && ch < 0x7Fu ? "%c" : "<%02x>", ch);
                        text += item;
                    }
                    for (uint32_t i = 0; i < kTextStateSize; i += 4u)
                    {
                        uint32_t w;
                        std::memcpy(&w, &before[i], 4);
                        char item[16];
                        std::snprintf(item, sizeof(item), " %x", w);
                        words += item;
                    }
                    std::fprintf(stderr, "[sc3:2d] check text: string \"%s\", state before%s\n", text.c_str(), words.c_str());
                }
                std::fprintf(stderr, "[sc3:2d] check text mode %d: %s\n", call.mode, what.c_str());
            };
            for (uint32_t i = 0; i < kTextStateSize; i += 4u)
                if (std::memcmp(&after[i], &nativeState[i], 4) != 0)
                {
                    uint32_t a, b;
                    std::memcpy(&a, &nativeState[i], 4);
                    std::memcpy(&b, &after[i], 4);
                    char line[96];
                    std::snprintf(line, sizeof(line), "state +%#x native %#x game %#x", i, a, b);
                    log(line);
                }
            const std::vector<TextPacket> &mine = call.run.packets;
            if (mine.size() != game.size())
            {
                ++stats.checkCountDiffs;
                log("packets native " + std::to_string(mine.size()) + " game " + std::to_string(game.size()));
                return;
            }
            for (size_t p = 0; p < mine.size(); ++p)
            {
                const TextPacket &a = mine[p], &b = game[p];
                if (a.insert != b.insert || a.layer != b.layer || a.writes.size() != b.writes.size())
                {
                    ++stats.checkCountDiffs;
                    char line[128];
                    std::snprintf(line, sizeof(line), "packet %zu: native insert %#x layer %u writes %zu; game %#x %u %zu", p, a.insert, a.layer,
                                  a.writes.size(), b.insert, b.layer, b.writes.size());
                    log(line);
                    continue;
                }
                for (size_t i = 0; i < a.writes.size(); ++i)
                {
                    const TextWrite &x = a.writes[i], &y = b.writes[i];
                    if (x.reg == kGlyph)
                        ++stats.checkSprites;
                    const size_t words = x.reg == kGlyph ? 6u : 1u;
                    if (x.reg == y.reg && std::memcmp(x.w, y.w, words * 8u) == 0)
                        continue;
                    char line[256];
                    std::snprintf(line, sizeof(line), "packet %zu write %zu reg %#x/%#x: native %llx %llx %llx %llx; game %llx %llx %llx %llx", p, i, x.reg, y.reg,
                                  static_cast<unsigned long long>(x.w[0]), static_cast<unsigned long long>(x.w[1]),
                                  static_cast<unsigned long long>(x.w[2]), static_cast<unsigned long long>(x.w[3]),
                                  static_cast<unsigned long long>(y.w[0]), static_cast<unsigned long long>(y.w[1]),
                                  static_cast<unsigned long long>(y.w[2]), static_cast<unsigned long long>(y.w[3]));
                    log(line);
                }
            }
        }

        void text(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.text;
            ++stats.calls;
            if (s.mode == Mode::Off || !s.format)
            {
                callOriginal(kText, rdram, ctx, runtime);
                return;
            }
            const int32_t mode = static_cast<int32_t>(getRegU32(ctx, 4));
            const uint32_t string = getRegU32(ctx, 5), argument = getRegU32(ctx, 6);
            if (s.mode == Mode::Check)
            {
                std::vector<uint8_t> before(kTextStateSize), nativeState(kTextStateSize), globals(kTextGlobalsSize);
                std::memcpy(before.data(), rdram + kTextState, kTextStateSize);
                std::memcpy(globals.data(), rdram + kTextGlobals, kTextGlobalsSize);
                uint8_t scratch[16];
                std::memcpy(scratch, s.scratchpad + 0x50u, 16);
                __m128i gprs[32];
                float fprs[32];
                std::memcpy(gprs, ctx->r, sizeof(gprs));
                std::memcpy(fprs, ctx->f, sizeof(fprs));
                const uint32_t pc = ctx->pc;
                TextCall call;
                call.mode = mode;
                call.dry = true;
                textCall(call, rdram, ctx, runtime, string, argument);
                std::memcpy(ctx->r, gprs, sizeof(gprs));
                std::memcpy(ctx->f, fprs, sizeof(fprs));
                ctx->pc = pc;
                std::memcpy(nativeState.data(), rdram + kTextState, kTextStateSize);
                std::memcpy(rdram + kTextGlobals, globals.data(), kTextGlobalsSize);
                std::memcpy(s.scratchpad + 0x50u, scratch, 16);
                s.capture = true;
                s.captured.clear();
                runOriginal(kText, rdram, ctx, runtime);
                s.capture = false;
                compareText(call, string, before, nativeState, s.captured);
                return;
            }
            Page font;
            if (mode != 0 && (!page(kFontFile, 0u, font, stats)))
            {
                ++stats.toGs;
                callOriginal(kText, rdram, ctx, runtime);
                return;
            }
            TextCall call;
            call.mode = mode;
            textCall(call, rdram, ctx, runtime, string, argument);
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- the quadword words of the packets a builder links into a bucket, for check mode

        using Qword = std::array<uint32_t, 4>;

        // The data quadwords the game linked in front of `stop` from `head` in one bucket: per block, the quadwords after
        // its NEXT tag (QWC of them), newest first.
        std::vector<std::vector<Qword>> packetQwords(uint32_t head, uint32_t stop)
        {
            std::vector<std::vector<Qword>> out;
            for (uint32_t block = head, guard = 0; block != stop && block != 0u && guard < 64u; ++guard)
            {
                uint32_t q = block, next = 0, qwc = 0;
                for (uint32_t i = 0; i < 16u; ++i, q += 16u)
                    if (((peek64(q) >> 28) & 7u) == 2u)
                    {
                        next = static_cast<uint32_t>(peek64(q) >> 32);
                        qwc = static_cast<uint32_t>(peek64(q) & 0xFFFFu);
                        break;
                    }
                if (next == 0u)
                    break;
                std::vector<Qword> &words = out.emplace_back();
                for (uint32_t i = 1; i <= qwc; ++i)
                    words.push_back({peek32(q + i * 16u), peek32(q + i * 16u + 4u), peek32(q + i * 16u + 8u), peek32(q + i * 16u + 12u)});
                block = next;
            }
            return out;
        }

        void compareQwords(BuilderStats &stats, const std::vector<Qword> &native, const std::vector<Qword> &game, const char *name)
        {
            ++stats.checked;
            if (native.size() != game.size())
            {
                if (++stats.checkCountDiffs <= 4u)
                    std::fprintf(stderr, "[sc3:2d] check %s: %zu quadwords native, %zu game\n", name, native.size(), game.size());
                return;
            }
            for (size_t i = 0; i < native.size(); ++i)
            {
                ++stats.checkSprites;
                if (native[i] == game[i])
                    continue;
                if (++stats.checkDiffs <= 8u)
                    std::fprintf(stderr, "[sc3:2d] check %s qword %zu: native %08x %08x %08x %08x; game %08x %08x %08x %08x\n", name, i, native[i][0],
                                 native[i][1], native[i][2], native[i][3], game[i][0], game[i][1], game[i][2], game[i][3]);
            }
        }

        // The block a 0x120978 insertion of the game put in front (check mode): by the mode word, the GIF list's cursor
        // before the call (mode 0: CNT then its data) or the bucket the depth picks (modes 1 .. 3).
        struct InsertWatch
        {
            uint32_t mode = 0, entry = 0, before = 0;
        };

        InsertWatch watchInsert(uint32_t mode, float depth)
        {
            InsertWatch w;
            w.mode = mode;
            if (w.mode == 0u)
            {
                w.before = peek32(kRenderer + 8u);
                return w;
            }
            int bucket = -1;
            if (w.mode == 1u && depth >= 0.0f && depth <= 12.5f)
                bucket = truncate(std::min(depth * 0.12f, 1.0f) * peekFloat(kRenderer + 0x34u)), w.entry = peek32(kRenderer + 0x50u);
            else if (w.mode == 2u)
                bucket = depth < 0.0f ? 0x1FEF : depth < 0.5f ? truncate(depth * 15.0f) + 0x1FEA : depth < 1.0f ? truncate(depth * 15.0f) + 0x1FEF : 0x1FFE;
            else if (w.mode == 3u)
                bucket = depth < 0.0f ? 0x1FEA : depth < 1.0f ? truncate(depth * 3.0f) + 0x1FF3 : 0x1FF6;
            if (bucket < 0 || bucket >= 0x2000)
                return w;
            if (w.mode != 1u)
                w.entry = peek32(kRenderer + 0x54u);
            w.entry += static_cast<uint32_t>(bucket) * 16u;
            w.before = peek32(w.entry + 4u);
            return w;
        }

        InsertWatch watchInsert(float depth) { return watchInsert(peek32(kInsertMode), depth); }

        std::vector<Qword> insertedQwords(const InsertWatch &w)
        {
            if (w.mode == 0u)
            {
                std::vector<Qword> out;
                const uint32_t qwc = peek32(w.before) & 0xFFFFu;
                for (uint32_t i = 1; i <= qwc && peek32(kRenderer + 8u) > w.before; ++i)
                    out.push_back({peek32(w.before + i * 16u), peek32(w.before + i * 16u + 4u), peek32(w.before + i * 16u + 8u),
                                   peek32(w.before + i * 16u + 12u)});
                return out;
            }
            if (w.entry == 0u)
                return {};
            const std::vector<std::vector<Qword>> blocks = packetQwords(peek32(w.entry + 4u), w.before);
            return blocks.size() == 1u ? blocks[0] : std::vector<Qword>{};
        }

        GsMaterial flatMaterial(GSPrimType type, uint32_t primBits, uint64_t alpha)
        {
            GsMaterial m;
            m.prim.type = type;
            m.prim.iip = (primBits & 0x8u) != 0u;
            m.prim.abe = (primBits & 0x40u) != 0u;
            m.prim.aa1 = (primBits & 0x80u) != 0u;
            m.prim.ctxt = (primBits & 0x200u) != 0u;
            m.alpha = alpha;
            m.testFromTarget = true;
            return m;
        }

        uint64_t reg64(const R5900Context *ctx, int reg)
        {
            return getRegU32(ctx, reg) | (static_cast<uint64_t>(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[reg], 1))) << 32);
        }

        // ---- 0x123340 (a0 -> the corner (x, y, z, w), a1 RGBA, a2 ALPHA_2 (0: 0x2A, alpha byte 0x80), f12 / f13 the size):
        // an untextured blended sprite (PRIM 0x246), inserted through 0x120978 at the corners' mean Q times w (-w when w < 0)

        constexpr uint32_t kQuad = 0x00123340u;

        void quad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.quads;
            ++stats.calls;
            if (s.mode == Mode::Off)
            {
                stats.gifQwords += 7u;
                callOriginal(kQuad, rdram, ctx, runtime);
                return;
            }
            // In the GIF list (mode 0) the quad's ALPHA_2 is the one the textured strips 0x1134A0 inserted after it draw with
            // (they set none): the game's packet until that builder is native.
            if (s.mode == Mode::On && peek32(kInsertMode) == 0u)
            {
                ++stats.toGs;
                stats.gifQwords += 7u;
                callOriginal(kQuad, rdram, ctx, runtime);
                return;
            }
            const uint32_t p = getRegU32(ctx, 4);
            uint32_t rgba = getRegU32(ctx, 5);
            uint64_t alpha = reg64(ctx, 6);
            if (alpha == 0u)
            {
                alpha = 0x2Au;
                rgba = (rgba & 0x00FFFFFFu) | 0x80000000u;
            }
            const float dx = ctx->f[12], dy = ctx->f[13];
            const float x = peekFloat(p), y = peekFloat(p + 4u), z = peekFloat(p + 8u), w = peekFloat(p + 12u);
            float m[16];
            viewMatrix(m);
            const GsPoint p0 = toGs(m, x, y, z);
            const float q0 = peekFloat(kScratchpad + 0x58u);
            const GsPoint p1 = toGs(m, x + dx, y + dy, z + 0.0f);
            const float q1 = peekFloat(kScratchpad + 0x58u);
            float depth = (q0 + q1) * 0.5f;
            depth = w < 0.0f ? -w : depth * w;
            const uint32_t colour[4] = {rgba & 0xFFu, (rgba >> 8) & 0xFFu, (rgba >> 16) & 0xFFu, rgba >> 24};
            if (s.mode == Mode::Check)
            {
                const std::vector<Qword> expected = {
                    {0x00008001u, 0x41234000u, 0x0000441Eu, 0u},
                    {static_cast<uint32_t>(alpha), static_cast<uint32_t>(alpha >> 32), 0x43u, 0u},
                    {colour[0], colour[1], colour[2], colour[3]},
                    {static_cast<uint32_t>(p0.x), static_cast<uint32_t>(p0.y), static_cast<uint32_t>(p0.z), 0u},
                    {static_cast<uint32_t>(p1.x), static_cast<uint32_t>(p1.y), static_cast<uint32_t>(p1.z), 0u},
                };
                const InsertWatch watch = watchInsert(depth);
                stats.gifQwords += 7u;
                runOriginal(kQuad, rdram, ctx, runtime);
                if (peek32(kRenderer) == 0u)
                    compareQwords(stats, expected, insertedQwords(watch), "quads");
                return;
            }
            const uint64_t key = newKey();
            std::vector<GsVertex> corners;
            spriteCorners(corners, p0, p1, 0.0f, 0.0f, 0.0f, 0.0f, colour[0] | (colour[1] << 8) | (colour[2] << 16) | (colour[3] << 24));
            ps2x::native::Renderer::instance().drawGsKeyed(key, flatMaterial(GS_PRIM_SPRITE, 0x246u, alpha), corners.data(),
                                                           static_cast<uint32_t>(corners.size()));
            const Linked linked = insert2d(key, depth);
            if (linked != Linked::Yes)
                discardKeyed(key);
            if (linked == Linked::NotHere)
            {
                ++stats.toGs;
                callOriginal(kQuad, rdram, ctx, runtime);
                return;
            }
            if (linked == Linked::Yes)
            {
                ++stats.sprites;
                ++stats.markers;
            }
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- 0x1223D0 (a0 / a1 -> the ends as packed XYZ2 quadwords, a2 RGBA, a3 ALPHA_2, f12 the depth): one
        // antialiased blended line (PRIM 0x2C1), inserted through 0x120978

        constexpr uint32_t kLine = 0x001223D0u;

        void line(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.lines;
            ++stats.calls;
            if (s.mode == Mode::Off)
            {
                stats.gifQwords += 7u;
                callOriginal(kLine, rdram, ctx, runtime);
                return;
            }
            uint32_t xyz[2][4];
            for (int i = 0; i < 2; ++i)
                for (int w = 0; w < 4; ++w)
                    xyz[i][w] = peek32(getRegU32(ctx, 4 + i) + static_cast<uint32_t>(w) * 4u);
            const uint32_t rgba = getRegU32(ctx, 6);
            const uint64_t alpha = reg64(ctx, 7);
            if (s.mode == Mode::Check)
            {
                const std::vector<Qword> expected = {
                    {0x00008001u, 0x4160C000u, 0x0000441Eu, 0u},
                    {static_cast<uint32_t>(alpha), static_cast<uint32_t>(alpha >> 32), 0x43u, 0u},
                    {rgba & 0xFFu, (rgba >> 8) & 0xFFu, (rgba >> 16) & 0xFFu, rgba >> 24},
                    {xyz[0][0], xyz[0][1], xyz[0][2], xyz[0][3]},
                    {xyz[1][0], xyz[1][1], xyz[1][2], xyz[1][3]},
                };
                const InsertWatch watch = watchInsert(ctx->f[12]);
                stats.gifQwords += 7u;
                runOriginal(kLine, rdram, ctx, runtime);
                if (peek32(kRenderer) == 0u)
                    compareQwords(stats, expected, insertedQwords(watch), "lines");
                return;
            }
            const uint64_t key = newKey();
            const bool drawn = (xyz[1][3] & 0x8000u) == 0u;
            if (drawn)
            {
                GsVertex ends[2];
                for (int i = 0; i < 2; ++i)
                {
                    ends[i].x = static_cast<float>(xyz[i][0] & 0xFFFFu) / 16.0f;
                    ends[i].y = static_cast<float>(xyz[i][1] & 0xFFFFu) / 16.0f;
                    ends[i].z = xyz[i][2];
                    ends[i].rgba = rgba;
                }
                ps2x::native::Renderer::instance().drawGsKeyed(key, flatMaterial(GS_PRIM_LINE, 0x2C1u, alpha), ends, 2u);
            }
            const Linked linked = insert2d(key, ctx->f[12]);
            if (linked != Linked::Yes && drawn)
                discardKeyed(key);
            if (linked == Linked::NotHere)
            {
                ++stats.toGs;
                callOriginal(kLine, rdram, ctx, runtime);
                return;
            }
            if (linked == Linked::Yes && drawn)
            {
                ++stats.sprites;
                ++stats.markers;
            }
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- 0x1280F8 (a0 the icon, a1 -> float colour, a2 ALPHA_2, f12 / f13 x, y, f14 / f15 width, height): one textured
        // sprite of the menu icons' file (in the ELF at 0x4AF170, page 0) into bucket 0x1FF2. Icons 0x12 and up (another
        // builder, 0x127AD8) and the first call (the file's set-up, 0x128038) are the game's.

        constexpr uint32_t kIcon = 0x001280F8u;
        constexpr uint32_t kIconFile = 0x004AF170u;
        constexpr uint32_t kIconSt = 0x005BECA0u;
        constexpr uint32_t kIconDepth = 0x00468D5Cu, kIconReady = 0x00468D58u, kIconUpload = 0x00468D60u;
        constexpr uint32_t kNoGsPackets = 0x0046E5DBu;
        constexpr uint32_t kDefaultDepth = 0x0011FF98u;

        // 0x11AAA0 (colour, scale): a float colour's RGBAQ words, int(c.xyz * scale), int(c.w * 128), each at most 255.
        void colourWords(uint32_t colour, float scale, uint32_t out[4])
        {
            for (int i = 0; i < 4; ++i)
            {
                const float c = peekFloat(colour + static_cast<uint32_t>(i) * 4u);
                out[i] = static_cast<uint32_t>(std::min(ps2FpuCvtWS(ps2_fmul(c, i < 3 ? scale : 128.0f)), 255));
            }
        }

        void icon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.icons;
            ++stats.calls;
            const int32_t asked = static_cast<int32_t>(getRegU32(ctx, 4));
            const uint32_t index = static_cast<uint32_t>(std::min(std::max(asked, 0), 0x39));
            Page pageRegs;
            if (s.mode == Mode::Off || peek8(kNoGsPackets) != 0u || index >= 0x12u || peek32(kIconReady) == 0u ||
                !page(kIconFile, 0u, pageRegs, stats))
            {
                if (s.mode != Mode::Off && peek8(kNoGsPackets) == 0u)
                    ++stats.toGs;
                stats.gifQwords += 9u;
                callOriginal(kIcon, rdram, ctx, runtime);
                return;
            }
            const uint32_t colourVec = getRegU32(ctx, 5);
            const uint64_t alpha = reg64(ctx, 6);
            const float x = ctx->f[12], y = ctx->f[13], width = ctx->f[14], height = ctx->f[15];
            float z = peekFloat(kIconDepth);
            if (z == 0.0f)
            {
                const uint32_t pc = ctx->pc, ra = getRegU32(ctx, 31);
                const float saved[4] = {ctx->f[12], ctx->f[13], ctx->f[14], ctx->f[15]};
                SET_GPR_U32(ctx, 31, kIcon + 0x8u);
                ctx->pc = kDefaultDepth;
                runToReturn(s.runtime->lookupFunction(kDefaultDepth), rdram, ctx, runtime);
                z = ctx->f[0];
                SET_GPR_U32(ctx, 31, ra);
                ctx->pc = pc;
                for (int i = 0; i < 4; ++i)
                    ctx->f[12 + i] = saved[i];
            }
            float m[16];
            viewMatrix(m);
            const float q = 1.0f / z;
            float at[3];
            for (int i = 0; i < 3; ++i)
            {
                float acc = m[i] * x;
                acc = acc + m[4 + i] * y;
                acc = acc + m[8 + i] * z;
                at[i] = acc + m[12 + i];
            }
            at[2] = std::max(at[2] * q, 0.0f);
            for (int i = 0; i < 3; ++i)
                pokeFloat(kScratchpad + 0x50u + static_cast<uint32_t>(i) * 4u, q);
            const float sx = width * 21.0f, sy = height * 21.0f;
            uint32_t colour[4];
            colourWords(colourVec, 128.0f, colour);
            const uint32_t st = kIconSt + index * 16u;
            const GsPoint p0{ftoi4(at[0]), ftoi4(at[1]), ftoi4(at[2])};
            const GsPoint p1{ftoi4(at[0] + sx), ftoi4(at[1] + sy), ftoi4(at[2] + 0.0f)};
            const uint32_t s0 = peek32(st), t0 = peek32(st + 4u), s1 = peek32(st + 8u), t1 = peek32(st + 12u);
            if (s.mode == Mode::Check)
            {
                const std::vector<Qword> expected = {
                    {0x00008001u, 0x612B4000u, 0x0042421Eu, 0u},
                    {static_cast<uint32_t>(alpha), static_cast<uint32_t>(alpha >> 32), 0x43u, 0u},
                    {colour[0], colour[1], colour[2], colour[3]},
                    {s0, t0, 0x3F800000u, 0u},
                    {static_cast<uint32_t>(p0.x), static_cast<uint32_t>(p0.y), static_cast<uint32_t>(p0.z), 0u},
                    {s1, t1, 0x3F800000u, 0u},
                    {static_cast<uint32_t>(p1.x), static_cast<uint32_t>(p1.y), static_cast<uint32_t>(p1.z), 0u},
                };
                const uint32_t entry = peek32(kRenderer + 0x54u) + static_cast<uint32_t>(kLabelBucket) * 16u;
                const uint32_t before = peek32(entry + 4u);
                stats.gifQwords += 9u;
                runOriginal(kIcon, rdram, ctx, runtime);
                if (peek32(kRenderer) == 0u)
                {
                    const std::vector<std::vector<Qword>> game = packetQwords(peek32(entry + 4u), before);
                    if (game.size() == 1u)
                        compareQwords(stats, expected, game[0], "icons");
                }
                return;
            }
            float fs0, ft0, fs1, ft1;
            std::memcpy(&fs0, &s0, 4), std::memcpy(&ft0, &t0, 4), std::memcpy(&fs1, &s1, 4), std::memcpy(&ft1, &t1, 4);
            std::vector<GsVertex> corners;
            spriteCorners(corners, p0, p1, fs0, ft0, fs1, ft1, colour[0] | (colour[1] << 8) | (colour[2] << 16) | (colour[3] << 24));
            const uint64_t key = newKey();
            ps2x::native::Renderer::instance().drawGsKeyed(key, spriteMaterial(pageRegs, alpha), corners.data(),
                                                           static_cast<uint32_t>(corners.size()));
            if (!linkMarker(key, kLabelBucket))
                discardKeyed(key);
            else
            {
                ++stats.sprites;
                ++stats.markers;
            }
            poke32(kIconUpload, 1u);
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- the once-a-frame uploads of the 2D texture files whose draws are native (0x11FCF0 the font, 0x138860 the
        // labels' file *(0x5BEE1C), 0x128080 the icons): CALLs of the file's upload lists and ALPHA_2 0x44 into the text and
        // label buckets, for the GS only. Dropped while the native 2D is on and the file's page is in the pack (else the
        // builders fall back to the GS, which needs them).

        constexpr uint32_t kFontUpload = 0x0011FCF0u, kLabelsUpload = 0x00138860u, kIconsUpload = 0x00128080u;

        void upload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t at = ctx->pc;
            const uint32_t file = at == kFontUpload ? kFontFile : at == kIconsUpload ? kIconFile : peek32(k2d + 0x4Cu);
            Page p;
            BuilderStats unused;
            if (s.mode != Mode::On || !page(file, 0u, p, unused))
            {
                callOriginal(at, rdram, ctx, runtime);
                return;
            }
            ++s.uploadsDropped;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- the screen-effect layer: the background behind the 3D (a sky band and a ground band split at the horizon,
        // context 1, Z written 0), built inline in the GIF list by 0x12C690 or into a slot it leaves there by 0x12C7E0.

        constexpr uint32_t kEffectSlot = 0x00469210u;      // the GIF list slot 0x12C690 left for 0x12C7E0 (0: none)
        constexpr uint32_t kEffectCamera = 0x004696C0u;    // 0x12B7D0's a1: the camera / light holder
        constexpr uint32_t kEffectGlobals = 0x00469234u;   // pointer: +0x10 / +0x108 the TEST left after, +0x70 slot or inline
        constexpr uint32_t kNoPacketsByte = 0x0046E5DBu;   // set: the layer builds nothing
        constexpr uint32_t kInlineByte = 0x0046E5D1u;      // set: 0x12C690 builds inline even when +0x70 asks for a slot
        constexpr uint32_t kXyoffsetShadow = 0x00470F80u;  // +0x28 / +0x2C: XYOFFSET_1 as the game last wrote it
        constexpr uint32_t kFillList = 0x005BEE80u;        // 0x13CCA8's prebuilt full-screen sprites (15 quadwords)
        constexpr uint32_t kGifListCursor = 0x005BE708u;   // 0x10DB68: list address = *(+8) + (p - *(0x70000000))
        constexpr uint32_t kRoll = 0x00135F78u, kLight = 0x00135C68u;
        constexpr uint32_t kEffectInline = 0x0012C690u, kEffectLayer = 0x0012C7E0u;

        struct EffectDraw
        {
            GSPrimType type = GS_PRIM_SPRITE;
            bool abe = false;
            uint64_t alpha = 0, test = 0;
            std::vector<GsVertex> vertices; // sprite corners, XYOFFSET_1 (0x6C00, 0x7200) frame
        };

        struct EffectBuild
        {
            std::vector<Qword> words; // the GIF data after the CNT header (check mode)
            std::vector<EffectDraw> draws;
            bool toGame = false;      // a path not drafted: the caller runs the original
        };

        void effectWord(EffectBuild &b, uint64_t lo, uint64_t hi)
        {
            b.words.push_back({static_cast<uint32_t>(lo), static_cast<uint32_t>(lo >> 32), static_cast<uint32_t>(hi), static_cast<uint32_t>(hi >> 32)});
        }

        // 0x11AAA0 (colour, 255): RGBAQ words, min(vftoi0(c.xyz * 255), 255) and min(cvt.w.s(c.w * 128), 255) (signed min).
        void effectWords255(const float c[4], int32_t out[4])
        {
            for (int i = 0; i < 3; ++i)
                out[i] = std::min(ps2FpuCvtWS(c[i] * 255.0f), 255);
            out[3] = std::min(ps2FpuCvtWS(ps2_fmul(c[3], 128.0f)), 255);
        }

        uint32_t effectRgba(const int32_t w[4], bool alpha)
        {
            return (static_cast<uint32_t>(w[0]) & 0xFFu) | ((static_cast<uint32_t>(w[1]) & 0xFFu) << 8) | ((static_cast<uint32_t>(w[2]) & 0xFFu) << 16) |
                   (alpha ? (static_cast<uint32_t>(w[3]) & 0xFFu) << 24 : 0u);
        }

        GsVertex effectVertex(int32_t x, int32_t y, uint32_t rgba)
        {
            GsVertex v;
            v.x = static_cast<float>(x & 0xFFFF) / 16.0f;
            v.y = static_cast<float>(y & 0xFFFF) / 16.0f;
            v.z = 0u;
            v.rgba = rgba;
            v.fog = 0u;
            return v;
        }

        // The TEST_1 the layer leaves (0x13D0B0 / 0x12B658): 0x5002D, or 0x30000 when the globals' +0x10 is 0 or bits 2-3 of
        // +0x108 are set.
        uint64_t effectTestAfter()
        {
            const uint32_t g = peek32(kEffectGlobals);
            if (peek32(g + 0x10u) == 0u)
                return 0x30000u;
            return ((peek64(g + 0x108u) >> 2) & 3u) != 0u ? 0x30000u : 0x5002Du;
        }

        // 0x13CDC0 (packet, mode, x0, y0, x1, y1 (12.4, XYOFFSET (0x6C00, 0x7200) frame), colour vec4, &count): one colour
        // over a rectangle, untextured sprites (PRIM 6, context 1, no ABE) in 64-pixel columns; TEST_1 by mode (1 0x1001 colour
        // only, 2 0x32001 Z only, 3 0x30000 both; 0 or other: nothing); RGBA from 0x11AAF8(colour, 255) with alpha 0.
        // Full screen (0x6C00, 0x7200, 0x9400, 0x8E00): the prebuilt list 0x5BEE80 (XYOFFSET_1 0, ten columns 0..640 x 0..448,
        // XYOFFSET_1 back, and its NLOOP 2 A+D tag's second write the TEST after). Else the rectangle moved into the current
        // XYOFFSET_1 (shadow 0x470FA8 / 0x470FAC), clamped to 0..0xFFFF.
        void effectRect(EffectBuild &b, uint32_t mode, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const float colour[4])
        {
            uint64_t test;
            if (mode == 1u)
                test = 0x1001u;
            else if (mode == 2u)
                test = 0x32001u;
            else if (mode == 3u)
                test = 0x30000u;
            else
                return;
            int32_t w[4];
            for (int i = 0; i < 3; ++i)
                w[i] = std::min(ps2FpuCvtWS(colour[i] * 255.0f), 255);
            w[3] = 0;
            const uint32_t rgba = effectRgba(w, false);
            const auto colourWord = [&]() { b.words.push_back({static_cast<uint32_t>(w[0]) & 0xFFu, static_cast<uint32_t>(w[1]) & 0xFFu,
                                                               static_cast<uint32_t>(w[2]) & 0xFFu, 0u}); };
            EffectDraw d;
            d.test = test;
            if (x0 == 0x6C00 && y0 == 0x7200 && x1 == 0x9400 && y1 == 0x8E00)
            {
                effectWord(b, 0x2000000000000001ull, 0x1Eu);
                effectWord(b, test, 0x47u);
                colourWord();
                effectWord(b, 0x1003400000000001ull, 0xEu);
                effectWord(b, 0u, 0x18u);
                effectWord(b, 0x240000000000000Aull, 0x4Cu);
                for (int32_t i = 0; i < 10; ++i)
                {
                    const uint64_t lo = static_cast<uint64_t>(0x400 * i), hi = static_cast<uint64_t>(0x3F8 + 0x400 * i) | (0x1BF8ull << 16);
                    effectWord(b, lo, hi);
                    d.vertices.push_back(effectVertex(0x6C00 + 0x400 * i, 0x7200, rgba));
                    d.vertices.push_back(effectVertex(0x6C00 + 0x3F8 + 0x400 * i, 0x7200 + 0x1BF8, rgba));
                }
                effectWord(b, 0x1000000000008002ull, 0xEu);
                effectWord(b, 0x0000720000006C00ull, 0x18u);
                effectWord(b, effectTestAfter(), 0x47u);
                b.draws.push_back(std::move(d));
                return;
            }
            const int32_t ox = 0x6C00 - static_cast<int32_t>(peek16(kXyoffsetShadow + 0x28u));
            const int32_t oy = 0x7200 - static_cast<int32_t>(peek16(kXyoffsetShadow + 0x2Cu));
            const auto clamp = [](int32_t v) { return std::min(std::max(v, 0), 0xFFFF); };
            int32_t a = clamp(x0 - ox), c = clamp(x1 - ox);
            const int32_t top = clamp(y0 - oy), bottom = clamp(y1 - oy);
            effectWord(b, 0x2003400000000001ull, 0x1Eu);
            effectWord(b, test, 0x47u);
            colourWord();
            if (static_cast<uint32_t>(c) < static_cast<uint32_t>(a))
                std::swap(a, c);
            uint32_t n = ((static_cast<uint32_t>(c + 0x400) & 0xFFFFu) - (static_cast<uint32_t>(a) & 0xFFFFu)) >> 10;
            if (n == 0u)
                n = 1u;
            effectWord(b, 0x2000000000000000ull | n, 0x44u);
            int32_t x = a;
            for (uint32_t i = 0; i + 1u < n; ++i, x += 0x400)
            {
                b.words.push_back({static_cast<uint32_t>(x), static_cast<uint32_t>(top), 0u, 0u});
                b.words.push_back({static_cast<uint32_t>(x + 0x400), static_cast<uint32_t>(bottom), 0u, 0u});
                d.vertices.push_back(effectVertex(x + ox, top + oy, rgba));
                d.vertices.push_back(effectVertex(x + 0x400 + ox, bottom + oy, rgba));
            }
            b.words.push_back({static_cast<uint32_t>(x), static_cast<uint32_t>(top), 0u, 0u});
            b.words.push_back({static_cast<uint32_t>(c), static_cast<uint32_t>(bottom), 0u, 0u});
            d.vertices.push_back(effectVertex(x + ox, top + oy, rgba));
            d.vertices.push_back(effectVertex(c + ox, bottom + oy, rgba));
            effectWord(b, 0x1000000000008001ull, 0xEu);
            effectWord(b, effectTestAfter(), 0x47u);
            b.draws.push_back(std::move(d));
        }

        // 0x13B8C0 (packet, file, page 0, colour, flag A, flag B): 0x13CDC0 over the rectangle the file's page measures
        // (0x117E30: the used size in the page block's tag, centred on 0x8000), mode = A | B << 1.
        void effectFill(EffectBuild &b, uint32_t file, const float colour[4], uint32_t flagA, uint32_t flagB)
        {
            int32_t width = 0, height = 0;
            if (fileAddress(file) && (peek8(file + 5u) + peek8(file + 7u) * 256u) > 0u)
            {
                const uint32_t block = peek32(peek32(file + 0xCu)) & ~0xFu;
                const uint32_t used = peek32(block + 0xCu);
                width = static_cast<int32_t>(used & 0xFFFFu);
                height = static_cast<int32_t>(used >> 16);
            }
            const uint32_t mode = (flagA & 1u) | ((flagB & 1u) << 1);
            effectRect(b, mode, (0x1000 - width) << 3, (0x1000 - height) << 3, (width << 3) + 0x8000, (height << 3) + 0x8000, colour);
        }

        // 0x12B458 (packet, mode, x0, y0, split, x1, y1, colour A (above), colour B (below)): two bands of 64-pixel sprite
        // columns. Mode 2: ALPHA_1 0x48 first (0x127698, no EOP), PRIM 0x46 (ABE), both colours 0; mode 3: PRIM 6; mode 1
        // (0x13AA98 FRAME_1 / ZBUF_1 from a page): not drafted; mode 0: nothing. Then A+D TEST_1 0x30000, XYOFFSET_1
        // (0x6C00, 0x7200); full screen: 0x12B390 (ten fixed columns), else 0x12B290; A+D TEST_1 the TEST after (EOP).
        // 0x12B290 / 0x12B390's columns, as the GS gets them.
        void effectColumns(EffectBuild &b, EffectDraw &d, uint64_t prim, const int32_t w[4], int32_t xFirst, int32_t step, uint32_t n, int32_t xEndFirst,
                           int32_t yTop, int32_t yBottom, bool alpha)
        {
            effectWord(b, prim | 0x1000000000000001ull, 1u);
            b.words.push_back({static_cast<uint32_t>(w[0]), static_cast<uint32_t>(w[1]), static_cast<uint32_t>(w[2]), static_cast<uint32_t>(w[3])});
            effectWord(b, 0x2400000000000000ull | n, 0x4Cu);
            const uint32_t rgba = effectRgba(w, alpha);
            for (uint32_t i = 0; i < n; ++i)
            {
                const int32_t xa = xFirst + step * static_cast<int32_t>(i), xb = xEndFirst + step * static_cast<int32_t>(i);
                effectWord(b, (static_cast<uint64_t>(xa) & 0xFFFFu) | ((static_cast<uint64_t>(yTop) & 0xFFFFu) << 16),
                           (static_cast<uint64_t>(xb) & 0xFFFFu) | ((static_cast<uint64_t>(yBottom) & 0xFFFFu) << 16));
                d.vertices.push_back(effectVertex(xa, yTop, rgba));
                d.vertices.push_back(effectVertex(xb, yBottom, rgba));
            }
        }

        bool effectBands(EffectBuild &b, uint32_t mode, int32_t x0, int32_t y0, int32_t split, int32_t x1, int32_t y1, const float above[4],
                         const float below[4])
        {
            if (mode == 1u)
                return false;
            if (mode != 2u && mode != 3u)
                return true;
            int32_t a[4], c[4];
            effectWords255(above, a);
            effectWords255(below, c);
            uint64_t prim = 0x0003400000000000ull;
            EffectDraw top, bottom;
            if (mode == 2u)
            {
                effectWord(b, 0x1000000000000001ull, 0xEu);
                effectWord(b, 0x48u, 0x42u);
                prim = 0x0023400000000000ull;
                for (int i = 0; i < 4; ++i)
                    a[i] = c[i] = 0;
                top.abe = bottom.abe = true;
                top.alpha = bottom.alpha = 0x48u;
            }
            top.test = bottom.test = 0x30000u;
            effectWord(b, 0x1000000000000002ull, 0xEu);
            effectWord(b, 0x30000u, 0x47u);
            effectWord(b, 0x0000720000006C00ull, 0x18u);
            if (x0 == 0x6C00 && y0 == 0x7200 && x1 == 0x9400 && y1 == 0x8E00)
            {
                effectColumns(b, top, prim, a, 0x6C00, 0x400, 10u, 0x6FF8, 0x7200, split, true);
                effectColumns(b, bottom, prim, c, 0x6C00, 0x400, 10u, 0x6FF8, split, 0x8DF8, true);
            }
            else
            {
                // 0x12B290: the columns from x0 & ~0x3F, (x1 + 0x3F & ~0x3F) - that >> 6 of them, 0x400 apart (64 pixels: the
                // count is in 4-pixel steps, so they run past x1 on the right, which the scissor cuts). The game writes the
                // second band's PACKED tag one quadword short, over the first band's last column (the packet is malformed
                // there); the draft gives the words as the game writes them and draws both bands as meant.
                const int32_t first = x0 & ~0x3F;
                const uint32_t n = static_cast<uint32_t>((((x1 + 0x3F) & ~0x3F) - first) >> 6);
                effectColumns(b, top, prim, a, first, 0x400, n, first + 0x400, y0, split, true);
                if (n > 0u)
                {
                    Qword &last = b.words[b.words.size() - 1u];
                    last = {static_cast<uint32_t>(prim | 0x1000000000000001ull), static_cast<uint32_t>((prim | 0x1000000000000001ull) >> 32), 1u, 0u};
                }
                else
                    effectWord(b, prim | 0x1000000000000001ull, 1u);
                b.words.push_back({static_cast<uint32_t>(c[0]), static_cast<uint32_t>(c[1]), static_cast<uint32_t>(c[2]), static_cast<uint32_t>(c[3])});
                effectWord(b, 0x2400000000000000ull | n, 0x4Cu);
                const uint32_t rgba = effectRgba(c, true);
                for (uint32_t i = 0; i < n; ++i)
                {
                    const int32_t xa = first + 0x400 * static_cast<int32_t>(i), xb = xa + 0x400;
                    effectWord(b, (static_cast<uint64_t>(xa) & 0xFFFFu) | ((static_cast<uint64_t>(split) & 0xFFFFu) << 16),
                               (static_cast<uint64_t>(xb) & 0xFFFFu) | ((static_cast<uint64_t>(y1) & 0xFFFFu) << 16));
                    bottom.vertices.push_back(effectVertex(xa, split, rgba));
                    bottom.vertices.push_back(effectVertex(xb, y1, rgba));
                }
            }
            effectWord(b, 0x1000000000008001ull, 0xEu);
            effectWord(b, effectTestAfter(), 0x47u);
            b.draws.push_back(std::move(top));
            b.draws.push_back(std::move(bottom));
            return true;
        }

        // 0x12B7D0 (packet, camera 0x4696C0, file, colour above, colour below, flag A, flag B, f12 half width, f13 half height,
        // f14 the light's height) without a camera roll: where the horizon falls on screen, the point 10000 units from the
        // light towards the camera's position (y 0, normalised) put at the light's height, through the 3D view (*(0x46DA88)
        // +0x448); above it colour A, below colour B, all of one when it is off screen.
        // Game calls (logic, not packets): 0x135F78 (camera) -> f0 (the roll), 0x135C68 (camera, out) -> the light's position.
        // a game function (logic, not a packet builder) called from a hook and run to its return (runToReturn), on a frame
        // below the caller's; the caller's GPRs, FPRs and pc are kept (the hook may still hand the call to the original).
        float callGame(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a0, uint32_t a1, uint32_t frame)
        {
            decltype(ctx->r) gprs;
            decltype(ctx->f) fprs;
            std::memcpy(&gprs, &ctx->r, sizeof(gprs));
            std::memcpy(&fprs, &ctx->f, sizeof(fprs));
            const uint32_t pc = ctx->pc;
            SET_GPR_U32(ctx, 29, frame);
            SET_GPR_U32(ctx, 4, a0);
            SET_GPR_U32(ctx, 5, a1);
            SET_GPR_U32(ctx, 31, kEffectLayer + 8u);
            ctx->pc = address;
            runToReturn(runtime->lookupFunction(address), rdram, ctx, runtime);
            const float f0 = ctx->f[0];
            std::memcpy(&ctx->r, &gprs, sizeof(gprs));
            std::memcpy(&ctx->f, &fprs, sizeof(fprs));
            ctx->pc = pc;
            return f0;
        }

        bool effectHorizon(EffectBuild &b, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t file, const float above[4],
                           const float below[4], uint32_t flagA, uint32_t flagB, float halfWidth, float halfHeight, float lightHeight)
        {
            const uint32_t frame = (getRegU32(ctx, 29) - 0x200u) & ~0xFu;
            if (callGame(kRoll, rdram, ctx, runtime, kEffectCamera, 0u, frame) != 0.0f)
                return false;
            const uint32_t lightAt = frame + 0x100u;
            callGame(kLight, rdram, ctx, runtime, kEffectCamera, lightAt, frame);
            float light[4], dir[4];
            for (int i = 0; i < 4; ++i)
            {
                light[i] = peekFloat(lightAt + static_cast<uint32_t>(i) * 4u);
                dir[i] = peekFloat(kEffectCamera + 0x20u + static_cast<uint32_t>(i) * 4u);
            }
            for (int i = 0; i < 3; ++i)
                dir[i] = dir[i] - light[i];
            dir[1] = 0.0f;
            const float inv = 1.0f / std::sqrt(ps2_fadd(ps2_fadd(ps2_fmul(dir[0], dir[0]), ps2_fmul(dir[1], dir[1])), ps2_fmul(dir[2], dir[2])));
            for (int i = 0; i < 3; ++i)
                dir[i] = ps2_fmul(dir[i], inv);
            light[1] = lightHeight;
            float p[4];
            for (int i = 0; i < 3; ++i)
                p[i] = light[i] + dir[i] * 10000.0f;
            p[3] = 1.0f;
            const uint32_t view = peek32(kView);
            const uint32_t m = peek32(view + 0x448u);
            float q[4];
            for (int i = 0; i < 4; ++i)
            {
                float acc = peekFloat(m + static_cast<uint32_t>(i) * 4u) * p[0];
                acc = acc + peekFloat(m + 0x10u + static_cast<uint32_t>(i) * 4u) * p[1];
                acc = acc + peekFloat(m + 0x20u + static_cast<uint32_t>(i) * 4u) * p[2];
                q[i] = acc + peekFloat(m + 0x30u + static_cast<uint32_t>(i) * 4u) * p[3];
            }
            const int32_t yBottom = ps2FpuCvtWS(ps2_fadd(halfHeight, 4096.0f)) << 3;
            const int32_t yTop = ps2FpuCvtWS(ps2_fsub(4096.0f, halfHeight)) << 3;
            int32_t split;
            if (q[3] < 0.08f)
                split = q[1] < 0.0f ? yTop : yBottom + 1;
            else
                split = ps2FpuCvtWS(ps2_fmul(ps2_fsub(ps2_fadd(ps2_fdiv(q[1], q[3]), peekFloat(view + 0x3D4u)), 224.0f), 16.0f));
            if (yBottom < split)
            {
                effectFill(b, file, above, flagA, flagB);
                return true;
            }
            if (split <= yTop)
            {
                effectFill(b, file, below, flagA, flagB);
                return true;
            }
            const int32_t x1 = ps2FpuCvtWS(ps2_fmul(ps2_fadd(halfWidth, 4096.0f), 8.0f));
            const int32_t x0 = ps2FpuCvtWS(ps2_fmul(ps2_fsub(4096.0f, halfWidth), 8.0f));
            return effectBands(b, (flagA & 1u) | ((flagB & 1u) << 1), x0, yTop, split, x1, yBottom, above, below);
        }

        // The colours both entries hand 0x12B7D0 / 0x13B8C0: the view's +0x90 (above) and +0x80 (below) times +0x7C, w 0.
        void effectColours(uint32_t view, float above[4], float below[4])
        {
            const float k = peekFloat(view + 0x7Cu);
            for (int i = 0; i < 3; ++i)
            {
                above[i] = peekFloat(view + 0x90u + static_cast<uint32_t>(i) * 4u) * k;
                below[i] = peekFloat(view + 0x80u + static_cast<uint32_t>(i) * 4u) * k;
            }
            above[3] = below[3] = 0.0f;
        }

        void effectDraw(const EffectBuild &b, uint64_t key)
        {
            for (const EffectDraw &d : b.draws)
            {
                if (d.vertices.empty())
                    continue;
                GsMaterial m;
                m.prim.type = d.type;
                m.prim.abe = d.abe;
                m.alpha = d.alpha;
                m.test = d.test;
                ps2x::native::Renderer::instance().drawGsKeyed(key, m, d.vertices.data(), static_cast<uint32_t>(d.vertices.size()));
            }
        }

        // The packet's words as the game left them at `block` (its CNT header, then the GIF data; word 1 of the header is
        // never written by the game), for check mode.
        std::vector<Qword> effectPacket(uint32_t block)
        {
            std::vector<Qword> out;
            const uint32_t qwc = peek32(block) & 0xFFFFu;
            for (uint32_t i = 1; i <= qwc; ++i)
                out.push_back({peek32(block + i * 16u), peek32(block + i * 16u + 4u), peek32(block + i * 16u + 8u), peek32(block + i * 16u + 12u)});
            return out;
        }

        // 0x12C7E0 (view): when 0x12C690 left a slot (*(0x469210)), the layer built and CALLed from it; native: a marker in the
        // slot (CNT QWC 0, the key in its upper 64 bits), *(0x469210) = 0. Frame not built (the game's copy fails): CNT 0.
        void screenEffectLayer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.effects;
            ++stats.calls;
            const uint32_t slot = peek32(kEffectSlot);
            if (s.mode == Mode::Off || slot == 0u)
            {
                callOriginal(kEffectLayer, rdram, ctx, runtime);
                return;
            }
            const uint32_t view = getRegU32(ctx, 4);
            float above[4], below[4];
            effectColours(view, above, below);
            const uint64_t flags = peek64(view + 0xE8u);
            EffectBuild b;
            const bool drafted = effectHorizon(b, rdram, ctx, runtime, peek32(view + 4u), above, below, static_cast<uint32_t>(flags >> 24) & 1u,
                                               static_cast<uint32_t>(flags >> 32) & 1u, peekFloat(view + 0x18u), peekFloat(view + 0x1Cu),
                                               peekFloat(view + 0x78u));
            if (!drafted)
            {
                ++stats.toGs;
                callOriginal(kEffectLayer, rdram, ctx, runtime);
                return;
            }
            if (s.mode == Mode::Check)
            {
                runOriginal(kEffectLayer, rdram, ctx, runtime);
                if ((peek32(slot) >> 28) == 5u)
                    compareQwords(stats, b.words, effectPacket(peek32(slot + 4u)), "screen effect");
                return;
            }
            if (peek32(kRenderer) != 0u)
            {
                poke64(slot, 0x10000000ull);
                poke64(slot + 8u, 0u);
            }
            else
            {
                const uint64_t key = newKey();
                effectDraw(b, key);
                poke64(slot, 0x10000000ull);
                poke64(slot + 8u, key);
                stats.sprites += b.draws.size();
                ++stats.markers;
            }
            poke32(kEffectSlot, 0u);
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x12C690 (view, cursor) -> cursor: the layer in the GIF list being built: nothing when 0x46E5DB is set; a one-quadword
        // slot for 0x12C7E0 when 0x46E5D1 is 0 and the globals' +0x70 is set (*(0x469210) = its list address, 0x10DB68);
        // else built inline (+0x70 0: 0x13B8C0 with colour A over the screen; else 0x12B7D0). Native inline: a marker quadword
        // at the cursor (CNT QWC 0 with the key) for the packet, the cursor after it.
        void screenEffectInline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.effects;
            ++stats.calls;
            const uint32_t view = getRegU32(ctx, 4), cursor = getRegU32(ctx, 5);
            if (s.mode == Mode::Off)
            {
                callOriginal(kEffectInline, rdram, ctx, runtime);
                return;
            }
            poke32(kEffectSlot, 0u);
            if (peek8(kNoPacketsByte) != 0u)
            {
                setReturnU32(ctx, cursor);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            const uint32_t wanted = peek32(peek32(kEffectGlobals) + 0x70u);
            if (peek8(kInlineByte) == 0u && wanted != 0u)
            {
                poke32(kEffectSlot, peek32(kGifListCursor) + (cursor - peek32(kScratchpad)));
                setReturnU32(ctx, cursor + 16u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            float above[4], below[4];
            effectColours(view, above, below);
            const uint64_t flags = peek64(view + 0xE8u);
            const uint32_t flagA = static_cast<uint32_t>(flags >> 24) & 1u, flagB = static_cast<uint32_t>(flags >> 32) & 1u;
            EffectBuild b;
            bool drafted = true;
            if (wanted == 0u)
                effectFill(b, peek32(view + 4u), above, flagA, flagB);
            else
                drafted = effectHorizon(b, rdram, ctx, runtime, peek32(view + 4u), above, below, flagA, flagB, peekFloat(view + 0x18u),
                                        peekFloat(view + 0x1Cu), peekFloat(view + 0x78u));
            if (!drafted)
            {
                ++stats.toGs;
                callOriginal(kEffectInline, rdram, ctx, runtime);
                return;
            }
            if (s.mode == Mode::Check)
            {
                runOriginal(kEffectInline, rdram, ctx, runtime);
                compareQwords(stats, b.words, effectPacket(cursor), "screen effect inline");
                return;
            }
            const uint64_t key = newKey();
            effectDraw(b, key);
            poke64(cursor, 0x10000000ull);
            poke64(cursor + 8u, key);
            stats.sprites += b.draws.size();
            ++stats.markers;
            ++stats.native;
            setReturnU32(ctx, cursor + 16u);
            ctx->pc = getRegU32(ctx, 31);
        }

        void insertion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t at = ctx->pc;
            const uint32_t base = peek32(kScratchpad);
            const uint32_t end = getRegU32(ctx, 4);
            if (s.capture && (at == kInsertText || at == kInsertLayer || at == kInsertList))
                s.captured.push_back(capturedPacket(end, getRegU32(ctx, 5), at, getRegU32(ctx, 7) & 0xFFu));
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
            reportBuilder("text 0x11EA80", s.text);
            reportBuilder("quads 0x123340", s.quads);
            reportBuilder("lines 0x1223D0", s.lines);
            reportBuilder("icons 0x1280F8", s.icons);
            reportBuilder("screen effects 0x12C690 / 0x12C7E0", s.effects);
            std::fprintf(stderr, "[sc3:2d]   2D file uploads dropped: %llu\n", static_cast<unsigned long long>(s.uploadsDropped));
            s.uploadsDropped = 0;
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
                if (leaf.name.rfind("elf/", 0) == 0)
                    g_state.elfFiles.push_back(leaf);
                else
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
        for (const Leaf &file : s.elfFiles)
        {
            const packs::LoadedImages::Bound bound = s.loaded.bind(s.pack, s.rdram, file.name, static_cast<uint32_t>(file.offset), file.size, false);
            if (!bound.image.file)
                std::fprintf(stderr, "[sc3:2d] %s: not the ELF's image (crc %#x), its draws go to the GS\n", file.name.c_str(), bound.crc);
        }
        wrap(runtime, kFileRequest, fileRequest);
        wrap(runtime, kCdRead, cdRead);
        wrap(runtime, kLabels, labels);
        wrap(runtime, kTriangle, triangle);
        wrap(runtime, kText, text);
        wrap(runtime, kQuad, quad);
        wrap(runtime, kLine, line);
        wrap(runtime, kIcon, icon);
        wrap(runtime, kEffectLayer, screenEffectLayer);
        wrap(runtime, kEffectInline, screenEffectInline);
        for (const uint32_t address : {kFontUpload, kLabelsUpload, kIconsUpload})
            wrap(runtime, address, upload);
        s.format = runtime.lookupFunction(kTextFormat);
        wrap(runtime, kFrameKick, frameKick);
        for (const uint32_t address : kInserts)
            wrap(runtime, address, insertion);
        ps2x::native::setGifChainMarkGate(markerGate);
        s.installed = true;
        std::fprintf(stderr, "[sc3:2d] native 2D%s: %s, %zu leaves with texture pages\n", s.switchPath.empty() ? "" : " (as its switch file says)",
                     path, s.pack.files().size());
    }
}
