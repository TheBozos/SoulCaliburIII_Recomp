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
#include <initializer_list>
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
        // The movie task's copy of a picture into a texture file's pixels (handle, frame, pixels, flags) and the pointer
        // of a file's upload to those pixels (pixels, file, call, page).
        constexpr uint32_t kMovieCopy = 0x00217E28u, kFilePixels = 0x00118B68u;
        constexpr uint64_t kMovieKey = 1ull << 63;
        constexpr uint32_t kMovieSlots = 3u; // pictures a draw may still be waiting to use from the GS thread
        constexpr uint32_t kLabels = 0x001388E8u;
        constexpr uint32_t kTriangle = 0x00124C80u;
        constexpr uint32_t kInsertMode = 0x00468CB0u; // 0x120978's mode
        // Where 2D packets join the frame: the 2D insertion dispatcher (by the mode at 0x468CB0), fixed OT buckets, the
        // text layers' buckets, and the in-order GIF list.
        constexpr uint32_t kInserts[] = {0x00120978u, 0x0010DDF8u, 0x0010DF30u, 0x0010E068u, 0x0010E1C8u, 0x0010E368u,
                                         0x0010D9A0u, 0x0010DC90u, 0x0010E1F8u, 0x0010D2F0u, 0x001132A8u};

        constexpr uint32_t kRenderer = 0x005BE700u;   // +0 frame not built, +0x2c heap qwords used, +0x30 heap cursor, +0x54 the OT
        constexpr uint32_t kOtLayerBase = 0x00467490u; // bucket offset of the current layer
        constexpr uint32_t kOtGroups = 0x00467488u;    // 64 bits: the 128-bucket groups with packets
        constexpr uint32_t k2d = 0x005BEDD0u;          // +0x4c the 2D texture file, +0x68 the 2D depth
        constexpr uint32_t kAlphaTable = 0x0046EFD8u;
        constexpr uint32_t kView = 0x0046DA88u;        // +0x460 the 2D view matrix
        constexpr uint32_t kView2dMatrix = 0x460u, kView3dMatrix = 0x448u; // the view's matrix slots
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
            uint64_t alpha = 0, test = 0; // the block's own ALPHA / TEST writes, when it has them
            bool hasAlpha = false, hasTest = false;
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
            BuilderStats labels, triangles, text, quads, lines, shadedLines, icons, effects, strips, objects, screenObjects, screenSprites, billboards, screenStrips;
            std::map<uint32_t, uint64_t> objectForms; // 0x110E50 by +0x1A << 24 | +0x1C << 16 | +0x1D << 8 | +0x1F
            std::map<uint32_t, uint64_t> screenObjectForms, screenSpriteForms, billboardForms; // 0x1107E0 / 0x110258 / 0x112148, as objectForms
            std::map<uint32_t, uint64_t> stripForms, screenStripForms; // 0x1134A0 / 0x114B50 by +0x1A << 24 | +0x1C << 16 | +0x1D << 8 | +0x1F << 4 | insertion
            struct PageCopy
            {
                uint32_t file = 0, page = 0, clut = 0;
                bool absolute = false; // clut is the CBP itself
                bool resolved = false; // resolvedPage is the copied page, found while its file was there
                Page resolvedPage;
            };
            std::map<uint32_t, PageCopy> pageCopies; // 0x119318's copies of page blocks, by where they went
            uint64_t pageCopyChecks = 0, pageCopyMisses = 0; // check mode: head-2 strips whose copy is the recorded page's / not
            bool stripCapture = false;                // check mode: the packets the strips' insertions get, from the scratch base
            std::vector<std::vector<std::array<uint32_t, 4>>> stripPackets;
            uint64_t uploadsDropped = 0;
            std::map<std::pair<uint32_t, bool>, uint64_t> uploadsKept; // native on: (file, an item of it went to the GS) -> uploads sent
            Function format = nullptr; // 0x11CBC8, the text renderer's formatter
            bool capture = false;      // check mode: the game's text packets kept as its insertions get them
            std::vector<TextPacket> captured;
            std::map<uint64_t, uint64_t> textureMisses; // TEX0 -> draws left to the GS without a pack texture
            // Packets the game still builds for 2D, by the return address of their insertion (ra: the builder) and the
            // insertion function: calls and quadwords.
            std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>> inserted;
            uint32_t insertDepth = 0; // inside a counted insertion: 0x10F808 from there is part of it
            uint32_t outerInsertRa = 0; // inside a draw item's insertion (0x110D78 ...): its caller, who the packet is counted for
            uint64_t frames = 0, bound = 0, boundBad = 0;
            // Texture files of draw items: the last frame an item of theirs linked a game packet, the first frame their upload
            // item (0x115600) was seen, and the file of the item type still the game's that runs now.
            std::map<uint32_t, uint64_t> itemFilesGs, uploadFilesSeen;
            uint32_t screenItemsGsLogged = 0;
            uint32_t gsItemFile = 0;
            // Small movies the native 2D draws from the host decoder's pictures: by player handle, the pixels the movie task
            // copies them to (0x217E28's destination), and the page blocks of the texture files pointing there.
            struct Movie
            {
                uint32_t number = ~0u, slot = 0;
                uint64_t copied = 0; // the frame of the movie task's last copy
                std::array<std::vector<uint32_t>, kMovieSlots> rgba;
                std::array<ps2x::native::NativeTexture, kMovieSlots> native;
            };
            sc3::MoviePicture moviePictures = nullptr;
            std::map<uint32_t, Movie> movies;
            std::map<uint32_t, uint32_t> movieDests, moviePages;
            uint64_t movieCopiesSkipped = 0, movieTextures = 0, movieChecks = 0, movieTexelsDiffering = 0;
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

        uint32_t loadedImageBase(uint32_t address)
        {
            packs::LoadedImages::Image image;
            return g_state.loaded.find(address, image) && image.file ? image.base : address;
        }

        // The page `index` of a texture file (0x1177E0's choice: the last one past its count) from the 2D pack, its texture
        // the one the file's own uploads write (decoded on first use, kept for the program's life).
        bool blockPage(uint32_t block, Page &out, BuilderStats &stats, uint32_t cbp);
        bool moviePage(uint32_t block, Page &out);

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
                case 0x42:
                case 0x43:
                    out.alpha = value;
                    out.hasAlpha = true;
                    break;
                case 0x47:
                case 0x48:
                    out.test = value;
                    out.hasTest = true;
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
            if (moviePage(block, out))
                return true;
            ++stats.noTexture;
            ++s.textureMisses[out.tex0 & ((0x3FFFull) | (0x3Full << 20) | (0x3FFFull << 37))];
            return false;
        }

        // ---- small movies: the host decoder's picture as the texture of the pages the movie task copies it into

        ps2x::native::NativeTexture *moviePicture(uint32_t handle)
        {
            State &s = g_state;
            if (!s.moviePictures)
                return nullptr;
            State::Movie &m = s.movies[handle];
            const uint32_t next = (m.slot + 1u) % kMovieSlots;
            uint32_t width = 0, height = 0, number = 0;
            if (!s.moviePictures(handle, m.number, m.rgba[next], width, height, number) || width == 0u || height == 0u)
                return nullptr;
            if (number != m.number)
            {
                m.slot = next;
                m.number = number;
                ps2x::native::NativeTexture &t = m.native[next];
                t.width = width;
                t.height = height;
                t.rgba = m.rgba[next].data();
                t.key = kMovieKey | (static_cast<uint64_t>(handle) << 2) | next;
                t.version = number;
            }
            return &m.native[m.slot];
        }

        bool moviePage(uint32_t block, Page &out)
        {
            State &s = g_state;
            const auto it = s.moviePages.find(block);
            if (it == s.moviePages.end())
                return false;
            const ps2x::native::NativeTexture *picture = moviePicture(it->second);
            if (!picture)
                return false;
            out.texture = picture;
            ++s.movieTextures;
            return true;
        }

        void movieCopy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t handle = getRegU32(ctx, 4), frame = getRegU32(ctx, 5), dest = getRegU32(ctx, 6) & PS2_RAM_MASK;
            if (s.mode == Mode::Off || !s.moviePictures)
            {
                callOriginal(kMovieCopy, rdram, ctx, runtime);
                return;
            }
            s.movieDests[dest] = handle;
            s.movies[handle].copied = s.frames;
            if (s.mode == Mode::On)
            {
                ++s.movieCopiesSkipped;
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            const uint32_t width = peek32(frame + 0xCu), height = peek32(frame + 0x10u);
            const uint32_t rows = (getRegU32(ctx, 7) & 8u) != 0u ? height / 32u * 16u : height;
            runOriginal(kMovieCopy, rdram, ctx, runtime);
            const ps2x::native::NativeTexture *picture = moviePicture(handle);
            if (!picture || picture->width != width || picture->height != height)
                return;
            ++s.movieChecks;
            for (uint32_t i = 0; i < width * rows; ++i)
                if (peek32(dest + i * 4u) != picture->rgba[i])
                    ++s.movieTexelsDiffering;
        }

        void filePixels(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t pixels = getRegU32(ctx, 4) & PS2_RAM_MASK, file = getRegU32(ctx, 5);
            callOriginal(kFilePixels, rdram, ctx, runtime);
            const auto it = s.movieDests.find(pixels);
            if (it == s.movieDests.end() || !fileAddress(file))
                return;
            const uint32_t pages = peek8(file + 5u) + peek8(file + 7u) * 256u;
            for (uint32_t i = 0; i < pages; ++i)
                s.moviePages[peek32(peek32(file + 0xCu) + i * 4u) & ~0xFu] = it->second;
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

        // Z of a PACKED XYZF2 / XYZF3 write: bits 4..27 of its third word (XYZ2 takes the whole word).
        uint32_t xyzfZ(uint32_t word) { return (word >> 4) & 0xFFFFFFu; }

        GsPoint xyzfPoint(GsPoint p)
        {
            p.z = static_cast<int32_t>(xyzfZ(static_cast<uint32_t>(p.z)));
            return p;
        }

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
        // tag after its NEXT, its NLOOP sprites of five quadwords (RGBAQ, ST, XYZF2, ST, XYZF2).
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
                    spriteCorners(triangles, xyzfPoint(GsPoint{sp.x0, sp.y0, sp.z0}), xyzfPoint(GsPoint{sp.x1, sp.y1, sp.z1}), sp.s0, sp.t0, sp.s1, sp.t1, sp.rgba);
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
            v.z = xyzfZ(xyz[2]);
            v.rgba = (rgba[0] & 0xFFu) | ((rgba[1] & 0xFFu) << 8) | ((rgba[2] & 0xFFu) << 16) | ((rgba[3] & 0xFFu) << 24);
            return v;
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

        // ---- 0x124C80 (xyz0, xyz1, xyz2 pointers to packed XYZ2 quadwords, a3 / t0 / t1 their RGBAQ quadwords, t2 ALPHA_2,
        // f12 the depth): one Gouraud-shaded blended triangle, untextured, inserted through 0x120978

        void triangle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.triangles;
            ++stats.calls;
            if (s.mode == Mode::Off)
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
            if (s.mode == Mode::Check)
            {
                const uint64_t alpha = reg64(ctx, 10);
                std::vector<Qword> expected = {
                    {0x00008001u, 0x71264000u, 0x0414141Eu, 0u},
                    {static_cast<uint32_t>(alpha), static_cast<uint32_t>(alpha >> 32), 0x43u, 0u},
                };
                for (int i = 0; i < 3; ++i)
                {
                    expected.push_back({rgba[i][0], rgba[i][1], rgba[i][2], rgba[i][3]});
                    expected.push_back({xyz[i][0], xyz[i][1], xyz[i][2], xyz[i][3]});
                }
                const InsertWatch watch = watchInsert(ctx->f[12]);
                stats.gifQwords += 9u;
                runOriginal(kTriangle, rdram, ctx, runtime);
                if (peek32(kRenderer) == 0u)
                    compareQwords(stats, expected, insertedQwords(watch), "triangles");
                return;
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
            spriteCorners(corners, xyzfPoint(p0), xyzfPoint(p1), 0.0f, 0.0f, 0.0f, 0.0f, colour[0] | (colour[1] << 8) | (colour[2] << 16) | (colour[3] << 24));
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
                    ends[i].z = xyzfZ(xyz[i][2]);
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

        // ---- 0x1220C0 (a0 / a1 -> the ends as packed XYZ2 quadwords, a2 / a3 their RGBAQ quadwords, t0 ALPHA_2, f12 the
        // depth): one antialiased Gouraud-shaded blended line (PRIM 0x2C9), inserted through 0x120978

        constexpr uint32_t kShadedLine = 0x001220C0u;

        void shadedLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.shadedLines;
            ++stats.calls;
            if (s.mode == Mode::Off)
            {
                stats.gifQwords += 7u;
                callOriginal(kShadedLine, rdram, ctx, runtime);
                return;
            }
            uint32_t xyz[2][4], rgba[2][4];
            for (int i = 0; i < 2; ++i)
            {
                for (int w = 0; w < 4; ++w)
                    xyz[i][w] = peek32(getRegU32(ctx, 4 + i) + static_cast<uint32_t>(w) * 4u);
                word4(ctx, 6 + i, rgba[i]);
            }
            const uint64_t alpha = reg64(ctx, 8);
            if (s.mode == Mode::Check)
            {
                const std::vector<Qword> expected = {
                    {0x00008001u, 0x5164C000u, 0x0004141Eu, 0u},
                    {static_cast<uint32_t>(alpha), static_cast<uint32_t>(alpha >> 32), 0x43u, 0u},
                    {rgba[0][0], rgba[0][1], rgba[0][2], rgba[0][3]},
                    {xyz[0][0], xyz[0][1], xyz[0][2], xyz[0][3]},
                    {rgba[1][0], rgba[1][1], rgba[1][2], rgba[1][3]},
                    {xyz[1][0], xyz[1][1], xyz[1][2], xyz[1][3]},
                };
                const InsertWatch watch = watchInsert(ctx->f[12]);
                stats.gifQwords += 7u;
                runOriginal(kShadedLine, rdram, ctx, runtime);
                if (peek32(kRenderer) == 0u)
                    compareQwords(stats, expected, insertedQwords(watch), "shaded lines");
                return;
            }
            const uint64_t key = newKey();
            const bool drawn = (xyz[1][3] & 0x8000u) == 0u;
            if (drawn)
            {
                GsVertex ends[2];
                for (int i = 0; i < 2; ++i)
                    ends[i] = packedVertex(rgba[i], xyz[i]);
                ps2x::native::Renderer::instance().drawGsKeyed(key, flatMaterial(GS_PRIM_LINE, 0x2C9u, alpha), ends, 2u);
            }
            const Linked linked = insert2d(key, ctx->f[12]);
            if (linked != Linked::Yes && drawn)
                discardKeyed(key);
            if (linked == Linked::NotHere)
            {
                ++stats.toGs;
                callOriginal(kShadedLine, rdram, ctx, runtime);
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
            spriteCorners(corners, xyzfPoint(p0), xyzfPoint(p1), fs0, ft0, fs1, ft1, colour[0] | (colour[1] << 8) | (colour[2] << 16) | (colour[3] << 24));
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

        // 0x115600 (a0 -> item, type 13 of the draw items): CALLs of the upload lists of the texture file +0x14 (0x117920 /
        // 0x118430) or of the block +0x10 (0x117B80 / 0x118700), then TEXFLUSH, for later draws. Dropped while the native 2D
        // is on, every page of the file +0x14 is in the pack with its texture and no item of it linked a game packet for a
        // while (the item types still the game's draw from these uploads on the GS); never in the first two frames the
        // upload is seen, so such an item is found before.

        constexpr uint32_t kUploadItem = 0x00115600u;
        constexpr uint64_t kGsItemFrames = 120u;

        void uploadItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t item = getRegU32(ctx, 4), file = peek32(item + 0x14u);
            auto gs = s.itemFilesGs.find(file);
            if (gs == s.itemFilesGs.end())
                gs = s.itemFilesGs.find(loadedImageBase(file));
            const uint64_t seen = s.uploadFilesSeen.try_emplace(file, s.frames).first->second;
            bool packed = s.mode == Mode::On && fileAddress(file) && seen + 2u <= s.frames &&
                          (gs == s.itemFilesGs.end() || gs->second + kGsItemFrames < s.frames);
            const uint32_t pages = packed ? peek8(file + 5u) + peek8(file + 7u) * 256u : 0u;
            packed = packed && pages != 0u;
            for (uint32_t i = 0; packed && i < pages; ++i)
            {
                Page p;
                BuilderStats unused;
                packed = page(file, i, p, unused);
            }
            if (!packed)
            {
                if (s.mode == Mode::On)
                    ++s.uploadsKept[{file, gs != s.itemFilesGs.end() && gs->second + kGsItemFrames >= s.frames}];
                callOriginal(kUploadItem, rdram, ctx, runtime);
                return;
            }
            ++s.uploadsDropped;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- 0x1134A0 (a0 -> item): textured triangle strips, VU0 macro code. The item: +0 ALPHA_2 (0: 0x2A), +8 the vertex
        // count, +0xA & 3 the insertion (the table at 0x467510: 0x1132A8 the GIF list, 0x10D9A0, 0x110D78, 0x113250, as
        // 0x120978's modes), +0xB more PRIM bits, +0xC the vertices (0x40 bytes: ST, a restart word at +0xC, position,
        // colour), +0x10 a matrix +0x1D multiplies the view's with, +0x14 / +0x18 the texture file and its page, +0x1A its
        // uploads, +0x1C the head (0: a REF of the page; PRIM 0x54 in context 1 when 1, else 0x254), +0x1F Z from the
        // view's projection, +0x20 an ST offset (z: Z scale), +0x28 a fixed depth when negative, +0x2C out: the mean w.
        // A packet takes 0x9C vertices and the two before them; a vertex behind w 0.08 or off the GS's range and the two
        // after it carry ADC.

        constexpr uint32_t kStrips = 0x001134A0u;
        constexpr uint32_t kStripTable = 0x00467510u, kStripInsertion = 0x00467520u;
        constexpr uint32_t kStripInserts[] = {0x001132A8u, 0x0010D9A0u, 0x00110D78u, 0x00113250u};
        constexpr uint32_t kStripClip = 0x00467690u, kStripColourMax = 0x00467600u, kStripDepthMin = 0x0058A560u;
        constexpr uint32_t kStripPacketVertices = 0x9Cu;
        constexpr uint32_t kUnknownWord = 0xFFFFFFFFu;

        using Vec = __m128;

        Vec lane(Vec v, int i)
        {
            switch (i)
            {
            case 0:
                return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
            case 1:
                return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
            case 2:
                return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
            default:
                return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
            }
        }

        // fd with the lanes of `mask` (1 x, 2 y, 4 z, 8 w) from r.
        Vec put(Vec fd, Vec r, unsigned mask)
        {
            const __m128i m = _mm_set_epi32((mask & 8u) ? -1 : 0, (mask & 4u) ? -1 : 0, (mask & 2u) ? -1 : 0, (mask & 1u) ? -1 : 0);
            return _mm_blendv_ps(fd, r, _mm_castsi128_ps(m));
        }

        Vec loadVec(uint32_t address)
        {
            address &= ~0xFu;
            return _mm_castsi128_ps(_mm_set_epi32(static_cast<int32_t>(peek32(address + 12u)), static_cast<int32_t>(peek32(address + 8u)),
                                                  static_cast<int32_t>(peek32(address + 4u)), static_cast<int32_t>(peek32(address))));
        }

        // vftoi0 / vftoi4 / vftoi12 / vftoi15 as the recompiler does them (truncated, saturated).
        Vec ftoiVec(Vec v, float scale)
        {
            const __m128 src = _mm_mul_ps(v, _mm_set1_ps(scale));
            const __m128i positiveOverflow =
                _mm_andnot_si128(_mm_srai_epi32(_mm_castps_si128(src), 31), _mm_castps_si128(_mm_cmpnlt_ps(src, _mm_set1_ps(2147483648.0f))));
            return _mm_castsi128_ps(_mm_xor_si128(_mm_cvttps_epi32(src), positiveOverflow));
        }

        Qword vecWords(Vec v)
        {
            Qword q;
            std::memcpy(q.data(), &v, 16);
            return q;
        }

        float vecLane(Vec v, int i)
        {
            float f[4];
            std::memcpy(f, &v, 16);
            return f[i];
        }

        // Words off the GS's coordinate range (vftoi4 X or Y above 0xFFFF): psrlw 16, ppach, and with the X / Y halves.
        bool offRange(Vec xyz)
        {
            const Qword w = vecWords(xyz);
            return ((w[0] >> 16) | (w[1] >> 16)) != 0u;
        }

        struct Vu0
        {
            Vec vf[32];
            Vec acc;
            float q = 0;
        };

        // 0x101058 (out a0, a a1, b a2): out row i = a times b's row i (vf1 .. vf8, ACC).
        void matrixMultiply(Vu0 &vu, uint32_t a, uint32_t b, Vec out[4])
        {
            for (int i = 0; i < 4; ++i)
                vu.vf[5 + i] = loadVec(a + static_cast<uint32_t>(i) * 16u);
            for (int i = 0; i < 4; ++i)
            {
                Vec &r = vu.vf[1 + i];
                r = loadVec(b + static_cast<uint32_t>(i) * 16u);
                vu.acc = ps2_vmul(vu.vf[5], lane(r, 0));
                vu.acc = ps2_vadd(vu.acc, ps2_vmul(vu.vf[6], lane(r, 1)));
                vu.acc = ps2_vadd(vu.acc, ps2_vmul(vu.vf[7], lane(r, 2)));
                r = ps2_vadd(vu.acc, ps2_vmul(vu.vf[8], lane(r, 3)));
                out[i] = r;
            }
        }

        struct StripPacket
        {
            std::vector<Qword> words; // from the packet's first CNT: A+D tag, ALPHA_2, a stale A+D, the DMA tag, GIF tag, vertices
            uint32_t qwc = 0;
            bool inserted = false;
            float depth = 0;
        };

        struct StripCall
        {
            uint32_t mode = 0, insertion = 0, file = 0, page = 0, prim = 0;
            uint64_t alpha = 0;
            std::vector<StripPacket> packets;
            std::vector<float> meanW; // +0x2C after each packet
        };

        // The packets 0x1134A0 builds for `item` (false: it returns before any), on a copy of VU0 that ends as the game's.
        bool stripBuild(uint32_t item, Vu0 &vu, StripCall &call)
        {
            if (peek8(item + 0x1Eu) == 0u)
                return false;
            const uint32_t count = peek16(item + 8u);
            call.mode = peek8(item + 0xAu) & 3u;
            call.insertion = peek32(kStripTable + call.mode * 4u);
            uint32_t p = peek32(item + 0xCu);
            call.file = peek32(item + 0x14u);
            if (p == 0u || (p & 0xFu) != 0u || call.file == 0u || (call.file & 0xFu) != 0u)
                return false;
            call.page = peek8(item + 0x18u);
            call.prim = (peek8(item + 0x1Cu) == 1u ? 0x54u : 0x254u) | peek8(item + 0xBu);
            const uint64_t tag = (static_cast<uint64_t>(call.prim) << 47) | (0x30004000ull << 32) | 0x8000u;
            call.alpha = peek64(item);
            if (call.alpha == 0u)
                call.alpha = 0x2Au;
            const uint32_t view = peek32(kView);
            Vec matrix[4];
            const uint32_t kind = peek8(item + 0x1Du);
            if (kind == 1u || kind == 2u)
                matrixMultiply(vu, peek32(view + (kind == 1u ? 0x448u : 0x454u)), peek32(item + 0x10u), matrix);
            else
                for (int i = 0; i < 4; ++i)
                    matrix[i] = loadVec(peek32(view + 0x448u) + static_cast<uint32_t>(i) * 16u);
            Vec *vf = vu.vf;
            const Vec one = lane(vf[0], 3);
            vf[23] = loadVec(kStripClip);
            vf[24] = loadVec(item + 0x20u);
            vf[25] = ps2_vsub(vf[0], one);
            vf[24] = put(vf[24], ps2_vadd(vf[0], lane(vf[23], 2)), 8u);
            vf[26] = loadVec(kStripColourMax);
            vf[27] = loadVec(view + 0x380u);
            for (int i = 0; i < 4; ++i)
                vf[28 + i] = matrix[i];
            vf[24] = put(vf[24], ftoiVec(vf[24], 1.0f), 8u);
            const float nearW = 0.08f;
            const bool projected = peek8(item + 0x1Fu) != 0u;
            bool fixedDepth = false;
            if (projected)
            {
                const uint32_t projection = peek32(view + 0x454u);
                vf[18] = loadVec(kStripDepthMin);
                vf[21] = loadVec(projection + 0x20u);
                vf[22] = loadVec(projection + 0x30u);
                vf[24] = put(vf[24], ps2_vsub(vf[0], lane(vf[24], 2)), 4u);
            }
            else
                fixedDepth = FPU_C_OLT_S(peekFloat(item + 0x28u), 0.0f);
            int32_t restart = 0;
            int32_t left = static_cast<int32_t>(count);
            const std::vector<Qword> *previous = nullptr;
            while (left > 0)
            {
                const int32_t total = left;
                left -= static_cast<int32_t>(kStripPacketVertices);
                const uint32_t n = left < 0 ? static_cast<uint32_t>(total) : kStripPacketVertices;
                StripPacket &packet = call.packets.emplace_back();
                if (call.packets.size() > 1u)
                    previous = &call.packets[call.packets.size() - 2u].words;
                std::vector<Qword> &w = packet.words;
                w.push_back({0x10000003u, kUnknownWord, kUnknownWord, kUnknownWord});
                w.push_back({2u, 0x10000000u, 0xEu, 0u});
                w.push_back({static_cast<uint32_t>(call.alpha), static_cast<uint32_t>(call.alpha >> 32), 0x43u, 0u});
                w.push_back({kUnknownWord, kUnknownWord, 3u, 0u});
                w.push_back({kUnknownWord, kUnknownWord, kUnknownWord, kUnknownWord});
                uint32_t vertices = n;
                const size_t tagAt = w.size();
                w.push_back({});
                if (previous)
                {
                    w.insert(w.end(), previous->end() - 6, previous->end());
                    vertices += 2u;
                }
                const uint64_t gifTag = vertices | tag;
                w[tagAt] = {static_cast<uint32_t>(gifTag), static_cast<uint32_t>(gifTag >> 32), 0x412u, 0u};
                vf[20] = ps2_vsub(vf[0], one);
                for (uint32_t i = 0; i < n; ++i, p += 0x40u)
                {
                    vf[3] = loadVec(p + 0x10u);
                    vu.acc = ps2_vmul(vf[31], one);
                    vf[2] = loadVec(p);
                    vu.acc = ps2_vadd(vu.acc, ps2_vmul(vf[28], lane(vf[3], 0)));
                    vu.acc = ps2_vadd(vu.acc, ps2_vmul(vf[29], lane(vf[3], 1)));
                    vf[3] = ps2_vadd(vu.acc, ps2_vmul(vf[30], lane(vf[3], 2)));
                    const uint32_t restartWord = peek32(p + 0xCu);
                    vf[4] = loadVec(p + 0x20u);
                    if (projected)
                        vf[19] = put(vf[19], ps2_vadd(vf[24], lane(vf[3], 3)), 4u);
                    vu.q = ps2_fdiv(1.0f, vecLane(vf[3], 3));
                    if (!projected)
                        vf[3] = put(vf[3], ps2_vmul(vf[3], lane(vf[24], 2)), 4u);
                    vf[4] = ps2_vmul(vf[4], lane(vf[27], 1));
                    vf[2] = put(vf[2], ps2_vadd(vf[2], vf[24]), 3u);
                    vf[2] = put(vf[2], ps2_vadd(vf[0], one), 4u);
                    if (projected)
                        vf[19] = put(vf[19], _mm_max_ps(vf[19], vf[18]), 4u);
                    const float w3 = vecLane(vf[3], 3);
                    if (restart <= 0 && restartWord != 0u)
                        restart = 1;
                    vf[4] = _mm_max_ps(vf[4], vf[25]);
                    const bool behind = FPU_C_OLT_S(w3, nearW);
                    vf[3] = put(vf[3], ps2_vmul(vf[3], _mm_set1_ps(vu.q)), 7u);
                    bool clipped = behind;
                    if (!behind)
                    {
                        vf[2] = put(vf[2], ps2_vmul(vf[2], _mm_set1_ps(vu.q)), 7u);
                        if (projected)
                            vu.q = ps2_fdiv(1.0f, vecLane(vf[19], 2));
                        vf[2] = put(vf[2], ps2_vsub(vf[2], lane(vf[2], 3)), 8u);
                        vf[4] = _mm_min_ps(vf[4], vf[26]);
                        vf[5] = put(vf[5], ftoiVec(vf[3], 16.0f), projected ? 3u : 7u);
                        vf[4] = ftoiVec(vf[4], 1.0f);
                        if (projected)
                        {
                            vf[19] = put(vf[19], ps2_vmul(vf[19], lane(vf[21], 2)), 4u);
                            vf[19] = put(vf[19], ps2_vadd(vf[19], lane(vf[22], 2)), 4u);
                        }
                        clipped = offRange(vf[5]);
                        vf[5] = put(vf[5], ps2_vsub(vf[0], one), 8u);
                        if (!clipped)
                        {
                            if (projected)
                                vf[19] = put(vf[19], ps2_vmul(vf[19], _mm_set1_ps(vu.q)), 4u);
                            vf[20] = put(vf[20], ps2_vadd(vf[20], one), 2u);
                            if (restart != 0)
                            {
                                vf[5] = put(vf[5], vf[24], 8u);
                                --restart;
                            }
                            if (projected)
                                vf[5] = put(vf[5], ftoiVec(vf[19], 16.0f), 4u);
                            vf[20] = put(vf[20], ps2_vadd(vf[20], lane(vf[3], 3)), 1u);
                        }
                    }
                    if (clipped)
                    {
                        vf[5] = put(vf[5], vf[24], 8u);
                        restart = 2;
                    }
                    w.push_back(vecWords(vf[2]));
                    w.push_back(vecWords(vf[4]));
                    w.push_back(vecWords(vf[5]));
                }
                packet.qwc = vertices * 3u + 1u;
                const float x = vecLane(vf[20], 0), y = vecLane(vf[20], 1);
                float depth = ps2_fdiv(y, x);
                vu.q = ps2_fdiv(x, y);
                if (FPU_C_OLT_S(0.0f, x))
                {
                    if (fixedDepth)
                    {
                        depth = -peekFloat(item + 0x28u);
                        if (peek8(item + 0xAu) == 1u)
                            depth = ps2_fmul(depth, 12.5f);
                    }
                    packet.inserted = true;
                    packet.depth = depth;
                    call.meanW.push_back(vu.q);
                }
                else
                    call.meanW.push_back(-1.0f);
            }
            return true;
        }

        // ---- 0x114B50 (a0 -> item): 0x1134A0's item and packets in screen space (the fight's health gauges): the view's 2D
        // matrix (+0x460, times +0x10 when +0x1D is 1 or 2), X / Y as it gives them, Z times the Z scale (+0x28, 1 when
        // negative) over the position's z, ST with Q 1; a vertex whose position z is under 0.08 or off the GS's range and
        // the two after it carry ADC. Depth: the drawn vertices over the sum of their z / the Z scale (-(+0x28) when
        // negative), +0x2C their mean.

        constexpr uint32_t kScreenStrips = 0x00114B50u;

        bool screenStripBuild(uint32_t item, Vu0 &vu, StripCall &call)
        {
            if (peek8(item + 0x1Eu) == 0u)
                return false;
            const uint32_t count = peek16(item + 8u);
            call.mode = peek8(item + 0xAu) & 3u;
            call.insertion = peek32(kStripTable + call.mode * 4u);
            uint32_t p = peek32(item + 0xCu);
            call.file = peek32(item + 0x14u);
            if (p == 0u || (p & 0xFu) != 0u || call.file == 0u || (call.file & 0xFu) != 0u)
                return false;
            call.page = peek8(item + 0x18u);
            call.prim = (peek8(item + 0x1Cu) == 1u ? 0x54u : 0x254u) | peek8(item + 0xBu);
            const uint64_t tag = (static_cast<uint64_t>(call.prim) << 47) | (0x30004000ull << 32) | 0x8000u;
            call.alpha = peek64(item);
            if (call.alpha == 0u)
                call.alpha = 0x2Au;
            const uint32_t view = peek32(kView);
            Vec matrix[4];
            const uint32_t kind = peek8(item + 0x1Du);
            if (kind == 1u || kind == 2u)
                matrixMultiply(vu, peek32(view + kView2dMatrix), peek32(item + 0x10u), matrix);
            else
                for (int i = 0; i < 4; ++i)
                    matrix[i] = loadVec(peek32(view + kView2dMatrix) + static_cast<uint32_t>(i) * 16u);
            const float fixed = peekFloat(item + 0x28u);
            const bool fixedDepth = FPU_C_OLT_S(fixed, 0.0f);
            Vec *vf = vu.vf;
            const Vec one = lane(vf[0], 3);
            vf[23] = loadVec(kStripClip);
            vf[24] = loadVec(item + 0x20u);
            if (fixedDepth)
                vf[24] = put(vf[24], _mm_set1_ps(1.0f), 4u);
            vf[25] = ps2_vsub(vf[0], one);
            vu.q = ps2_fdiv(1.0f, vecLane(vf[24], 2));
            vf[24] = put(vf[24], ps2_vadd(vf[0], lane(vf[23], 2)), 8u);
            vf[26] = loadVec(kStripColourMax);
            vf[27] = loadVec(view + 0x380u);
            for (int i = 0; i < 4; ++i)
                vf[28 + i] = matrix[i];
            vf[24] = put(vf[24], ftoiVec(vf[24], 1.0f), 8u);
            vf[7] = ps2_vadd(vf[0], _mm_set1_ps(vu.q));
            const float nearZ = 0.08f;
            int32_t restart = 0;
            int32_t left = static_cast<int32_t>(count);
            const std::vector<Qword> *previous = nullptr;
            while (left > 0)
            {
                const int32_t total = left;
                left -= static_cast<int32_t>(kStripPacketVertices);
                const uint32_t n = left < 0 ? static_cast<uint32_t>(total) : kStripPacketVertices;
                StripPacket &packet = call.packets.emplace_back();
                if (call.packets.size() > 1u)
                    previous = &call.packets[call.packets.size() - 2u].words;
                std::vector<Qword> &w = packet.words;
                w.push_back({0x10000003u, kUnknownWord, kUnknownWord, kUnknownWord});
                w.push_back({2u, 0x10000000u, 0xEu, 0u});
                w.push_back({static_cast<uint32_t>(call.alpha), static_cast<uint32_t>(call.alpha >> 32), 0x43u, 0u});
                w.push_back({kUnknownWord, kUnknownWord, 3u, 0u});
                w.push_back({kUnknownWord, kUnknownWord, kUnknownWord, kUnknownWord});
                uint32_t vertices = n;
                const size_t tagAt = w.size();
                w.push_back({});
                if (previous)
                {
                    w.insert(w.end(), previous->end() - 6, previous->end());
                    vertices += 2u;
                }
                const uint64_t gifTag = vertices | tag;
                w[tagAt] = {static_cast<uint32_t>(gifTag), static_cast<uint32_t>(gifTag >> 32), 0x412u, 0u};
                vf[20] = ps2_vsub(vf[0], vf[0]);
                for (uint32_t i = 0; i < n; ++i, p += 0x40u)
                {
                    vf[6] = loadVec(p + 0x10u);
                    vu.acc = ps2_vmul(vf[31], one);
                    vu.acc = ps2_vadd(vu.acc, ps2_vmul(vf[28], lane(vf[6], 0)));
                    vu.acc = ps2_vadd(vu.acc, ps2_vmul(vf[29], lane(vf[6], 1)));
                    vf[3] = ps2_vadd(vu.acc, ps2_vmul(vf[30], lane(vf[6], 2)));
                    vu.q = ps2_fdiv(1.0f, vecLane(vf[6], 2));
                    vf[2] = loadVec(p);
                    const uint32_t restartWord = peek32(p + 0xCu);
                    vf[4] = loadVec(p + 0x20u);
                    vf[3] = put(vf[3], ps2_vmul(vf[3], lane(vf[24], 2)), 4u);
                    vf[4] = ps2_vmul(vf[4], lane(vf[27], 1));
                    vf[2] = put(vf[2], ps2_vadd(vf[2], vf[24]), 3u);
                    vf[2] = put(vf[2], ps2_vadd(vf[0], one), 4u);
                    vf[6] = put(vf[6], ps2_vmul(vf[6], lane(vf[7], 2)), 4u);
                    const float z = peekFloat(p + 0x18u);
                    if (restart <= 0 && restartWord != 0u)
                        restart = 1;
                    vf[4] = _mm_max_ps(vf[4], vf[25]);
                    const bool behind = FPU_C_OLT_S(z, nearZ);
                    vf[3] = put(vf[3], ps2_vmul(vf[3], _mm_set1_ps(vu.q)), 4u);
                    bool clipped = behind;
                    if (!behind)
                    {
                        vf[2] = put(vf[2], ps2_vsub(vf[2], lane(vf[2], 3)), 8u);
                        vf[4] = _mm_min_ps(vf[4], vf[26]);
                        vf[5] = put(vf[5], ftoiVec(vf[3], 16.0f), 7u);
                        vf[4] = ftoiVec(vf[4], 1.0f);
                        clipped = offRange(vf[5]);
                        vf[5] = put(vf[5], ps2_vsub(vf[0], one), 8u);
                        if (!clipped)
                        {
                            vf[20] = put(vf[20], ps2_vadd(vf[20], one), 2u);
                            if (restart > 0)
                            {
                                vf[5] = put(vf[5], vf[24], 8u);
                                --restart;
                            }
                            vf[20] = put(vf[20], ps2_vadd(vf[20], lane(vf[6], 2)), 1u);
                        }
                    }
                    if (clipped)
                    {
                        vf[5] = put(vf[5], vf[24], 8u);
                        restart = 2;
                    }
                    w.push_back(vecWords(vf[2]));
                    w.push_back(vecWords(vf[4]));
                    w.push_back(vecWords(vf[5]));
                }
                packet.qwc = vertices * 3u + 1u;
                const float x = vecLane(vf[20], 0), y = vecLane(vf[20], 1);
                float depth = ps2_fdiv(y, x);
                vu.q = ps2_fdiv(x, y);
                if (FPU_C_OLT_S(0.0f, x))
                {
                    if (fixedDepth)
                        depth = -fixed;
                    packet.inserted = true;
                    packet.depth = depth;
                    call.meanW.push_back(vu.q);
                }
                else
                    call.meanW.push_back(-1.0f);
            }
            return true;
        }

        GsVertex stripVertex(const Qword &st, const Qword &rgba, const Qword &xyz)
        {
            GsVertex v;
            v.x = static_cast<float>(xyz[0] & 0xFFFFu) / 16.0f;
            v.y = static_cast<float>(xyz[1] & 0xFFFFu) / 16.0f;
            v.z = xyzfZ(xyz[2]);
            std::memcpy(&v.s, &st[0], 4);
            std::memcpy(&v.t, &st[1], 4);
            std::memcpy(&v.q, &st[2], 4);
            v.rgba = (rgba[0] & 0xFFu) | ((rgba[1] & 0xFFu) << 8) | ((rgba[2] & 0xFFu) << 16) | ((rgba[3] & 0xFFu) << 24);
            return v;
        }

        // A strip's triangles as the GS draws them: each vertex without ADC from the third on closes one with the two before.
        std::vector<GsVertex> stripTriangles(const StripPacket &packet)
        {
            std::vector<GsVertex> out;
            const size_t first = 6u;
            const size_t count = (packet.words.size() - first) / 3u;
            for (size_t i = 2; i < count; ++i)
            {
                const Qword &xyz = packet.words[first + i * 3u + 2u];
                if ((xyz[3] & 0x8000u) != 0u)
                    continue;
                for (size_t k = i - 2u; k <= i; ++k)
                    out.push_back(stripVertex(packet.words[first + k * 3u], packet.words[first + k * 3u + 1u], packet.words[first + k * 3u + 2u]));
            }
            return out;
        }

        void loadVu0(Vu0 &vu, const R5900Context *ctx)
        {
            std::memcpy(vu.vf, ctx->vu0_vf, sizeof(vu.vf));
            vu.acc = ctx->vu0_acc;
            vu.q = ctx->vu0_q;
        }

        void storeVu0(const Vu0 &vu, R5900Context *ctx)
        {
            std::memcpy(ctx->vu0_vf, vu.vf, sizeof(vu.vf));
            ctx->vu0_acc = vu.acc;
            ctx->vu0_q = vu.q;
        }

        // The REF 0x1177E0 puts in front: the page's register block (the last page past the file's count).
        Qword pageRef(uint32_t file, uint32_t index)
        {
            const uint32_t pages = peek8(file + 5u) + peek8(file + 7u) * 256u;
            const uint32_t entry = peek32(peek32(file + 0xCu) + (index < pages ? index : pages - 1u) * 4u);
            const uint32_t qwc = (entry & 0xFu) + 1u;
            return {0x30000000u | qwc, entry & ~0xFu, 0u, 0x51000000u | qwc};
        }

        void compareStrips(BuilderStats &stats, const StripCall &call, bool refHead, const std::vector<std::vector<Qword>> &game)
        {
            size_t inserted = 0;
            for (const StripPacket &p : call.packets)
                inserted += p.inserted ? 1u : 0u;
            ++stats.checked;
            if (inserted != game.size())
            {
                if (++stats.checkCountDiffs <= 4u)
                    std::fprintf(stderr, "[sc3:2d] check strips: %zu packets native, %zu game\n", inserted, game.size());
                return;
            }
            size_t g = 0;
            for (const StripPacket &p : call.packets)
            {
                if (!p.inserted)
                    continue;
                const std::vector<Qword> &words = game[g++];
                if (words.size() < p.words.size())
                {
                    if (++stats.checkCountDiffs <= 4u)
                        std::fprintf(stderr, "[sc3:2d] check strips: %zu quadwords native, %zu game\n", p.words.size(), words.size());
                    continue;
                }
                const size_t at = words.size() - p.words.size();
                if (refHead && (at != 1u || words[0] != pageRef(call.file, call.page)))
                {
                    if (++stats.checkDiffs <= 8u)
                        std::fprintf(stderr, "[sc3:2d] check strips: head of %zu quadwords, %08x %08x %08x %08x\n", at, words[0][0], words[0][1],
                                     words[0][2], words[0][3]);
                }
                for (size_t i = 0; i < p.words.size(); ++i)
                {
                    ++stats.checkSprites;
                    bool same = true;
                    for (int k = 0; k < 4; ++k)
                        same &= p.words[i][k] == kUnknownWord || p.words[i][k] == words[at + i][k];
                    if (same)
                        continue;
                    if (++stats.checkDiffs <= 8u)
                        std::fprintf(stderr, "[sc3:2d] check strips qword %zu of %zu: native %08x %08x %08x %08x; game %08x %08x %08x %08x\n", i,
                                     p.words.size(), p.words[i][0], p.words[i][1], p.words[i][2], p.words[i][3], words[at + i][0], words[at + i][1],
                                     words[at + i][2], words[at + i][3]);
                }
            }
        }

        // ---- copies of a texture file's page block (TEX1, TEX0, CLAMP) into draw items (their head 2), (file, out, page):
        // 0x1191D0 as it is (0x1FB128's table of 15 pages, 0x13BB08), 0x119318 with a CLUT offset a3 added to TEX0's CBP
        // (times 4 for 8-bit pages; 0x215998, 0x36C3D0 and others), 0x119248 with CBP a3 + (t1 ? the file's CLUT t0's
        // base +4 + t2 * 4 : t2). Recorded by where the copy goes, so a head-2 draw finds its page and CLUT without
        // reading the copy.

        constexpr uint32_t kPageCopy = 0x001191D0u, kPageCopyClut = 0x00119318u, kPageCopyCbp = 0x00119248u;

        bool resolveCopy(const State::PageCopy &copy, Page &out, BuilderStats &stats);

        void pageCopy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t at = ctx->pc, file = getRegU32(ctx, 4);
            State::PageCopy &copy = g_state.pageCopies[getRegU32(ctx, 5)];
            copy = {file, getRegU32(ctx, 6), at == kPageCopyClut ? getRegU32(ctx, 7) : 0u, false};
            if (at == kPageCopyCbp)
            {
                uint32_t base = getRegU32(ctx, 10);
                if (getRegU32(ctx, 9) != 0u)
                    base = peek16(peek32(file + 0x10u) + getRegU32(ctx, 8) * 8u + 4u) + base * 4u;
                copy.clut = (getRegU32(ctx, 7) + base) & 0x3FFFu;
                copy.absolute = true;
            }
            BuilderStats unused;
            copy.resolved = resolveCopy(copy, copy.resolvedPage, unused);
            callOriginal(at, rdram, ctx, runtime);
        }

        // The page a head-2 item's copy came from, with its CLUT (false: not copied by 0x119318, or no pack texture). Resolved
        // when the copy is made: the file it came from can be gone by the time the copy is drawn.
        bool resolveCopy(const State::PageCopy &copy, Page &out, BuilderStats &stats)
        {
            Page base;
            if (!page(copy.file, copy.page, base, stats))
                return false;
            if (copy.clut == 0u && !copy.absolute)
            {
                out = base;
                return true;
            }
            const uint32_t psm = static_cast<uint32_t>((base.tex0 >> 20) & 0x3Fu);
            const uint32_t cbp = copy.absolute ? copy.clut
                                               : (static_cast<uint32_t>((base.tex0 >> 37) & 0x3FFFu) + ((psm & 1u) == 0u ? copy.clut : copy.clut * 4u)) & 0x3FFFu;
            if (!page(copy.file, copy.page, out, stats, cbp))
                return false;
            out.tex0 = (out.tex0 & ~(0x3FFFull << 37)) | (static_cast<uint64_t>(cbp) << 37);
            return true;
        }

        bool copiedPage(uint32_t block, Page &out, BuilderStats &stats)
        {
            State &s = g_state;
            const auto copy = s.pageCopies.find(block);
            if (copy == s.pageCopies.end())
            {
                ++stats.noBlock;
                return false;
            }
            if (copy->second.resolved)
            {
                out = copy->second.resolvedPage;
                return true;
            }
            return resolveCopy(copy->second, out, stats);
        }

        // The texture file a draw item's head names: +0x14, or for a head-2 item the file its page copy came from.
        uint32_t itemTextureFile(uint32_t item)
        {
            const uint32_t file = peek32(item + 0x14u);
            if (peek8(item + 0x1Cu) == 1u)
                return loadedImageBase(file);
            if (peek8(item + 0x1Cu) != 2u)
                return file;
            const auto copy = g_state.pageCopies.find(file);
            return copy != g_state.pageCopies.end() ? copy->second.file : file;
        }

        void stripsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        void objectsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

        void trackItem(void (*draw)(uint8_t *, R5900Context *, PS2Runtime *), BuilderStats &stats, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t file = itemTextureFile(getRegU32(ctx, 4));
            const uint64_t toGs = stats.toGs;
            draw(rdram, ctx, runtime);
            if (stats.toGs != toGs || s.mode != Mode::On)
                s.itemFilesGs[file] = s.frames;
        }

        void strips(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { trackItem(stripsDraw, g_state.strips, rdram, ctx, runtime); }
        void objects(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { trackItem(objectsDraw, g_state.objects, rdram, ctx, runtime); }

        // The draw item types still the game's (the table at 0x4676A8 bar 0x110E50, 0x1134A0 and the upload 0x115600), each
        // with its end: a packet linked from inside one marks the file of the item that runs.
        constexpr std::pair<uint32_t, uint32_t> kGsItems[] = {
            {0x001117D8u, 0x00112148u}, {0x001124B8u, 0x00113250u}, {0x00113AF8u, 0x00113F50u}, {0x00115018u, 0x00115600u},
            {0x00115BE0u, 0x00115E48u}, {0x00115E48u, 0x00116128u}, {0x00116128u, 0x00116490u}};

        bool inGsItem(uint32_t ra)
        {
            for (const auto &[start, end] : kGsItems)
                if (ra >= start && ra < end)
                    return true;
            return false;
        }

        void gsItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t at = ctx->pc;
            s.gsItemFile = itemTextureFile(getRegU32(ctx, 4));
            callOriginal(at, rdram, ctx, runtime);
        }

        using StripBuild = bool (*)(uint32_t, Vu0 &, StripCall &);

        void stripItemDraw(uint32_t address, StripBuild build, BuilderStats &stats, std::map<uint32_t, uint64_t> &forms, uint8_t *rdram,
                           R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            ++stats.calls;
            const uint32_t item = getRegU32(ctx, 4);
            if (s.mode == Mode::Off)
            {
                callOriginal(address, rdram, ctx, runtime);
                return;
            }
            const uint32_t uploads = peek8(item + 0x1Au), head = peek8(item + 0x1Cu);
            ++forms[(uploads << 24) | (head << 16) | (peek8(item + 0x1Du) << 8) | (peek8(item + 0x1Fu) << 4) | (peek8(item + 0xAu) & 3u)];
            if (s.mode == Mode::Check)
            {
                Vu0 vu;
                loadVu0(vu, ctx);
                StripCall call;
                const bool built = build(item, vu, call);
                s.stripCapture = true;
                s.stripPackets.clear();
                runOriginal(address, rdram, ctx, runtime);
                s.stripCapture = false;
                if (head == 2u)
                {
                    ++s.pageCopyChecks;
                    Page copied;
                    BuilderStats why;
                    const uint32_t at = peek32(item + 0x14u);
                    const bool same = copiedPage(at, copied, why) && copied.tex0 == peek64(at + 0x20u) && copied.tex1 == peek64(at + 0x10u) &&
                                      copied.clamp == peek64(at + 0x30u);
                    if (!same && s.pageCopyMisses < 4u)
                    {
                        const auto copy = s.pageCopies.find(at);
                        packs::LoadedImages::Image image;
                        const uint32_t file = copy != s.pageCopies.end() ? copy->second.file : 0u;
                        const bool bound = s.loaded.find(file, image) && image.file;
                        std::fprintf(stderr, "[sc3:2d] check strips: the copy at %#x is not the recorded page's (TEX0 %llx recorded, %llx copied): file %#x (%s) page %u clut %u, no file %llu no block %llu no texture %llu\n", at,
                                     static_cast<unsigned long long>(copied.tex0), static_cast<unsigned long long>(peek64(at + 0x20u)), file,
                                     bound ? image.file->name.c_str() : "not a packed leaf", copy != s.pageCopies.end() ? copy->second.page : 0u,
                                     copy != s.pageCopies.end() ? copy->second.clut : 0u, static_cast<unsigned long long>(why.noFile),
                                     static_cast<unsigned long long>(why.noBlock), static_cast<unsigned long long>(why.noTexture));
                    }
                    if (!same)
                        ++s.pageCopyMisses;
                }
                if (built && peek32(kRenderer) == 0u)
                {
                    compareStrips(stats, call, head == 0u && uploads == 0u, s.stripPackets);
                    if (!call.meanW.empty() && std::memcmp(&call.meanW.back(), guest(item + 0x2Cu, 4), 4) != 0 && ++stats.checkDiffs <= 8u)
                        std::fprintf(stderr, "[sc3:2d] check strips: mean w %g native, %g game\n", call.meanW.back(), peekFloat(item + 0x2Cu));
                }
                return;
            }
            const uint32_t file = peek32(item + 0x14u);
            const uint32_t prim = (head == 1u ? 0x54u : 0x254u) | peek8(item + 0xBu);
            if (head > 2u || (prim & 0x100u) != 0u)
            {
                ++stats.toGs;
                callOriginal(address, rdram, ctx, runtime);
                return;
            }
            if (uploads != 0u && head != 1u && fileAddress(file) && (peek8(file) == 2u || peek8(file) == 3u))
            {
                ++stats.toGs;
                callOriginal(address, rdram, ctx, runtime);
                return;
            }
            Page pageRegs;
            if (peek8(item + 0x1Eu) != 0u && fileAddress(file) &&
                !(head == 2u   ? copiedPage(file, pageRegs, stats)
                  : head == 1u ? blockPage(file, pageRegs, stats, ~0u) && pageRegs.hasAlpha
                               : page(file, peek8(item + 0x18u), pageRegs, stats)))
            {
                ++stats.toGs;
                callOriginal(address, rdram, ctx, runtime);
                return;
            }
            Vu0 vu;
            loadVu0(vu, ctx);
            StripCall call;
            if (!build(item, vu, call))
            {
                if (peek8(item + 0x1Eu) != 0u)
                    poke32(kStripInsertion, call.insertion);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            poke32(kStripInsertion, call.insertion);
            GsMaterial m = spriteMaterial(pageRegs, head == 1u ? pageRegs.alpha : call.alpha);
            m.prim = ps2x::gs::decodePrim(call.prim);
            m.prim.type = GS_PRIM_TRIANGLE;
            if (head == 1u && pageRegs.hasTest)
            {
                m.test = pageRegs.test;
                m.testFromTarget = false;
            }
            for (size_t i = 0; i < call.packets.size(); ++i)
            {
                pokeFloat(item + 0x2Cu, call.meanW[i]);
                const StripPacket &packet = call.packets[i];
                if (!packet.inserted)
                    continue;
                const uint64_t key = newKey();
                const std::vector<GsVertex> triangles = stripTriangles(packet);
                if (!triangles.empty())
                    ps2x::native::Renderer::instance().drawGsKeyed(key, m, triangles.data(), static_cast<uint32_t>(triangles.size()));
                const Linked linked = insert2d(key, call.mode, packet.depth);
                if (linked != Linked::Yes && !triangles.empty())
                    discardKeyed(key);
                if (linked == Linked::Yes)
                {
                    stats.sprites += triangles.size() / 3u;
                    ++stats.markers;
                }
            }
            storeVu0(vu, ctx);
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        void stripsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            stripItemDraw(kStrips, stripBuild, g_state.strips, g_state.stripForms, rdram, ctx, runtime);
        }

        void screenStripsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            stripItemDraw(kScreenStrips, screenStripBuild, g_state.screenStrips, g_state.screenStripForms, rdram, ctx, runtime);
        }

        void screenStrips(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trackItem(screenStripsDraw, g_state.screenStrips, rdram, ctx, runtime);
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

        // ---- 0x110E50 (a0 -> item): object sprites, VU0 macro code. The item as 0x1134A0's (+0 ALPHA_2, +8 the count, +0xA
        // the insertion, +0xC the entries, +0x14 / +0x18 / +0x1A / +0x1C the texture head) and +0x10 an ST table (0: the
        // page's), +0x1D & 2 TEXA 0x80 / 0x80, & 1 an A+D first, +0x1F & 0x40 TEST_2 0x30000, & 0xF Z from the projection;
        // +0xB set to 2. An entry (0x50 bytes): +0 / +0x10 position and offset, +0x1C out w, +0x20 colour (+0x4A & 6: 0
        // RGBA bytes times the view's +0x390 halfwords, 2 a pointer to floats, else floats), +0x30 size (z the depth scale, w
        // the angle), +0x40 more size, +0x48 the ST rows, +0x4C drawn, +0x4D the corners' order. One rotated quad each (a
        // strip of four vertices, PRIM 0x254) with its own head and packet (0x10FE50), at depth +0x38 / w (Z: 1 / the
        // clamped w - size z).

        constexpr uint32_t kObjects = 0x00110E50u;
        constexpr uint32_t kObjectSt = 0x00467580u; // the ST rows of a page's whole texture (0x10FE50 scales them)
        constexpr uint32_t kObjectDepthClamp = 0x0058A530u;

        // 0x100AC0 (f12 -> *a0, *a1): its set-up in C++ and its VU0 microprogram (0x38) run as the game's vcallms runs it;
        // the quadrant picks the two lanes of vf2 it stores.
        void sinCos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Vu0 &vu, float angle, uint32_t &first, uint32_t &second)
        {
            storeVu0(vu, ctx);
            Vec *vf = ctx->vu0_vf;
            vf[2] = _mm_setr_ps(0.25f, 0.5f, 0.25f, 0.5f);
            vf[1] = _mm_set1_ps(angle);
            Vec r = ps2_vadd(vf[1], vf[2]);
            vf[1] = r;
            ps2Vu0FmacFlags(ctx->vu0_mac_flags, ctx->vu0_status, r, 15u);
            vf[3] = _mm_cvtepi32_ps(_mm_castps_si128(ftoiVec(vf[1], 1.0f)));
            r = ps2_vsub(vf[1], vf[3]);
            vf[4] = r;
            ps2Vu0FmacFlags(ctx->vu0_mac_flags, ctx->vu0_status, r, 15u);
            vf[3] = _mm_castsi128_ps(_mm_setr_epi32(0x3F400000, 0x3F400000, 0, 0));
            const __m128i bits = _mm_castps_si128(vf[4]);
            const __m128i denormal = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
            vf[4] = _mm_castsi128_ps(_mm_andnot_si128(denormal, _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF))));
            r = ps2_vsub(vf[3], vf[4]);
            vf[1] = put(vf[1], r, 3u);
            ps2Vu0FmacFlags(ctx->vu0_mac_flags, ctx->vu0_status, r, 12u);
            r = ps2_vsub(vf[4], lane(vf[2], 0));
            vf[1] = put(vf[1], r, 0xCu);
            ps2Vu0FmacFlags(ctx->vu0_mac_flags, ctx->vu0_status, r, 3u);
            const Qword fraction = vecWords(vf[4]);
            ctx->vu0_tpc = 0x38;
            ps2rExecuteVU0Microprogram(runtime, rdram, ctx, 0x38);
            const uint32_t quadrant = (static_cast<int32_t>(fraction[1]) > 0x3F000000 ? 2u : 0u) | (static_cast<int32_t>(fraction[0]) > 0x3F000000 ? 1u : 0u);
            const Qword v = vecWords(ctx->vu0_vf[2]);
            first = (quadrant & 1u) ? v[0] : v[2];
            second = (quadrant & 2u) ? v[1] : v[3];
            loadVu0(vu, ctx);
        }

        struct ObjectSprite
        {
            Qword st[4], xyz[4];
            Qword colour;
            uint32_t tex1 = 0;
            float depth = 0;
            bool inserted = false;
        };

        struct ObjectCall
        {
            uint32_t mode = 0, insertion = 0, file = 0, page = 0;
            uint64_t alpha = 0, tag = 0, regs = 0;
            bool texa = false, test = false, tex1 = false, staleSt = false;
            std::vector<ObjectSprite> sprites;
        };

        // The sprites 0x110E50 builds for `item` (false: none), VU0 on a copy that ends as the game's; 0x118020's page sizes
        // (the page's used size over its TEX0 size) from the pack's page.
        bool objectBuild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t item, Vu0 &vu, ObjectCall &call, const Page &pageRegs)
        {
            const uint32_t count = peek16(item + 8u);
            uint32_t e = peek32(item + 0xCu);
            if (count == 0u || e == 0u)
                return false;
            const uint32_t view = peek32(kView);
            call.alpha = peek64(item);
            if (call.alpha == 0u)
                call.alpha = 0x2Au;
            call.texa = (peek8(item + 0x1Du) & 2u) != 0u;
            const bool adFirst = (peek8(item + 0x1Du) & 1u) != 0u;
            call.test = (peek8(item + 0x1Fu) & 0x40u) != 0u;
            call.mode = peek8(item + 0xAu) & 3u;
            call.insertion = peek32(kStripTable + call.mode * 4u);
            call.file = peek32(item + 0x14u);
            call.page = peek8(item + 0x18u);
            uint32_t prim = 0x254u;
            if (peek8(item + 0x1Cu) == 1u)
                prim ^= 0x200u;
            call.tag = (12ull << 60) | (static_cast<uint64_t>(prim) << 47) | (1ull << 46) | 1u;
            call.regs = adFirst ? 0x424242421EFEull : 0x424242421EFFull;
            Vec *vf = vu.vf;
            const Vec one = lane(vf[0], 3);
            vf[26] = loadVec(kStripColourMax);
            vf[27] = loadVec(view + 0x380u);
            const uint32_t matrix = peek32(view + 0x448u);
            for (int i = 0; i < 4; ++i)
                vf[28 + i] = loadVec(matrix + static_cast<uint32_t>(i) * 16u);
            uint16_t scale[8];
            std::memcpy(scale, guest(view + 0x390u, 16), 16);
            const float width = peekFloat(view + 0x380u);
            for (int i = 10; i <= 13; ++i)
                vf[i] = put(vf[i], ps2_vsub(vf[i], vf[i]), 8u);
            const bool projected = (peek8(item + 0x1Fu) & 0xFu) != 0u;
            uint32_t table = peek32(item + 0x10u);
            if (table == 0u && call.file != 0u)
                table = kObjectSt;
            for (uint32_t n = 0; n < count; ++n, e += 0x50u)
            {
                vf[1] = loadVec(e);
                vf[2] = loadVec(e + 0x10u);
                vf[4] = put(vf[4], ps2_vadd(vf[1], vf[2]), 7u);
                if (peek8(e + 0x4Cu) == 0u)
                    continue;
                if (!projected)
                    vf[5] = put(vf[5], _mm_set1_ps(peekFloat(e + 0x38u)), 1u);
                vu.acc = ps2_vmul(vf[31], one);
                vu.acc = ps2_vadd(vu.acc, ps2_vmul(vf[28], lane(vf[4], 0)));
                vu.acc = ps2_vadd(vu.acc, ps2_vmul(vf[29], lane(vf[4], 1)));
                vf[3] = ps2_vadd(vu.acc, ps2_vmul(vf[30], lane(vf[4], 2)));
                vf[24] = ps2_vsub(vf[0], vf[0]);
                vu.q = ps2_fdiv(1.0f, vecLane(vf[3], 3));
                const Vec point = vf[3];
                const float w = vecLane(point, 3);
                ObjectSprite sprite;
                if (!projected)
                    vf[3] = put(vf[3], ps2_vmul(vf[3], lane(vf[5], 0)), 4u);
                vf[3] = put(vf[3], ps2_vsub(vf[3], lane(vf[3], 3)), 8u);
                std::memcpy(guest(kScratchpad + 0x50u, 16), &point, 16);
                if (!FPU_C_OLT_S(0.08f, w))
                {
                    pokeFloat(e + 0x1Cu, w);
                    continue;
                }
                vf[3] = put(vf[3], ps2_vmul(vf[3], _mm_set1_ps(vu.q)), 7u);
                const Vec screen = vf[3];
                if (table == 0u)
                {
                    if (!projected)
                        pokeFloat(e + 0x1Cu, w);
                    continue;
                }
                if (peek32(item + 0x10u) == 0u)
                {
                    const float sw = FPU_DIV_S(FPU_CVT_S_W(pageRegs.width), FPU_CVT_S_W(1u << ((pageRegs.tex0 >> 26) & 0xFu)));
                    const float sh = FPU_DIV_S(FPU_CVT_S_W(pageRegs.height), FPU_CVT_S_W(1u << ((pageRegs.tex0 >> 30) & 0xFu)));
                    pokeFloat(kObjectSt + 0x10u, sw);
                    pokeFloat(kObjectSt + 0x30u, sw);
                    pokeFloat(kObjectSt + 0x24u, sh);
                    pokeFloat(kObjectSt + 0x34u, sh);
                }
                if (projected)
                    pokeFloat(e + 0x1Cu, w);
                const float inverse = FPU_DIV_S(1.0f, w);
                const uint32_t colourKind = peek8(e + 0x4Au) & 6u;
                if (colourKind != 0u)
                {
                    vf[1] = loadVec(colourKind == 2u ? peek32(e + 0x20u) : e + 0x20u);
                    vf[1] = ps2_vmul(vf[1], lane(vf[27], 1));
                    vf[1] = _mm_min_ps(vf[1], vf[26]);
                    if (!projected)
                        vf[2] = _mm_set1_ps(w);
                    vf[1] = _mm_max_ps(vf[1], vf[24]);
                    if (!projected && FPU_C_OLT_S(w, 1.0f))
                        vf[1] = put(vf[1], ps2_vmul(vf[1], lane(vf[2], 0)), 8u);
                    vf[1] = ftoiVec(vf[1], 1.0f);
                    sprite.colour = vecWords(vf[1]);
                }
                else
                {
                    const uint32_t rgba = peek32(e + 0x20u);
                    for (int i = 0; i < 4; ++i)
                    {
                        const int32_t product = static_cast<int32_t>(static_cast<int16_t>(((rgba >> (i * 8)) & 0xFFu) + 1u)) *
                                                static_cast<int32_t>(static_cast<int16_t>(scale[i * 2]));
                        sprite.colour[i] = static_cast<uint32_t>(product) >> 8;
                    }
                }
                static constexpr int kOrder[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1}, {3, 2, 1, 0}};
                const uint32_t order = peek8(e + 0x4Du);
                const uint32_t rows = table + peek16(e + 0x48u) * 64u;
                for (int i = 0; i < 4; ++i)
                {
                    if (order < 4u)
                    {
                        const uint32_t row = rows + static_cast<uint32_t>(kOrder[order][i]) * 16u;
                        sprite.st[i] = {peek32(row), peek32(row + 4u), peek32(row + 8u), peek32(row + 12u)};
                    }
                    else
                        sprite.st[i] = {kUnknownWord, kUnknownWord, kUnknownWord, kUnknownWord};
                }
                const float f20 = FPU_MUL_S(width, inverse);
                uint32_t first, second;
                sinCos(rdram, ctx, runtime, vu, peekFloat(e + 0x3Cu), first, second);
                vf[5] = _mm_castsi128_ps(_mm_setr_epi32(static_cast<int32_t>(second), static_cast<int32_t>(first), 0, 0));
                vf[5] = put(vf[5], _mm_set1_ps(f20), 0xCu);
                vf[3] = loadVec(e + 0x30u);
                vf[24] = put(vf[24], ps2_vsub(vf[24], vf[24]), 0xAu);
                vf[4] = loadVec(e + 0x40u);
                vf[5] = put(vf[5], ps2_vmul(vf[5], lane(vf[3], 0)), 4u);
                if (!projected)
                {
                    vf[3] = put(vf[3], ps2_vmul(vf[3], lane(vf[4], 1)), 2u);
                    vf[25] = screen;
                    for (int i = 20; i <= 23; ++i)
                        vf[i] = put(vf[i], ps2_vsub(vf[i], lane(vf[i], 3)), 8u);
                    vf[5] = put(vf[5], ps2_vmul(vf[5], lane(vf[4], 0)), 4u);
                    vf[3] = put(vf[3], ps2_vmul(vf[3], lane(vf[5], 3)), 2u);
                    for (int i = 20; i <= 23; ++i)
                        vf[i] = put(vf[i], vf[25], 4u);
                    vf[24] = put(vf[24], ps2_vadd(vf[0], lane(vf[5], 0)), 5u);
                    vf[24] = put(vf[24], ps2_vadd(vf[24], lane(vf[5], 1)), 0xAu);
                    vf[24] = put(vf[24], ps2_vmul(vf[24], lane(vf[5], 2)), 3u);
                    vf[24] = put(vf[24], ps2_vmul(vf[24], lane(vf[3], 1)), 0xCu);
                    vf[24] = put(vf[24], ps2_vmul(vf[24], lane(vf[27], 2)), 9u);
                    vf[19] = put(vf[19], ps2_vmul(vf[24], lane(vf[27], 3)), 6u);
                    vf[3] = put(vf[3], ps2_vadd(vf[24], lane(vf[24], 3)), 1u);
                    vf[5] = put(vf[5], ps2_vsub(vf[19], lane(vf[19], 2)), 2u);
                    vf[4] = put(vf[4], ps2_vsub(vf[24], lane(vf[24], 3)), 1u);
                    vf[6] = put(vf[6], ps2_vadd(vf[19], lane(vf[19], 2)), 2u);
                    vf[20] = put(vf[20], ps2_vsub(vf[25], lane(vf[3], 0)), 1u);
                    vf[23] = put(vf[23], ps2_vadd(vf[25], lane(vf[3], 0)), 1u);
                    vf[21] = put(vf[21], ps2_vadd(vf[25], lane(vf[4], 0)), 1u);
                    vf[22] = put(vf[22], ps2_vsub(vf[25], lane(vf[4], 0)), 1u);
                    vf[20] = put(vf[20], ps2_vadd(vf[25], lane(vf[5], 1)), 2u);
                    vf[23] = put(vf[23], ps2_vsub(vf[25], lane(vf[5], 1)), 2u);
                    vf[21] = put(vf[21], ps2_vsub(vf[25], lane(vf[6], 1)), 2u);
                    vf[22] = put(vf[22], ps2_vadd(vf[25], lane(vf[6], 1)), 2u);
                    for (int i = 0; i < 4; ++i)
                    {
                        vf[20 + i] = ftoiVec(vf[20 + i], 16.0f);
                        sprite.xyz[i] = vecWords(vf[20 + i]);
                    }
                    sprite.depth = FPU_MUL_S(peekFloat(e + 0x38u), inverse);
                }
                else
                {
                    const uint32_t projection = peek32(view + 0x454u);
                    const Qword p = vecWords(point);
                    vf[18] = _mm_castsi128_ps(_mm_setr_epi32(static_cast<int32_t>(p[2]), 0, static_cast<int32_t>(p[3]), 0));
                    vf[19] = loadVec(kObjectDepthClamp);
                    vf[18] = put(vf[18], ps2_vsub(vf[18], lane(vf[3], 2)), 4u);
                    vf[21] = put(vf[21], ps2_vadd(vf[0], lane(vf[19], 3)), 4u);
                    vf[22] = loadVec(projection + 0x20u);
                    vf[23] = loadVec(projection + 0x30u);
                    vf[18] = put(vf[18], _mm_max_ps(vf[18], vf[19]), 4u);
                    vf[24] = put(vf[24], ps2_vsub(vf[24], vf[24]), 0xAu);
                    vf[3] = put(vf[3], ps2_vmul(vf[3], lane(vf[4], 1)), 2u);
                    vf[18] = put(vf[18], _mm_min_ps(vf[18], vf[21]), 4u);
                    vf[24] = put(vf[24], ps2_vadd(vf[0], lane(vf[5], 0)), 5u);
                    vf[5] = put(vf[5], ps2_vmul(vf[5], lane(vf[4], 0)), 4u);
                    vf[24] = put(vf[24], ps2_vadd(vf[24], lane(vf[5], 1)), 0xAu);
                    vf[3] = put(vf[3], ps2_vmul(vf[3], lane(vf[5], 3)), 2u);
                    vu.q = ps2_fdiv(1.0f, vecLane(vf[18], 2));
                    vf[18] = put(vf[18], ps2_vmul(vf[18], lane(vf[22], 2)), 4u);
                    vf[24] = put(vf[24], ps2_vmul(vf[24], lane(vf[5], 2)), 3u);
                    vf[25] = screen;
                    vf[24] = put(vf[24], ps2_vmul(vf[24], lane(vf[3], 1)), 0xCu);
                    vf[18] = put(vf[18], ps2_vadd(vf[18], lane(vf[23], 2)), 4u);
                    vf[25] = put(vf[25], ps2_vsub(vf[25], lane(vf[25], 3)), 8u);
                    vf[20] = ps2_vadd(vf[0], _mm_set1_ps(vu.q));
                    vf[18] = put(vf[18], ps2_vmul(vf[18], _mm_set1_ps(vu.q)), 4u);
                    vf[25] = put(vf[25], ps2_vsub(vf[25], one), 8u);
                    sprite.depth = vecLane(vf[20], 0);
                    vf[25] = put(vf[25], vf[18], 4u);
                    vf[1] = put(vf[1], ps2_vsub(vf[25], lane(vf[24], 0)), 1u);
                    vf[1] = put(vf[1], ps2_vadd(vf[25], lane(vf[24], 1)), 2u);
                    vf[2] = put(vf[2], ps2_vadd(vf[25], lane(vf[24], 0)), 1u);
                    vf[2] = put(vf[2], ps2_vsub(vf[25], lane(vf[24], 1)), 2u);
                    const Vec zw = ps2_vadd(vf[0], vf[25]);
                    vf[20] = put(put(put(vf[20], ps2_vsub(vf[1], lane(vf[24], 3)), 1u), ps2_vsub(vf[1], lane(vf[24], 2)), 2u), zw, 0xCu);
                    vf[21] = put(put(put(vf[21], ps2_vsub(vf[2], lane(vf[24], 3)), 1u), ps2_vsub(vf[2], lane(vf[24], 2)), 2u), zw, 0xCu);
                    vf[22] = put(put(put(vf[22], ps2_vadd(vf[1], lane(vf[24], 3)), 1u), ps2_vadd(vf[1], lane(vf[24], 2)), 2u), zw, 0xCu);
                    vf[23] = put(put(put(vf[23], ps2_vadd(vf[2], lane(vf[24], 3)), 1u), ps2_vadd(vf[2], lane(vf[24], 2)), 2u), zw, 0xCu);
                    for (int i = 0; i < 4; ++i)
                    {
                        vf[10 + i] = put(vf[10 + i], ftoiVec(vf[20 + i], 16.0f), 7u);
                        sprite.xyz[i] = vecWords(vf[10 + i]);
                    }
                }
                bool off = false;
                for (const Qword &xyz : sprite.xyz)
                    off |= ((xyz[0] >> 16) | (xyz[1] >> 16)) != 0u;
                if (!projected)
                    pokeFloat(e + 0x1Cu, w);
                sprite.inserted = !off;
                call.sprites.push_back(sprite);
            }
            return true;
        }

        // The quadwords of a sprite's packet from its DMA tag slot (TEXA and TEST_2 A+D packets before it when the item asks).
        std::vector<Qword> objectWords(const ObjectCall &call, const ObjectSprite &sprite)
        {
            std::vector<Qword> w;
            if (call.texa)
            {
                w.push_back({0x10000002u, kUnknownWord, kUnknownWord, kUnknownWord});
                w.push_back({1u, 0x10000000u, 0xEu, 0u});
                w.push_back({0x80u, 0x80u, 0x3Bu, 0u});
            }
            if (call.test)
            {
                w.push_back({0x10000002u, kUnknownWord, kUnknownWord, kUnknownWord});
                w.push_back({1u, 0x10000000u, 0xEu, 0u});
                w.push_back({0x30000u, 0u, 0x48u, 0u});
            }
            w.push_back({kUnknownWord, kUnknownWord, kUnknownWord, kUnknownWord});
            w.push_back({static_cast<uint32_t>(call.tag), static_cast<uint32_t>(call.tag >> 32), static_cast<uint32_t>(call.regs),
                         static_cast<uint32_t>(call.regs >> 32)});
            w.push_back({0u, 0u, 0u, 0u});
            w.push_back({0u, 0u, 0u, 0u});
            w.push_back({static_cast<uint32_t>(call.alpha), static_cast<uint32_t>(call.alpha >> 32), 0x43u, 0u});
            w.push_back(sprite.colour);
            for (int i = 0; i < 4; ++i)
            {
                w.push_back(sprite.st[i]);
                w.push_back(sprite.xyz[i]);
            }
            return w;
        }

        using ObjectWords = std::vector<Qword> (*)(const ObjectCall &, const ObjectSprite &);

        void compareObjects(BuilderStats &stats, const ObjectCall &call, bool refHead, const std::vector<std::vector<Qword>> &game,
                            ObjectWords objectWords, const char *name)
        {
            size_t inserted = 0;
            for (const ObjectSprite &o : call.sprites)
                inserted += o.inserted ? 1u : 0u;
            ++stats.checked;
            if (inserted != game.size())
            {
                if (++stats.checkCountDiffs <= 4u)
                    std::fprintf(stderr, "[sc3:2d] check %s: %zu sprites native, %zu game\n", name, inserted, game.size());
                return;
            }
            size_t g = 0;
            for (const ObjectSprite &o : call.sprites)
            {
                if (!o.inserted)
                    continue;
                const std::vector<Qword> &words = game[g++];
                const std::vector<Qword> expected = objectWords(call, o);
                if (words.size() < expected.size())
                {
                    if (++stats.checkCountDiffs <= 4u)
                        std::fprintf(stderr, "[sc3:2d] check %s: %zu quadwords native, %zu game\n", name, expected.size(), words.size());
                    continue;
                }
                const size_t at = words.size() - expected.size();
                if (refHead && (at != 1u || words[0] != pageRef(call.file, call.page)) && ++stats.checkDiffs <= 8u)
                    std::fprintf(stderr, "[sc3:2d] check %s: head of %zu quadwords, %08x %08x %08x %08x\n", name, at, words[0][0], words[0][1],
                                 words[0][2], words[0][3]);
                for (size_t i = 0; i < expected.size(); ++i)
                {
                    ++stats.checkSprites;
                    bool same = true;
                    for (int k = 0; k < 4; ++k)
                        same &= expected[i][k] == kUnknownWord || expected[i][k] == words[at + i][k];
                    if (!same && ++stats.checkDiffs <= 8u)
                        std::fprintf(stderr, "[sc3:2d] check %s qword %zu of %zu: native %08x %08x %08x %08x; game %08x %08x %08x %08x\n", name, i,
                                     expected.size(), expected[i][0], expected[i][1], expected[i][2], expected[i][3], words[at + i][0],
                                     words[at + i][1], words[at + i][2], words[at + i][3]);
                }
            }
        }

        void objectsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            BuilderStats &stats = s.objects;
            ++stats.calls;
            const uint32_t item = getRegU32(ctx, 4);
            if (s.mode == Mode::Off)
            {
                callOriginal(kObjects, rdram, ctx, runtime);
                return;
            }
            const uint32_t uploads = peek8(item + 0x1Au), head = peek8(item + 0x1Cu);
            ++s.objectForms[(uploads << 24) | (head << 16) | (peek8(item + 0x1Du) << 8) | peek8(item + 0x1Fu)];
            if (s.mode == Mode::Check)
            {
                Vu0 entry, vu;
                loadVu0(entry, ctx);
                vu = entry;
                ObjectCall call;
                Page pageRegs;
                BuilderStats unused;
                const uint32_t file = peek32(item + 0x14u);
                if (peek32(item + 0x10u) == 0u && (head != 0u || !page(file, peek8(item + 0x18u), pageRegs, unused)))
                    pageRegs.width = pageRegs.height = 0u;
                const bool built = objectBuild(rdram, ctx, runtime, item, vu, call, pageRegs);
                storeVu0(entry, ctx);
                s.stripCapture = true;
                s.stripPackets.clear();
                runOriginal(kObjects, rdram, ctx, runtime);
                s.stripCapture = false;
                if (built && peek32(kRenderer) == 0u)
                {
                    compareObjects(stats, call, head == 0u && uploads == 0u, s.stripPackets, objectWords, "objects");
                }
                return;
            }
            const uint32_t file = peek32(item + 0x14u);
            Page pageRegs;
            if (head > 2u || (peek8(item + 0x1Du) & 1u) != 0u || !fileAddress(file) || (head != 0u && peek32(item + 0x10u) == 0u) ||
                (uploads != 0u && head == 0u && (peek8(file) == 2u || peek8(file) == 3u)))
            {
                ++stats.toGs;
                callOriginal(kObjects, rdram, ctx, runtime);
                return;
            }
            const bool found = head == 0u   ? page(file, peek8(item + 0x18u), pageRegs, stats)
                               : head == 2u ? copiedPage(file, pageRegs, stats)
                                            : blockPage(file, pageRegs, stats, ~0u) && pageRegs.hasAlpha;
            if (!found)
            {
                ++stats.toGs;
                callOriginal(kObjects, rdram, ctx, runtime);
                return;
            }
            if (peek16(item + 8u) == 0u || peek32(item + 0xCu) == 0u)
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            Vu0 vu;
            loadVu0(vu, ctx);
            poke8(item + 0xBu, 2u);
            poke32(kStripInsertion, peek32(kStripTable + (peek8(item + 0xAu) & 3u) * 4u));
            ObjectCall call;
            if (!objectBuild(rdram, ctx, runtime, item, vu, call, pageRegs))
            {
                storeVu0(vu, ctx);
                ++stats.native;
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            GsMaterial m = spriteMaterial(pageRegs, head == 1u ? pageRegs.alpha : call.alpha);
            m.prim = ps2x::gs::decodePrim(static_cast<uint32_t>(call.tag >> 47) & 0x7FFu);
            m.prim.type = GS_PRIM_TRIANGLE;
            if (head == 1u ? pageRegs.hasTest : call.test)
            {
                m.test = head == 1u ? pageRegs.test : 0x30000u;
                m.testFromTarget = false;
            }
            for (const ObjectSprite &o : call.sprites)
            {
                if (!o.inserted)
                    continue;
                GsVertex v[4];
                for (int i = 0; i < 4; ++i)
                    v[i] = stripVertex(o.st[i], o.colour, o.xyz[i]);
                const GsVertex triangles[6] = {v[0], v[1], v[2], v[1], v[2], v[3]};
                const uint64_t key = newKey();
                ps2x::native::Renderer::instance().drawGsKeyed(key, m, triangles, 6u);
                if (insert2d(key, call.mode, o.depth) != Linked::Yes)
                    discardKeyed(key);
                else
                {
                    stats.sprites += 1u;
                    ++stats.markers;
                }
            }
            storeVu0(vu, ctx);
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- 0x1107E0 (draw item type 6) and 0x110258 (type 2): 0x110E50's siblings in the 2D view (*(view +0x460), the matrix
        // 0x10FD78 loads with a3 = 1), the same item, entries and head (0x10FE50). The position's Z is the depth, not divided
        // through: Z = the view's z times the depth scale (+0x38, 1 when negative) over the position's z, drawn when that z is
        // over 0.08. Depth for the insertion: +0x38 over the z, or when +0x38 is not positive -(+0x38) (x 12.5 for mode 1).
        // +0x4A & 0x40 with +0x1F & 0x80 or *(*(0x469230) +0x50) (the display's aspect) picks the width correction.

        constexpr uint32_t kScreenObjects = 0x001107E0u, kScreenSprites = 0x00110258u, kBillboards = 0x00112148u, kGifListAppend = 0x0010F808u;
        constexpr uint32_t kScreenTex1 = 0x00467528u; // TEX1_2 by an entry's +0x4B (the item's +0x1D & 1)
        constexpr uint32_t kDisplay = 0x00469230u;    // +0x50: the screen's aspect is not the game's
        constexpr uint32_t kScreenSpriteAspect = 0x00467610u;

        // 0x10FD78: the colour cap, the view's +0x380 and a matrix in vf26 .. vf31, the 2D one (a3 = 1) or the 3D one (a3 = 0).
        void screenView(Vu0 &vu, uint32_t view, uint32_t slot)
        {
            vu.vf[26] = loadVec(kStripColourMax);
            vu.vf[27] = loadVec(view + 0x380u);
            const uint32_t matrix = peek32(view + slot);
            for (int i = 0; i < 4; ++i)
                vu.vf[28 + i] = loadVec(matrix + static_cast<uint32_t>(i) * 16u);
        }

        // vf3 = the matrix (vf28 .. vf31) times (p, 1), as the VU0 macro code's ACC chain.
        Vec screenPoint(const Vu0 &vu, Vec p)
        {
            Vec acc = ps2_vmul(vu.vf[31], lane(vu.vf[0], 3));
            acc = ps2_vadd(acc, ps2_vmul(vu.vf[28], lane(p, 0)));
            acc = ps2_vadd(acc, ps2_vmul(vu.vf[29], lane(p, 1)));
            return ps2_vadd(acc, ps2_vmul(vu.vf[30], lane(p, 2)));
        }

        // 0x10FE50's ST table: the item's +0x10, else the page's whole texture (0x467580, scaled to the page's used size).
        uint32_t screenTable(uint32_t item, const Page &pageRegs)
        {
            const uint32_t table = peek32(item + 0x10u);
            if (table != 0u || peek32(item + 0x14u) == 0u)
                return table;
            const float sw = FPU_DIV_S(FPU_CVT_S_W(pageRegs.width), FPU_CVT_S_W(1u << ((pageRegs.tex0 >> 26) & 0xFu)));
            const float sh = FPU_DIV_S(FPU_CVT_S_W(pageRegs.height), FPU_CVT_S_W(1u << ((pageRegs.tex0 >> 30) & 0xFu)));
            pokeFloat(kObjectSt + 0x10u, sw);
            pokeFloat(kObjectSt + 0x30u, sw);
            pokeFloat(kObjectSt + 0x24u, sh);
            pokeFloat(kObjectSt + 0x34u, sh);
            return kObjectSt;
        }

        // 0x10FD78 / 0x10FE50's head fields of the call: ALPHA_2, TEXA, TEST_2 and the TEX1_2 A+D first.
        void screenCall(uint32_t item, ObjectCall &call)
        {
            call.alpha = peek64(item);
            if (call.alpha == 0u)
                call.alpha = 0x2Au;
            call.texa = (peek8(item + 0x1Du) & 2u) != 0u;
            call.tex1 = (peek8(item + 0x1Du) & 1u) != 0u;
            call.test = (peek8(item + 0x1Fu) & 0x40u) != 0u;
            call.mode = peek8(item + 0xAu);
            call.insertion = peek32(kStripTable + (call.mode & 3u) * 4u);
            call.file = peek32(item + 0x14u);
            call.page = peek8(item + 0x18u);
        }

        // Colour from an entry: +0x4A & 6 nonzero floats (2: through a pointer) times the view's +0x384, capped, from `low`
        // up; else the RGBA bytes of +0x20, each (byte + 1) times the view's +0x390 halfword, over 256 (`halve`: (byte + 1) / 2).
        Qword screenColour(const Vu0 &vu, uint32_t e, Vec low, bool halve)
        {
            const uint32_t kind = peek8(e + 0x4Au) & 6u;
            if (kind != 0u)
            {
                Vec c = ps2_vmul(loadVec(kind == 2u ? peek32(e + 0x20u) : e + 0x20u), lane(vu.vf[27], 1));
                c = _mm_max_ps(_mm_min_ps(c, vu.vf[26]), low);
                return vecWords(ftoiVec(c, 1.0f));
            }
            const uint32_t rgba = peek32(e + 0x20u);
            const uint32_t view = peek32(kView);
            Qword out;
            for (int i = 0; i < 4; ++i)
            {
                const uint32_t b = ((rgba >> (i * 8)) & 0xFFu) + 1u;
                out[i] = halve ? b >> 1 : static_cast<uint32_t>(static_cast<int32_t>(b) * static_cast<int16_t>(peek16(view + 0x390u + i * 4u))) >> 8;
            }
            return out;
        }

        float wordFloat(uint32_t bits)
        {
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        // 0x110258's order: ACC = the rows times x, y, z, then vf3 = ACC + vf31.
        Vec screenPointLast(const Vu0 &vu, Vec p)
        {
            Vec acc = ps2_vmul(vu.vf[28], lane(p, 0));
            acc = ps2_vadd(acc, ps2_vmul(vu.vf[29], lane(p, 1)));
            acc = ps2_vadd(acc, ps2_vmul(vu.vf[30], lane(p, 2)));
            return ps2_vadd(acc, ps2_vmul(vu.vf[31], lane(vu.vf[0], 3)));
        }

        Qword stRow(uint32_t row) { return {peek32(row), peek32(row + 4u), peek32(row + 8u), peek32(row + 12u)}; }

        // The sprites 0x1107E0 builds for `item` (false: none): one rotated quad (four vertices of a strip) per entry.
        bool screenObjectsBuild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t item, Vu0 &vu, ObjectCall &call,
                                const Page &pageRegs)
        {
            const uint32_t count = peek16(item + 8u);
            uint32_t e = peek32(item + 0xCu);
            if (count == 0u || e == 0u)
                return false;
            const uint32_t view = peek32(kView);
            screenCall(item, call);
            uint32_t prim = 0x254u;
            if (peek8(item + 0x1Cu) == 1u)
                prim ^= 0x200u;
            call.tag = (12ull << 60) | (static_cast<uint64_t>(prim) << 47) | (1ull << 46) | 1u;
            call.regs = call.tex1 ? 0x424242421EFEull : 0x424242421EFFull;
            Vec *vf = vu.vf;
            screenView(vu, view, kView2dMatrix);
            for (int i = 10; i <= 13; ++i)
                vf[i] = put(vf[i], ps2_vsub(vf[i], vf[i]), 8u);
            const bool wideFlag = (peek8(item + 0x1Fu) & 0x80u) != 0u;
            const bool aspect = peek32(peek32(kDisplay) + 0x50u) != 0u;
            for (uint32_t n = 0; n < count; ++n, e += 0x50u)
            {
                if (peek8(e + 0x4Cu) == 0u)
                    continue;
                const Vec p = put(vf[4], ps2_vadd(loadVec(e), loadVec(e + 0x10u)), 7u);
                vf[4] = p;
                const float f21 = peekFloat(e + 0x38u);
                const float depthScale = FPU_C_OLT_S(f21, 0.0f) ? 1.0f : f21;
                vf[24] = ps2_vsub(vf[0], vf[0]);
                Vec point = screenPoint(vu, p);
                vu.q = ps2_fdiv(1.0f, vecLane(p, 2));
                point = put(point, ps2_vadd(vf[24], lane(p, 2)), 8u);
                std::memcpy(guest(kScratchpad + 0x50u, 16), &point, 16);
                const uint32_t outW = peek32(kScratchpad + 0x5Cu);
                Vec screen = put(point, ps2_vmul(point, _mm_set1_ps(depthScale)), 4u);
                screen = put(screen, ps2_vsub(screen, lane(screen, 3)), 8u);
                vf[3] = screen;
                const float z = vecLane(p, 2);
                if (!FPU_C_OLT_S(0.08f, z))
                {
                    poke32(e + 0x1Cu, outW);
                    continue;
                }
                const uint32_t table = screenTable(item, pageRegs);
                if (table == 0u)
                {
                    poke32(e + 0x1Cu, outW);
                    continue;
                }
                screen = put(screen, ps2_vmul(screen, _mm_set1_ps(vu.q)), 4u);
                ObjectSprite sprite;
                if (call.tex1)
                    sprite.tex1 = peek32(kScreenTex1 + peek8(e + 0x4Bu) * 4u);
                sprite.colour = screenColour(vu, e, vf[24], false);
                static constexpr int kOrder[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1}, {3, 2, 1, 0}};
                const uint32_t order = peek8(e + 0x4Du);
                const uint32_t rows = table + peek16(e + 0x48u) * 64u;
                for (int i = 0; i < 4; ++i)
                    sprite.st[i] = order < 4u ? stRow(rows + static_cast<uint32_t>(kOrder[order][i]) * 16u)
                                              : Qword{kUnknownWord, kUnknownWord, kUnknownWord, kUnknownWord};
                call.staleSt |= order >= 4u;
                const float depth = FPU_MUL_S(FPU_DIV_S(1.0f, z), depthScale);
                uint32_t first, second;
                sinCos(rdram, ctx, runtime, vu, peekFloat(e + 0x3Cu), first, second);
                const uint8_t flags = peek8(e + 0x4Au);
                uint32_t vec = 0x00467620u;
                float offset = 0.0f, scale = 1.0f;
                if ((flags & 0x40u) != 0u && (aspect || wideFlag))
                {
                    if (aspect && wideFlag)
                        vec = 0x00467650u;
                    else
                    {
                        vec = aspect ? 0x00467640u : 0x00467630u;
                        if ((flags & 0x80u) != 0u)
                        {
                            scale = wordFloat(aspect ? 0x3F4DB9CCu : 0x3F9F47ADu);
                            offset = 2048.0f;
                        }
                    }
                }
                const float x = FPU_ADD_S(FPU_MUL_S(FPU_SUB_S(vecLane(screen, 0), offset), scale), offset);
                const Vec size = put(loadVec(e + 0x30u), ps2_vmul(loadVec(e + 0x30u), loadVec(e + 0x40u)), 3u);
                Vec v19 = put(screen, ftoiVec(screen, 16.0f), 0xCu);
                const Vec v4 = put(loadVec(vec), ps2_vmul(loadVec(vec), size), 3u);
                const Vec v6 = _mm_castsi128_ps(_mm_setr_epi32(static_cast<int32_t>(second), static_cast<int32_t>(first), 0, 0));
                v19 = put(v19, _mm_set1_ps(x), 1u);
                const Vec v25 = ps2_vmul(v6, lane(v4, 1));
                const Vec v24 = ps2_vmul(v6, lane(v4, 0));
                Vec v20 = v19, v21 = v19, v22 = v19, v23 = v19;
                v23 = put(v23, ps2_vadd(v24, lane(v25, 1)), 1u);
                v20 = put(v20, ps2_vsub(v24, lane(v25, 0)), 2u);
                v21 = put(v21, ps2_vsub(v24, lane(v25, 1)), 1u);
                v22 = put(v22, ps2_vadd(v24, lane(v25, 0)), 2u);
                v23 = put(v23, ps2_vmul(v23, lane(v4, 2)), 1u);
                v20 = put(v20, ps2_vmul(v20, lane(v4, 3)), 2u);
                v21 = put(v21, ps2_vmul(v21, lane(v4, 2)), 1u);
                v22 = put(v22, ps2_vmul(v22, lane(v4, 3)), 2u);
                v20 = put(v20, ps2_vsub(v19, lane(v23, 0)), 1u);
                v23 = put(v23, ps2_vadd(v19, lane(v23, 0)), 1u);
                v22 = put(v22, ps2_vsub(v19, lane(v21, 0)), 1u);
                v21 = put(v21, ps2_vadd(v19, lane(v21, 0)), 1u);
                v23 = put(v23, ps2_vsub(v19, lane(v20, 1)), 2u);
                v20 = put(v20, ps2_vadd(v19, lane(v20, 1)), 2u);
                v21 = put(v21, ps2_vsub(v19, lane(v22, 1)), 2u);
                v22 = put(v22, ps2_vadd(v19, lane(v22, 1)), 2u);
                const Vec corners[4] = {v20, v21, v22, v23};
                for (int i = 0; i < 4; ++i)
                {
                    vf[20 + i] = put(corners[i], ftoiVec(corners[i], 16.0f), 3u);
                    sprite.xyz[i] = vecWords(vf[20 + i]);
                }
                vf[19] = v19;
                sprite.depth = FPU_C_OLT_S(0.0f, f21) ? depth : call.mode == 1u ? FPU_MUL_S(-f21, 12.5f) : -f21;
                sprite.inserted = true;
                poke32(e + 0x1Cu, outW);
                call.sprites.push_back(sprite);
            }
            return true;
        }

        // The sprites 0x110258 builds for `item` (false: none, also when the heap is near full: 0x10D528): one GS sprite (two
        // corners) per entry, its size +0x30 / +0x34 times +0x40, its anchor by +0x4A >> 4 & 3; dropped off the GS's range.
        bool screenSpritesBuild(uint32_t item, Vu0 &vu, ObjectCall &call, const Page &pageRegs)
        {
            if (static_cast<int32_t>(peek32(kRenderer + 0x2Cu) + peek32(kRenderer + 0x1Cu) + 0x64u) > 0x895C)
                return false;
            const uint32_t count = peek16(item + 8u);
            uint32_t e = peek32(item + 0xCu);
            if (count == 0u || e == 0u)
                return false;
            const uint32_t view = peek32(kView);
            screenCall(item, call);
            uint64_t prim = 0x95A0ull << 41;
            if (peek8(item + 0x1Cu) == 1u)
                prim ^= 1ull << 56;
            call.tag = ((call.tex1 ? 7ull : 6ull) << 60) | prim | 0x8000u | 1u;
            call.regs = call.tex1 ? 0x042421EEull : 0x0042421Eull;
            Vec *vf = vu.vf;
            screenView(vu, view, kView2dMatrix);
            const bool wideFlag = (peek8(item + 0x1Fu) & 0x80u) != 0u;
            const bool aspect = peek32(peek32(kDisplay) + 0x50u) != 0u;
            const bool corrected = wideFlag ? !aspect : aspect;
            for (uint32_t n = 0; n < count; ++n, e += 0x50u)
            {
                if (peek8(e + 0x4Cu) == 0u)
                    continue;
                const uint32_t table = screenTable(item, pageRegs);
                if (table == 0u)
                    continue;
                const float w = peekFloat(e + 0x30u), h = peekFloat(e + 0x34u);
                if (!FPU_C_OLT_S(0.0f, w) || !FPU_C_OLT_S(0.0f, h))
                    continue;
                const float d = peekFloat(e + 0x38u);
                const float depthScale = FPU_C_OLT_S(0.0f, d) ? d : 1.0f;
                const uint8_t flags = peek8(e + 0x4Au);
                const bool fix = corrected && (flags & 0x40u) != 0u;
                ObjectSprite sprite;
                if (call.tex1)
                {
                    uint32_t index = peek8(e + 0x4Bu);
                    if (fix && index == 1u)
                        index = 2u;
                    sprite.tex1 = peek32(kScreenTex1 + index * 4u);
                }
                const Vec p = put(loadVec(e), ps2_vadd(loadVec(e), loadVec(e + 0x10u)), 7u);
                vf[4] = p;
                Vec v3 = screenPointLast(vu, p);
                vu.q = ps2_fdiv(1.0f, vecLane(p, 2));
                const uint32_t kind = flags & 6u;
                if (kind != 0u)
                    vf[24] = ps2_vsub(vf[0], lane(vf[0], 3));
                if (!FPU_C_OLT_S(0.08f, peekFloat(e + 8u)))
                    continue;
                v3 = put(v3, ps2_vmul(v3, _mm_set1_ps(vu.q)), 4u);
                const float depth = FPU_MUL_S(vu.q, depthScale);
                pokeFloat(e + 0x1Cu, depth);
                Vec v6 = _mm_setr_ps(w, h, depthScale, 0.0f);
                const Vec v7 = loadVec(e + 0x40u);
                v3 = put(v3, ps2_vsub(v3, v3), 8u);
                sprite.colour = screenColour(vu, e, vf[24], true);
                Vec v8 = put(v6, ps2_vadd(v6, lane(vf[0], 3)), 3u);
                v3 = put(v3, _mm_max_ps(v3, vf[0]), 4u);
                v6 = put(v6, ps2_vmul(v6, v7), 3u);
                v8 = put(v8, ps2_vmul(v8, v7), 3u);
                const uint32_t order = peek8(e + 0x4Du);
                const uint32_t rows = table + peek16(e + 0x48u) * 64u;
                static constexpr uint32_t kRows[4][2] = {{0u, 3u}, {1u, 2u}, {2u, 1u}, {3u, 0u}};
                const uint32_t *r = kRows[order < 4u ? order : 0u];
                sprite.st[0] = stRow(rows + r[0] * 16u);
                sprite.st[1] = stRow(rows + r[1] * 16u);
                if (fix)
                {
                    const Vec a = loadVec(kScreenSpriteAspect);
                    const Vec factor = lane(a, wideFlag ? 3 : 2);
                    if ((flags & 0x80u) != 0u)
                        v3 = put(v3, ps2_vsub(v3, lane(a, 0)), 1u);
                    v6 = put(v6, ps2_vmul(v6, factor), 1u);
                    v8 = put(v8, ps2_vmul(v8, factor), 1u);
                    if ((flags & 0x80u) != 0u)
                    {
                        v3 = put(v3, ps2_vmul(v3, factor), 1u);
                        v3 = put(v3, ps2_vadd(v3, lane(a, 0)), 1u);
                    }
                }
                Vec v4 = v3;
                switch ((flags >> 4) & 3u)
                {
                case 1u:
                    v4 = put(v4, ps2_vadd(v4, v6), 3u);
                    break;
                case 2u:
                    v3 = put(v3, ps2_vsub(v3, v6), 1u);
                    v4 = put(v4, ps2_vadd(v4, v6), 2u);
                    break;
                default:
                    v3 = put(v3, ps2_vsub(v3, v6), 3u);
                    v4 = put(v4, ps2_vadd(v4, v8), 3u);
                    break;
                }
                v3 = put(v3, ftoiVec(v3, 16.0f), 7u);
                v4 = put(v4, ftoiVec(v4, 16.0f), 7u);
                vf[3] = v3;
                vf[6] = v6;
                vf[7] = v7;
                vf[8] = v8;
                if (offRange(v3) || offRange(v4))
                    continue;
                sprite.xyz[0] = vecWords(v3);
                sprite.xyz[1] = vecWords(v4);
                sprite.depth = FPU_C_OLT_S(0.0f, d) ? depth : call.mode == 1u ? FPU_MUL_S(-d, 12.5f) : -d;
                sprite.inserted = true;
                call.sprites.push_back(sprite);
            }
            return true;
        }

        // The sprites 0x112148 builds for `item` (false: none, also when the heap is near full): one GS sprite per entry at
        // its position (+0 plus +0x10, w +0xC) through the view's 3D matrix, w out to +0x1C, drawn when w is over 0.08; its
        // half size +0x30 / +0x34 times the view's +0x388 / +0x38C, +0x40 and Q times the view's +0x380, centred; Z times
        // +0x38, the depth Q times +0x38. Dropped off the GS's range.
        bool billboardsBuild(uint32_t item, Vu0 &vu, ObjectCall &call, const Page &pageRegs)
        {
            if (static_cast<int32_t>(peek32(kRenderer + 0x2Cu) + peek32(kRenderer + 0x1Cu) + 0x64u) > 0x895C)
                return false;
            const uint32_t count = peek16(item + 8u);
            uint32_t e = peek32(item + 0xCu);
            if (count == 0u || e == 0u)
                return false;
            const uint32_t view = peek32(kView);
            screenCall(item, call);
            uint64_t prim = 0x95A0ull << 41;
            if (peek8(item + 0x1Cu) == 1u)
                prim ^= 1ull << 56;
            call.tag = ((call.tex1 ? 7ull : 6ull) << 60) | prim | 0x8000u | 1u;
            call.regs = call.tex1 ? 0x042421EEull : 0x0042421Eull;
            Vec *vf = vu.vf;
            screenView(vu, view, kView3dMatrix);
            for (uint32_t n = 0; n < count; ++n, e += 0x50u)
            {
                if (peek8(e + 0x4Cu) == 0u)
                    continue;
                const uint32_t table = screenTable(item, pageRegs);
                if (table == 0u)
                    continue;
                ObjectSprite sprite;
                if (call.tex1)
                    sprite.tex1 = peek32(kScreenTex1 + peek8(e + 0x4Bu) * 4u);
                const Vec p = put(loadVec(e), ps2_vadd(loadVec(e), loadVec(e + 0x10u)), 7u);
                vf[4] = p;
                Vec acc = ps2_vmul(vf[28], lane(p, 0));
                acc = ps2_vadd(acc, ps2_vmul(vf[29], lane(p, 1)));
                acc = ps2_vadd(acc, ps2_vmul(vf[30], lane(p, 2)));
                Vec v3 = ps2_vadd(acc, ps2_vmul(vf[31], lane(p, 3)));
                const float w = vecLane(v3, 3);
                vu.q = ps2_fdiv(1.0f, w);
                const uint8_t flags = peek8(e + 0x4Au);
                if ((flags & 6u) != 0u)
                    vf[24] = ps2_vsub(vf[0], lane(vf[0], 3));
                pokeFloat(e + 0x1Cu, w);
                vf[3] = v3;
                if (!FPU_C_OLT_S(0.08f, w))
                    continue;
                v3 = put(v3, ps2_vmul(v3, _mm_set1_ps(vu.q)), 7u);
                vf[1] = put(vf[1], _mm_set1_ps(vu.q), 1u);
                const float depth = FPU_MUL_S(vu.q, peekFloat(e + 0x38u));
                Vec v6 = loadVec(e + 0x30u);
                const Vec v7 = loadVec(e + 0x40u);
                v6 = put(v6, ps2_vmul(v6, lane(vf[27], 2)), 1u);
                v3 = put(v3, ps2_vsub(v3, v3), 8u);
                sprite.colour = screenColour(vu, e, vf[24], false);
                v6 = put(v6, ps2_vmul(v6, lane(vf[27], 3)), 2u);
                v6 = put(v6, ps2_vmul(v6, v7), 3u);
                v3 = put(v3, ps2_vmul(v3, lane(v6, 2)), 4u);
                vf[2] = put(vf[2], ps2_vmul(vf[1], vf[27]), 1u);
                const uint32_t order = peek8(e + 0x4Du);
                const uint32_t rows = table + peek16(e + 0x48u) * 64u;
                static constexpr uint32_t kRows[4][2] = {{0u, 3u}, {1u, 2u}, {2u, 1u}, {3u, 0u}};
                const uint32_t *r = kRows[order < 4u ? order : 0u];
                sprite.st[0] = stRow(rows + r[0] * 16u);
                sprite.st[1] = stRow(rows + r[1] * 16u);
                v6 = put(v6, ps2_vmul(v6, lane(vf[2], 0)), 3u);
                Vec v4 = v3;
                v3 = put(v3, ps2_vsub(v3, v6), 3u);
                v4 = put(v4, ps2_vadd(v4, v6), 3u);
                v3 = put(v3, ftoiVec(v3, 16.0f), 7u);
                v4 = put(v4, ftoiVec(v4, 16.0f), 7u);
                vf[3] = v3;
                vf[4] = v4;
                vf[6] = v6;
                vf[7] = v7;
                if (offRange(v3) || offRange(v4))
                    continue;
                sprite.xyz[0] = vecWords(v3);
                sprite.xyz[1] = vecWords(v4);
                sprite.depth = depth;
                sprite.inserted = true;
                call.sprites.push_back(sprite);
            }
            return true;
        }

        // A sprite's packet from its DMA tag slot: 0x1107E0's 12 registers (NOP or TEX1_2, NOP, ALPHA_2, RGBAQ, four ST /
        // XYZF2), after 0x10FE50's TEXA / TEST_2 packets.
        std::vector<Qword> screenHead(const ObjectCall &call)
        {
            std::vector<Qword> w;
            if (call.texa)
            {
                w.push_back({0x10000002u, kUnknownWord, kUnknownWord, kUnknownWord});
                w.push_back({1u, 0x10000000u, 0xEu, 0u});
                w.push_back({0x80u, 0x80u, 0x3Bu, 0u});
            }
            if (call.test)
            {
                w.push_back({0x10000002u, kUnknownWord, kUnknownWord, kUnknownWord});
                w.push_back({1u, 0x10000000u, 0xEu, 0u});
                w.push_back({0x30000u, 0u, 0x48u, 0u});
            }
            w.push_back({kUnknownWord, kUnknownWord, kUnknownWord, kUnknownWord});
            w.push_back({static_cast<uint32_t>(call.tag), static_cast<uint32_t>(call.tag >> 32), static_cast<uint32_t>(call.regs),
                         static_cast<uint32_t>(call.regs >> 32)});
            return w;
        }

        std::vector<Qword> screenObjectWords(const ObjectCall &call, const ObjectSprite &sprite)
        {
            std::vector<Qword> w = screenHead(call);
            w.push_back(call.tex1 ? Qword{sprite.tex1, 0u, 0x15u, 0u} : Qword{0u, 0u, 0u, 0u});
            w.push_back({0u, 0u, 0u, 0u});
            w.push_back({static_cast<uint32_t>(call.alpha), static_cast<uint32_t>(call.alpha >> 32), 0x43u, 0u});
            w.push_back(sprite.colour);
            for (int i = 0; i < 4; ++i)
            {
                w.push_back(sprite.st[i]);
                w.push_back(sprite.xyz[i]);
            }
            return w;
        }

        // 0x110258's: [TEX1_2,] ALPHA_2, RGBAQ, two ST / XYZF2; with TEST_2 a CNT of two after it puts TEST_2 back (0x5002D).
        std::vector<Qword> screenSpriteWords(const ObjectCall &call, const ObjectSprite &sprite)
        {
            std::vector<Qword> w = screenHead(call);
            if (call.tex1)
                w.push_back({sprite.tex1, 0u, 0x15u, 0u});
            w.push_back({static_cast<uint32_t>(call.alpha), static_cast<uint32_t>(call.alpha >> 32), 0x43u, 0u});
            w.push_back(sprite.colour);
            for (int i = 0; i < 2; ++i)
            {
                w.push_back(sprite.st[i]);
                w.push_back(sprite.xyz[i]);
            }
            if (call.test)
            {
                w.push_back({kUnknownWord, kUnknownWord, kUnknownWord, kUnknownWord});
                w.push_back({1u, 0x10000000u, 0xEu, 0u});
                w.push_back({0x5002Du, 0u, 0x48u, 0u});
            }
            return w;
        }

        // 0x112148's: [TEX1_2,] ALPHA_2, RGBAQ, two ST / XYZ2.
        std::vector<Qword> billboardWords(const ObjectCall &call, const ObjectSprite &sprite)
        {
            std::vector<Qword> w = screenHead(call);
            if (call.tex1)
                w.push_back({sprite.tex1, 0u, 0x15u, 0u});
            w.push_back({static_cast<uint32_t>(call.alpha), static_cast<uint32_t>(call.alpha >> 32), 0x43u, 0u});
            w.push_back(sprite.colour);
            for (int i = 0; i < 2; ++i)
            {
                w.push_back(sprite.st[i]);
                w.push_back(sprite.xyz[i]);
            }
            return w;
        }

        // The page an object-like item draws from (false: its draws stay the game's), as objectsDraw takes it.
        bool screenPage(uint32_t item, Page &pageRegs, BuilderStats &stats)
        {
            const uint32_t file = peek32(item + 0x14u);
            const uint32_t uploads = peek8(item + 0x1Au), head = peek8(item + 0x1Cu);
            if (head > 2u || !fileAddress(file) || (head != 0u && peek32(item + 0x10u) == 0u) || peek8(item + 0xAu) > 3u ||
                (uploads != 0u && head == 0u && (peek8(file) == 2u || peek8(file) == 3u)))
                return false;
            return head == 0u ? page(file, peek8(item + 0x18u), pageRegs, stats)
                   : head == 2u ? copiedPage(file, pageRegs, stats)
                                : blockPage(file, pageRegs, stats, ~0u) && pageRegs.hasAlpha;
        }

        using ScreenBuild = bool (*)(uint8_t *, R5900Context *, PS2Runtime *, uint32_t, Vu0 &, ObjectCall &, const Page &);

        bool screenSpritesBuildHook(uint8_t *, R5900Context *, PS2Runtime *, uint32_t item, Vu0 &vu, ObjectCall &call, const Page &pageRegs)
        {
            return screenSpritesBuild(item, vu, call, pageRegs);
        }

        bool billboardsBuildHook(uint8_t *, R5900Context *, PS2Runtime *, uint32_t item, Vu0 &vu, ObjectCall &call, const Page &pageRegs)
        {
            return billboardsBuild(item, vu, call, pageRegs);
        }

        void screenItemDraw(uint32_t address, ScreenBuild build, ObjectWords words, const char *name, BuilderStats &stats,
                            std::map<uint32_t, uint64_t> &forms, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            ++stats.calls;
            const uint32_t item = getRegU32(ctx, 4);
            if (s.mode == Mode::Off)
            {
                callOriginal(address, rdram, ctx, runtime);
                return;
            }
            const uint32_t uploads = peek8(item + 0x1Au), head = peek8(item + 0x1Cu);
            const bool sprites = address != kScreenObjects;
            ++forms[(uploads << 24) | (head << 16) | (peek8(item + 0x1Du) << 8) | peek8(item + 0x1Fu)];
            if (s.mode == Mode::Check)
            {
                Vu0 entry, vu;
                loadVu0(entry, ctx);
                vu = entry;
                ObjectCall call;
                Page pageRegs;
                BuilderStats unused;
                const bool sized = peek32(item + 0x10u) != 0u || peek32(item + 0x14u) == 0u ||
                                   (head == 0u && page(peek32(item + 0x14u), peek8(item + 0x18u), pageRegs, unused));
                const bool built = sized && (!sprites || peek8(item + 0xBu) == 0u) && build(rdram, ctx, runtime, item, vu, call, pageRegs);
                storeVu0(entry, ctx);
                s.stripCapture = true;
                s.stripPackets.clear();
                runOriginal(address, rdram, ctx, runtime);
                s.stripCapture = false;
                if (built && peek32(kRenderer) == 0u)
                    compareObjects(stats, call, head == 0u && uploads == 0u, s.stripPackets, words, name);
                return;
            }
            Page pageRegs;
            const uint64_t reasons = stats.noFile + stats.noBlock + stats.noTexture;
            if ((sprites && peek8(item + 0xBu) != 0u) || !screenPage(item, pageRegs, stats))
            {
                ++stats.toGs;
                if (stats.noFile + stats.noBlock + stats.noTexture == reasons && ++s.screenItemsGsLogged <= 8u)
                    std::fprintf(stderr, "[sc3:2d] %s to the GS: item %#x count %u insertion %u prim %#x file %#x page %u head %u uploads %u, block %#x\n", name, item,
                                 peek16(item + 8u), peek8(item + 0xAu), peek8(item + 0xBu), peek32(item + 0x14u), peek8(item + 0x18u), peek8(item + 0x1Cu),
                                 peek8(item + 0x1Au), peek32(item + 0x10u));
                callOriginal(address, rdram, ctx, runtime);
                return;
            }
            Vu0 vu;
            loadVu0(vu, ctx);
            ObjectCall call;
            const bool built = build(rdram, ctx, runtime, item, vu, call, pageRegs);
            if (call.staleSt)
            {
                ++stats.toGs;
                callOriginal(address, rdram, ctx, runtime);
                return;
            }
            if (!sprites && peek16(item + 8u) != 0u && peek32(item + 0xCu) != 0u)
                poke8(item + 0xBu, 2u);
            if (built)
                poke32(kStripInsertion, call.insertion);
            GsMaterial m = spriteMaterial(pageRegs, head == 1u ? pageRegs.alpha : call.alpha);
            m.prim = ps2x::gs::decodePrim(static_cast<uint32_t>(call.tag >> 47) & 0x7FFu);
            if (!sprites)
                m.prim.type = GS_PRIM_TRIANGLE;
            if (head == 1u ? pageRegs.hasTest : call.test)
            {
                m.test = head == 1u ? pageRegs.test : 0x30000u;
                m.testFromTarget = false;
            }
            for (const ObjectSprite &o : call.sprites)
            {
                if (!o.inserted)
                    continue;
                if (call.tex1)
                    m.tex1 = o.tex1;
                const uint64_t key = newKey();
                if (sprites)
                {
                    const GsVertex corners[2] = {stripVertex(o.st[0], o.colour, o.xyz[0]), stripVertex(o.st[1], o.colour, o.xyz[1])};
                    ps2x::native::Renderer::instance().drawGsKeyed(key, m, corners, 2u);
                }
                else
                {
                    GsVertex v[4];
                    for (int i = 0; i < 4; ++i)
                        v[i] = stripVertex(o.st[i], o.colour, o.xyz[i]);
                    const GsVertex triangles[6] = {v[0], v[1], v[2], v[1], v[2], v[3]};
                    ps2x::native::Renderer::instance().drawGsKeyed(key, m, triangles, 6u);
                }
                if (insert2d(key, call.mode, o.depth) != Linked::Yes)
                    discardKeyed(key);
                else
                {
                    stats.sprites += 1u;
                    ++stats.markers;
                }
            }
            storeVu0(vu, ctx);
            ++stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }

        void screenObjectsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            screenItemDraw(kScreenObjects, screenObjectsBuild, screenObjectWords, "screen objects", g_state.screenObjects,
                           g_state.screenObjectForms, rdram, ctx, runtime);
        }

        void screenSpritesDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            screenItemDraw(kScreenSprites, screenSpritesBuildHook, screenSpriteWords, "screen sprites", g_state.screenSprites,
                           g_state.screenSpriteForms, rdram, ctx, runtime);
        }

        void billboardsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            screenItemDraw(kBillboards, billboardsBuildHook, billboardWords, "billboards", g_state.billboards, g_state.billboardForms, rdram,
                           ctx, runtime);
        }

        void screenObjects(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trackItem(screenObjectsDraw, g_state.screenObjects, rdram, ctx, runtime);
        }

        void screenSprites(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trackItem(screenSpritesDraw, g_state.screenSprites, rdram, ctx, runtime);
        }

        void billboards(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { trackItem(billboardsDraw, g_state.billboards, rdram, ctx, runtime); }

        void insertion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t at = ctx->pc;
            const uint32_t base = peek32(kScratchpad);
            const uint32_t end = getRegU32(ctx, 4);
            if (s.capture && (at == kInsertText || at == kInsertLayer || at == kInsertList))
                s.captured.push_back(capturedPacket(end, getRegU32(ctx, 5), at, getRegU32(ctx, 7) & 0xFFu));
            if (s.stripCapture && std::find(std::begin(kStripInserts), std::end(kStripInserts), at) != std::end(kStripInserts))
            {
                std::vector<Qword> &words = s.stripPackets.emplace_back();
                for (uint32_t q = base; q < end && words.size() < 0x400u; q += 16u)
                    words.push_back({peek32(q), peek32(q + 4u), peek32(q + 8u), peek32(q + 12u)});
            }
            if (inGsItem(getRegU32(ctx, 31)))
                s.itemFilesGs[s.gsItemFile] = s.frames;
            if (std::find(std::begin(kInserts), std::end(kInserts), at) == std::end(kInserts))
            {
                const uint32_t outer = s.outerInsertRa;
                s.outerInsertRa = getRegU32(ctx, 31);
                ++s.insertDepth;
                callOriginal(at, rdram, ctx, runtime);
                --s.insertDepth;
                s.outerInsertRa = outer;
                return;
            }
            auto &counted = s.inserted[{s.outerInsertRa ? s.outerInsertRa : getRegU32(ctx, 31), at}];
            ++counted.first;
            if (end > base && end - base < 0x4000u)
                counted.second += (end - base) / 16u + 1u;
            ++s.insertDepth;
            callOriginal(at, rdram, ctx, runtime);
            --s.insertDepth;
        }

        // 0x10F808 (end): the scratch packet from its base to `end` appended to the in-order GIF list (0x10EF78) by its
        // builder itself, not through an insertion: counted as one when no insertion called it.
        void gifListAppend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            const uint32_t base = peek32(kScratchpad);
            const uint32_t end = getRegU32(ctx, 4);
            if (s.insertDepth == 0u)
            {
                if (s.stripCapture)
                {
                    std::vector<Qword> &words = s.stripPackets.emplace_back();
                    for (uint32_t q = base; q < end && words.size() < 0x400u; q += 16u)
                        words.push_back({peek32(q), peek32(q + 4u), peek32(q + 8u), peek32(q + 12u)});
                }
                if (inGsItem(getRegU32(ctx, 31)))
                    s.itemFilesGs[s.gsItemFile] = s.frames;
                auto &counted = s.inserted[{getRegU32(ctx, 31), kGifListAppend}];
                ++counted.first;
                if (end > base && end - base < 0x4000u)
                    counted.second += (end - base) / 16u;
            }
            callOriginal(kGifListAppend, rdram, ctx, runtime);
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
            reportBuilder("shaded lines 0x1220C0", s.shadedLines);
            reportBuilder("icons 0x1280F8", s.icons);
            reportBuilder("screen effects 0x12C690 / 0x12C7E0", s.effects);
            reportBuilder("strips 0x1134A0", s.strips);
            std::fprintf(stderr, "[sc3:2d]   page copies %zu recorded; head-2 strips checked %llu, %llu not as recorded\n", s.pageCopies.size(),
                         static_cast<unsigned long long>(s.pageCopyChecks), static_cast<unsigned long long>(s.pageCopyMisses));
            std::string forms;
            for (const auto &[form, n] : s.stripForms)
            {
                std::snprintf(item, sizeof(item), " %08x:%llu", form, static_cast<unsigned long long>(n));
                forms += item;
            }
            s.stripForms.clear();
            std::fprintf(stderr, "[sc3:2d]   strips by uploads / head / matrix / Z / insertion:%s\n", forms.c_str());
            reportBuilder("screen strips 0x114B50", s.screenStrips);
            forms.clear();
            for (const auto &[form, n] : s.screenStripForms)
            {
                std::snprintf(item, sizeof(item), " %08x:%llu", form, static_cast<unsigned long long>(n));
                forms += item;
            }
            s.screenStripForms.clear();
            std::fprintf(stderr, "[sc3:2d]   screen strips by uploads / head / matrix / Z / insertion:%s\n", forms.c_str());
            reportBuilder("objects 0x110E50", s.objects);
            forms.clear();
            for (const auto &[form, n] : s.objectForms)
            {
                std::snprintf(item, sizeof(item), " %08x:%llu", form, static_cast<unsigned long long>(n));
                forms += item;
            }
            s.objectForms.clear();
            std::fprintf(stderr, "[sc3:2d]   objects by uploads / head / flags / Z:%s\n", forms.c_str());
            for (auto [name, stats, byForm] : {std::tuple{"screen objects 0x1107E0", &s.screenObjects, &s.screenObjectForms},
                                               std::tuple{"screen sprites 0x110258", &s.screenSprites, &s.screenSpriteForms},
                                               std::tuple{"billboards 0x112148", &s.billboards, &s.billboardForms}})
            {
                reportBuilder(name, *stats);
                forms.clear();
                for (const auto &[form, n] : *byForm)
                {
                    std::snprintf(item, sizeof(item), " %08x:%llu", form, static_cast<unsigned long long>(n));
                    forms += item;
                }
                byForm->clear();
                std::fprintf(stderr, "[sc3:2d]   %s by uploads / head / flags / Z:%s\n", name, forms.c_str());
            }
            std::string kept;
            for (const auto &[key, n] : s.uploadsKept)
            {
                packs::LoadedImages::Image image;
                const bool known = s.loaded.find(key.first, image) && image.file;
                std::snprintf(item, sizeof(item), " %#x(%s%s):%llu", key.first, known ? image.file->name.c_str() : "?", key.second ? ", GS item" : "",
                              static_cast<unsigned long long>(n));
                kept += item;
            }
            std::fprintf(stderr, "[sc3:2d]   2D file uploads dropped: %llu, sent:%s\n", static_cast<unsigned long long>(s.uploadsDropped), kept.c_str());
            s.uploadsDropped = 0;
            s.uploadsKept.clear();
            std::fprintf(stderr, "[sc3:2d]   movies: %zu pages, picture copies skipped %llu, page textures given %llu; checked %llu (%llu texels differing)\n",
                         s.moviePages.size(), static_cast<unsigned long long>(s.movieCopiesSkipped), static_cast<unsigned long long>(s.movieTextures),
                         static_cast<unsigned long long>(s.movieChecks), static_cast<unsigned long long>(s.movieTexelsDiffering));
            s.movieCopiesSkipped = s.movieTextures = s.movieChecks = s.movieTexelsDiffering = 0;
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
        wrap(runtime, kShadedLine, shadedLine);
        wrap(runtime, kIcon, icon);
        wrap(runtime, kEffectLayer, screenEffectLayer);
        wrap(runtime, kEffectInline, screenEffectInline);
        wrap(runtime, kStrips, strips);
        wrap(runtime, kScreenStrips, screenStrips);
        for (const uint32_t address : {kPageCopy, kPageCopyClut, kPageCopyCbp})
            wrap(runtime, address, pageCopy);
        wrap(runtime, kObjects, objects);
        wrap(runtime, kScreenObjects, screenObjects);
        wrap(runtime, kScreenSprites, screenSprites);
        wrap(runtime, kBillboards, billboards);
        for (const uint32_t address : {kFontUpload, kLabelsUpload, kIconsUpload})
            wrap(runtime, address, upload);
        wrap(runtime, kUploadItem, uploadItem);
        wrap(runtime, kMovieCopy, movieCopy);
        wrap(runtime, kFilePixels, filePixels);
        for (const auto &item : kGsItems)
            wrap(runtime, item.first, gsItem);
        s.format = runtime.lookupFunction(kTextFormat);
        wrap(runtime, kFrameKick, frameKick);
        for (const uint32_t address : kInserts)
            wrap(runtime, address, insertion);
        for (const uint32_t address : kStripInserts)
            if (!g_state.originals.count(address))
                wrap(runtime, address, insertion);
        wrap(runtime, kGifListAppend, gifListAppend);
        ps2x::native::setGifChainMarkGate(markerGate);
        s.installed = true;
        std::fprintf(stderr, "[sc3:2d] native 2D%s: %s, %zu leaves with texture pages\n", s.switchPath.empty() ? "" : " (as its switch file says)",
                     path, s.pack.files().size());
    }

    void setMoviePictures(MoviePicture pictures) { native2d::g_state.moviePictures = pictures; }

    bool native2dMovie(uint32_t handle)
    {
        const native2d::State &s = native2d::g_state;
        if (!s.installed || s.mode != native2d::Mode::On || !s.moviePictures)
            return false;
        const auto it = s.movies.find(handle);
        return it != s.movies.end() && it->second.copied != 0u && s.frames - it->second.copied <= 60u;
    }
}
