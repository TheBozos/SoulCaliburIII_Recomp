#include "sc3_textures.h"
#include "sc3_census.h"
#include "sc3_texture_pack.h"

#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/native/native_gs_hook.h"
#include "runtime/vif1_command_stats.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sc3
{
    namespace texpack
    {
    namespace
    {
        constexpr uint32_t kFrameKick = 0x0010F098u;
        constexpr uint32_t kFileRequest = 0x00150418u;
        constexpr uint32_t kCdRead = 0x00424E28u;
        constexpr uint32_t kCopy = 0x0012FE98u;
        constexpr uint32_t kModelCopyMin = 0x1000u;
        constexpr uint32_t kGifPacketClose = 0x0010F808u;
        constexpr uint32_t kOtInsertDepth = 0x0010D9A0u;
        constexpr uint32_t kSyncPoint = 0x0010F9D0u;
        constexpr uint64_t kSignal = 0x60u;
        constexpr uint32_t kGifSyncId = 1u;
        constexpr uint32_t kVif1SyncId = 2u;
        constexpr uint32_t kTextureCall = 0x00104DCCu;
        constexpr uint32_t kAnimatedTextureCall = 0x00104E48u;
        constexpr uint32_t kTextureCallOt = 0x00104E94u;
        constexpr uint32_t kAnimatedTextureCallOt = 0x00104E34u;
        constexpr uint32_t kScratchpad = 0x70000000u;
        constexpr uint32_t kPacketState = 0x005BE700u;
        constexpr uint32_t kCallTag = 0x50000000u;
        constexpr uint32_t kEmptyCntTag = 0x10000000u;
        constexpr uint32_t kSector = 2048u;

        using Function = PS2Runtime::RecompiledFunction;

        // One upload group: the CALL list entry a texture CALL started from (null: not in the pack).
        struct Group
        {
            const TextureKeys::Entry *entry = nullptr;
            uint32_t syncs = 0;  // GIF sync points before its CALL: its segment of the GIF / VIF1 handshake
            uint32_t target = 0; // the CALL's target
            uint32_t tag = 0;    // where 0x10EF78 / 0x10D9A0 copy the CALL tag (the GIF list, an ordering table), 0 unknown
            bool gsUse = false;  // a part left to VU1 may sample what it uploads
            TextureKeys::GameData game; // where the game holds the CALL list's leaf
            bool nativeUse = false; // a native part sampled one of its textures
        };

        struct Read
        {
            uint32_t end = 0;
            uint64_t olk = 0;
        };

        struct Stats
        {
            uint64_t calls = 0, packed = 0, leftOut = 0, unknown = 0, gsUse = 0, notNative = 0, misplaced = 0, incomplete = 0, untagged = 0;
            std::atomic<uint64_t> bound{0}, missed{0};
        };

        enum class PackMode
        {
            Off,
            Keep, // pack textures drawn, the game's uploads still sent (PS2X_NATIVE_TEXTURE_CHECK compares them)
            On,   // and the uploads of the groups the pack covers left out
        };

        std::atomic<PackMode> g_mode{PackMode::Off};
        std::string g_switchPath;
        std::string g_dumpDir;
        TextureKeys g_pack;
        uint8_t *g_rdram = nullptr;
        uint8_t *g_scratchpad = nullptr;
        PS2Runtime *g_runtime = nullptr;
        std::map<uint32_t, Function> g_originals;
        int64_t g_olkLbn = -1;
        uint32_t g_requestDest = 0, g_requestPos = 0;
        bool g_requestPending = false;
        std::map<uint32_t, Read> g_reads; // EE start -> what ROOT.OLK bytes the game read there
        std::vector<Group> g_building;    // the frame being built
        std::mutex g_framesMutex;
        std::deque<std::vector<Group>> g_kicked; // frames kicked, oldest first, for the VIF1 thread
        Stats g_stats;
        uint64_t g_frames = 0;
        uint64_t g_lastTransfers = 0, g_lastBytes = 0;
        uint64_t g_lastVif1[0x80] = {};
        uint64_t g_lastStarts = 0;
        std::map<uint32_t, uint64_t> g_unknownCalls; // texture CALL targets not in the pack, this window
        bool g_gsMiss = false; // the frame being built: a part left to VU1 sampled a texture no group has in the pack

        // VIF1 thread.
        std::vector<Group> g_drawing;
        uint32_t g_syncs = 0;        // the frame being built
        uint32_t g_streamSyncs = 0;  // VIF1 thread: the drawn frame's sync points passed
        bool g_wantFrame = true;
        std::map<uint64_t, uint64_t> g_misses; // TEX0 (TBP, PSM, CBP) -> draws without a pack texture
        std::mutex g_missMutex;

        uint32_t peek32(uint32_t address)
        {
            uint32_t value = 0;
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(&value, g_scratchpad + (address & 0x3FFCu), 4);
            else
                std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK & ~3u), 4);
            return value;
        }

        void poke32(uint32_t address, uint32_t value)
        {
            if ((address & 0xF0000000u) == kScratchpad)
                std::memcpy(g_scratchpad + (address & 0x3FFCu), &value, 4);
            else
                std::memcpy(g_rdram + (address & PS2_RAM_MASK & ~3u), &value, 4);
        }

        // The pack entry a texture CALL's target is: the ROOT.OLK bytes the game read where it sits, in a packed leaf.
        // `game` gets where the game holds that leaf: the reads around it that continue it in RDRAM and in ROOT.OLK.
        const TextureKeys::Entry *entryAt(uint32_t address, TextureKeys::GameData *game = nullptr)
        {
            auto read = g_reads.upper_bound(address);
            if (read == g_reads.begin())
                return nullptr;
            --read;
            if (address >= read->second.end)
                return nullptr;
            const uint64_t olk = read->second.olk + (address - read->first);
            uint64_t leaf = 0;
            const TextureKeys::Entry *entry = g_pack.entryAt(olk, &leaf);
            if (entry && game)
            {
                game->rdram = g_rdram;
                game->leaf = address - static_cast<uint32_t>(olk - leaf);
                game->lo = read->first;
                game->hi = read->second.end;
                game->frame = g_frames;
                for (auto before = read; before != g_reads.begin();)
                {
                    const auto prev = std::prev(before);
                    if (prev->second.end != before->first || prev->second.olk + (before->first - prev->first) != before->second.olk)
                        break;
                    game->lo = prev->first;
                    before = prev;
                }
                for (auto after = std::next(read), last = read; after != g_reads.end(); last = after++)
                {
                    if (last->second.end != after->first || last->second.olk + (after->first - last->first) != after->second.olk)
                        break;
                    game->hi = after->second.end;
                }
            }
            return entry;
        }

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_originals.at(address)(rdram, ctx, runtime);
        }

        void noteRead(uint32_t buffer, uint32_t end, uint64_t olk)
        {
            auto it = g_reads.lower_bound(buffer);
            if (it != g_reads.begin() && std::prev(it)->second.end > buffer)
                --it;
            while (it != g_reads.end() && it->first < end)
                it = g_reads.erase(it);
            g_reads[buffer] = Read{end, olk};
        }

        void fileRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 5) == 1u && g_olkLbn < 0)
            {
                g_requestDest = getRegU32(ctx, 8);
                g_requestPos = getRegU32(ctx, 6);
                g_requestPending = true;
            }
            callOriginal(kFileRequest, rdram, ctx, runtime);
        }

        void cdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t lbn = getRegU32(ctx, 4), sectors = getRegU32(ctx, 5), buffer = getRegU32(ctx, 6) & PS2_RAM_MASK;
            if (g_olkLbn < 0 && g_requestPending && buffer == (g_requestDest & PS2_RAM_MASK) && (g_requestPos % kSector) == 0u)
                g_olkLbn = static_cast<int64_t>(lbn) - g_requestPos / kSector;
            if (g_olkLbn >= 0 && lbn >= g_olkLbn && sectors != 0u)
                noteRead(buffer, buffer + sectors * kSector, static_cast<uint64_t>(lbn - g_olkLbn) * kSector);
            callOriginal(kCdRead, rdram, ctx, runtime);
        }

        // memcpy: a model the game copies out of what it read (character select's) keeps its ROOT.OLK bytes.
        void copy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t to = getRegU32(ctx, 4) & PS2_RAM_MASK, from = getRegU32(ctx, 5) & PS2_RAM_MASK, size = getRegU32(ctx, 6);
            if (size >= kModelCopyMin && size < 0x2000000u)
            {
                auto read = g_reads.upper_bound(from);
                if (read != g_reads.begin() && from + size <= (--read)->second.end)
                    noteRead(to, to + size, read->second.olk + (from - read->first));
            }
            callOriginal(kCopy, rdram, ctx, runtime);
        }

        // The texture CALL's target, from the game's own command list (0x104AF0's commands 2, 3 and 20).
        uint32_t callTarget(R5900Context *ctx, uint32_t ra)
        {
            const uint32_t command = getRegU32(ctx, 20);
            if (ra == kTextureCall)
                return peek32(command - 8u);
            if (ra == kTextureCallOt)
                return peek32(command - 4u);
            const uint32_t table = peek32(command + 4u);
            float frame;
            const uint32_t bits = peek32(peek32(command + 8u));
            std::memcpy(&frame, &bits, 4);
            return peek32(table + static_cast<uint32_t>(ps2FpuCvtWS(frame)) * 4u);
        }

        const TextureKeys::Entry *noteCall(R5900Context *ctx, uint32_t ra, uint32_t tag)
        {
            const uint32_t target = callTarget(ctx, ra);
            TextureKeys::GameData game;
            const TextureKeys::Entry *entry = entryAt(target, &game);
            if (censusTracingDraws())
                std::fprintf(stderr, "[sc3:trace] pack group %zu after %u syncs: call %#x ra=%#x %s\n", g_building.size(), g_syncs, target, ra,
                             entry ? "in the pack" : "not in the pack");
            g_building.push_back(Group{entry, g_syncs, target, tag, false, game});
            ++g_stats.calls;
            if (entry)
                ++g_stats.packed;
            else
            {
                ++g_stats.unknown;
                ++g_unknownCalls[target];
            }
            return entry;
        }

        // The texture CALL is the packet's only qword: 0x10EF78 copies it to the GIF list's cursor.
        void gifPacketClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (ra == kTextureCall || ra == kAnimatedTextureCall)
            {
                const bool alone = getRegU32(ctx, 4) == peek32(kScratchpad) + 16u && peek32(kPacketState) == 0u;
                noteCall(ctx, ra, alone ? peek32(kPacketState + 8u) : 0u);
            }
            callOriginal(kGifPacketClose, rdram, ctx, runtime);
        }

        // The texture CALL heads a two-qword packet at depth 0: 0x10D9A0 copies it below the ordering table's last
        // element, 128-byte aligned.
        void otInsert(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (ra == kTextureCallOt || ra == kAnimatedTextureCallOt)
            {
                const uint32_t cursor = peek32(kScratchpad), end = getRegU32(ctx, 4);
                const bool placed = peek32(kPacketState) == 0u && ctx->f[12] == 0.0f && getRegU32(ctx, 5) == cursor + 16u && end > cursor;
                noteCall(ctx, ra, placed ? (peek32(kPacketState + 0x30u) - ((end - cursor) & ~15u)) & ~0x7Fu : 0u);
            }
            callOriginal(kOtInsertDepth, rdram, ctx, runtime);
        }

        // At the kick, before the game sends the frame: the CALL of each group a native part sampled and no part left to VU1
        // may sample becomes an empty CNT, so its uploads are not sent. A group no native part sampled is sent: the game's
        // own GS draws may sample it (the fight intro's light rays, a PSMT8H texture in the Z buffer's top bytes). A group
        // whose uploads also feed textures the pack lacks (partly written by other uploads) is still sent when a part left to
        // VU1 sampled a texture no group of the frame has in the pack.
        void leaveOutUploads()
        {
            for (const Group &g : g_building)
            {
                if (!g.entry || g.gsUse)
                    continue;
                if (!g.nativeUse)
                {
                    ++g_stats.notNative;
                    continue;
                }
                if ((!g.entry->complete && g_gsMiss) || g.tag == 0u)
                {
                    ++(g.tag == 0u ? g_stats.untagged : g_stats.incomplete);
                    continue;
                }
                const uint32_t word = peek32(g.tag);
                if ((word & 0x7000FFFFu) != kCallTag || peek32(g.tag + 4u) != g.target)
                {
                    ++g_stats.misplaced;
                    continue;
                }
                poke32(g.tag, (word & ~0x70000000u) | kEmptyCntTag);
                ++g_stats.leftOut;
            }
        }

        void report()
        {
            if (++g_frames % 250u != 0u)
                return;
            GS &gs = g_runtime->gs();
            const uint64_t transfers = gs.imageTransfers(), bytes = gs.imageBytes();
            std::fprintf(stderr,
                         "[sc3:packets] %llu frames: texture uploads called %llu, from the pack %llu (left out %llu, sent %llu: a part "
                         "left to VU1 may sample them %llu, no native part sampled them %llu, CALL tag not where expected %llu, feeding textures the pack lacks one of those may sample %llu, CALL tag "
                         "place unknown %llu), not in the pack %llu; GS image transfers "
                         "read from GIF packets %llu (%llu bytes)\n",
                         250ull, static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.packed),
                         static_cast<unsigned long long>(g_stats.leftOut),
                         static_cast<unsigned long long>(g_stats.packed - g_stats.leftOut), static_cast<unsigned long long>(g_stats.gsUse),
                         static_cast<unsigned long long>(g_stats.notNative),
                         static_cast<unsigned long long>(g_stats.misplaced), static_cast<unsigned long long>(g_stats.incomplete),
                         static_cast<unsigned long long>(g_stats.untagged),
                         static_cast<unsigned long long>(g_stats.unknown), static_cast<unsigned long long>(transfers - g_lastTransfers),
                         static_cast<unsigned long long>(bytes - g_lastBytes));
            uint64_t vif1[0x80];
            for (uint32_t op = 0; op < 0x80u; ++op)
                vif1[op] = ps2_vif1_command_stats::bytes(static_cast<uint8_t>(op));
            const auto since = [&](uint32_t op) { return static_cast<unsigned long long>(vif1[op] - g_lastVif1[op]); };
            uint64_t other = 0;
            for (uint32_t op = 0; op < 0x80u; ++op)
                if (op != 0x07u && op != 0x4Au && op != 0x50u && op != 0x51u && op != 0x60u)
                    other += vif1[op] - g_lastVif1[op];
            const uint64_t starts = ps2_vif1_command_stats::commands(0x14u) + ps2_vif1_command_stats::commands(0x15u) + ps2_vif1_command_stats::commands(0x17u);
            std::fprintf(stderr,
                         "[sc3:packets] %llu frames: VIF1 bytes read: UNPACK %llu, MPG %llu, DIRECT %llu, MARK %llu, other codes %llu; "
                         "microprograms started %llu\n",
                         250ull, since(0x60u), since(0x4Au), since(0x50u) + since(0x51u), since(0x07u), static_cast<unsigned long long>(other),
                         static_cast<unsigned long long>(starts - g_lastStarts));
            std::memcpy(g_lastVif1, vif1, sizeof(vif1));
            if (!g_unknownCalls.empty())
            {
                std::vector<std::pair<uint32_t, uint64_t>> sorted(g_unknownCalls.begin(), g_unknownCalls.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                std::string calls;
                char item[96];
                for (size_t i = 0; i < sorted.size() && i < 6u; ++i)
                {
                    auto read = g_reads.upper_bound(sorted[i].first);
                    const bool known = read != g_reads.begin() && sorted[i].first < std::prev(read)->second.end;
                    if (known)
                        --read;
                    std::snprintf(item, sizeof(item), " %#x (ROOT.OLK %s%#llx) x%llu", sorted[i].first, known ? "+" : "unread ",
                                  static_cast<unsigned long long>(known ? read->second.olk + (sorted[i].first - read->first) : 0u),
                                  static_cast<unsigned long long>(sorted[i].second));
                    calls += item;
                }
                std::fprintf(stderr, "[sc3:packets] texture CALLs not in the pack:%s\n", calls.c_str());
                g_unknownCalls.clear();
            }
            g_lastStarts = starts;
            std::string misses;
            {
                std::lock_guard<std::mutex> lock(g_missMutex);
                std::vector<std::pair<uint64_t, uint64_t>> sorted(g_misses.begin(), g_misses.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                char item[96];
                for (size_t i = 0; i < sorted.size() && i < 8u; ++i)
                {
                    const uint64_t t = sorted[i].first;
                    std::snprintf(item, sizeof(item), " tbp %#x psm %#x cbp %#x x%llu", static_cast<unsigned>(t & 0x3FFFu),
                                  static_cast<unsigned>((t >> 20) & 0x3Fu), static_cast<unsigned>((t >> 37) & 0x3FFFu),
                                  static_cast<unsigned long long>(sorted[i].second));
                    misses += item;
                }
                g_misses.clear();
            }
            std::fprintf(stderr, "[sc3:gs] %llu frames: direct draws textured from the pack %llu, from GS memory %llu;%s\n", 250ull,
                         static_cast<unsigned long long>(g_stats.bound.exchange(0)), static_cast<unsigned long long>(g_stats.missed.exchange(0)),
                         misses.empty() ? " no misses" : misses.c_str());
            const TextureKeys::LiveStats live = g_pack.takeLiveStats();
            std::fprintf(stderr,
                         "[sc3:gs] %llu frames: pack textures checked against the game's upload data %llu (%llu bytes): palette changed "
                         "(drawn in the game's colours) %llu, texels changed %llu, data partly outside what the game read %llu\n",
                         250ull, static_cast<unsigned long long>(live.checked), static_cast<unsigned long long>(live.bytes),
                         static_cast<unsigned long long>(live.palettes), static_cast<unsigned long long>(live.texels),
                         static_cast<unsigned long long>(live.outside));
            g_lastTransfers = transfers;
            g_lastBytes = bytes;
            g_stats.calls = g_stats.packed = g_stats.leftOut = g_stats.unknown = g_stats.gsUse = g_stats.notNative = g_stats.misplaced = g_stats.incomplete = g_stats.untagged = 0;
        }

        PackMode parseMode(const std::string &value)
        {
            if (value == "keep")
                return PackMode::Keep;
            return value.empty() || value == "0" ? PackMode::Off : PackMode::On;
        }

        void followSwitch()
        {
            std::ifstream file(g_switchPath);
            std::string value;
            if (!std::getline(file, value))
                return;
            const PackMode mode = parseMode(value);
            if (mode != g_mode)
                std::fprintf(stderr, "[sc3] texture pack: %s from the next frame\n",
                             mode == PackMode::Off ? "off" : mode == PackMode::Keep ? "on, uploads still sent" : "on");
            g_mode = mode;
        }

        // SC3_TEXTURE_DUMP=<dir> (diagnosis): when <dir>/request appears, RDRAM, the ROOT.OLK read map and the frame's
        // upload groups go to <dir>/rdram.bin, reads.txt and groups.txt.
        void dumpOnRequest()
        {
            const std::string request = g_dumpDir + "/request";
            if (!std::ifstream(request))
                return;
            std::remove(request.c_str());
            std::ofstream(g_dumpDir + "/rdram.bin", std::ios::binary).write(reinterpret_cast<const char *>(g_rdram), PS2_RAM_SIZE);
            std::ofstream reads(g_dumpDir + "/reads.txt");
            for (const auto &[start, read] : g_reads)
                reads << std::hex << start << ' ' << read.end << ' ' << read.olk << '\n';
            std::ofstream groups(g_dumpDir + "/groups.txt");
            for (const Group &g : g_building)
                groups << std::hex << g.target << ' ' << (g.entry ? g.entry->list->name : std::string("-")) << ' ' << std::dec
                       << (g.entry ? g.entry->index : 0u) << '\n';
            std::fprintf(stderr, "[sc3] texture pack: dumped RDRAM, %zu reads, %zu groups to %s\n", g_reads.size(), g_building.size(), g_dumpDir.c_str());
        }

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_switchPath.empty() && (g_frames & 15u) == 0u)
                followSwitch();
            if (!g_dumpDir.empty() && (g_frames & 15u) == 0u)
                dumpOnRequest();
            if (g_mode == PackMode::On)
                leaveOutUploads();
            g_gsMiss = false;
            for (const Group &g : g_building)
                if (g.gsUse && g.entry && g.entry->complete)
                    ++g_stats.gsUse;
            {
                std::lock_guard<std::mutex> lock(g_framesMutex);
                g_kicked.push_back(std::move(g_building));
                g_syncs = 0;
                while (g_kicked.size() > 4u)
                    g_kicked.pop_front();
            }
            g_building.clear();
            report();
            callOriginal(kFrameKick, rdram, ctx, runtime);
        }

        void wrap(PS2Runtime &runtime, uint32_t address, Function hook)
        {
            Function original = runtime.lookupFunction(address);
            if (!original)
                return;
            g_originals[address] = original;
            runtime.registerFunction(address, hook);
        }

        // As the GS would sample it: from the latest group that covers it among those whose GIF segment (the GIF sync
        // points before its CALL) VIF1 has reached (as many of its own sync points passed).
        const ps2x::native::NativeTexture *find(uint64_t tex0, uint32_t width, uint32_t height)
        {
            for (size_t g = g_drawing.size(); g-- > 0;)
                if (g_drawing[g].syncs <= g_streamSyncs)
                    if (const TextureKeys::Entry *entry = g_drawing[g].entry)
                        if (const ps2x::native::NativeTexture *texture = g_pack.texture(*entry, tex0, width, height, &g_drawing[g].game))
                            return texture;
            return nullptr;
        }

        void syncPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 5) == kGifSyncId)
                ++g_syncs;
            callOriginal(kSyncPoint, rdram, ctx, runtime);
        }
    }
    }

    const ps2x::native::NativeTexture *packTexture(uint64_t tex0, uint32_t width, uint32_t height)
    {
        if (texpack::g_mode == texpack::PackMode::Off)
            return nullptr;
        if (texpack::g_wantFrame)
        {
            std::lock_guard<std::mutex> lock(texpack::g_framesMutex);
            if (!texpack::g_kicked.empty())
            {
                texpack::g_drawing = std::move(texpack::g_kicked.front());
                texpack::g_kicked.pop_front();
                texpack::g_wantFrame = false;
            }
        }
        const ps2x::native::NativeTexture *texture = texpack::find(tex0, width, height);
        if (censusTracingDraws())
            std::fprintf(stderr, "[sc3:trace] pack tex0=%#llx %ux%u after %zu syncs, %zu groups: %s\n", static_cast<unsigned long long>(tex0), width,
                         height, static_cast<size_t>(texpack::g_streamSyncs), texpack::g_drawing.size(), texture ? "pack" : "GS memory");
        if (texture)
        {
            texpack::g_stats.bound.fetch_add(1u, std::memory_order_relaxed);
            return texture;
        }
        texpack::g_stats.missed.fetch_add(1u, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(texpack::g_missMutex);
        ++texpack::g_misses[tex0 & ((0x3FFFull) | (0x3Full << 20) | (0x3FFFull << 37))];
        return nullptr;
    }

    bool texturePackReady() { return texpack::g_runtime != nullptr; }

    void packNoteGsUse(uint64_t tex0, uint32_t width, uint32_t height)
    {
        for (size_t g = texpack::g_building.size(); g-- > 0;)
            if (const TextureKeys::Entry *entry = texpack::g_building[g].entry)
                if (texpack::g_pack.texture(*entry, tex0, width, height))
                {
                    texpack::g_building[g].gsUse = true;
                    return;
                }
        texpack::g_gsMiss = true;
    }

    void packNoteGsUseAny()
    {
        for (texpack::Group &g : texpack::g_building)
            g.gsUse = true;
        texpack::g_gsMiss = true;
    }

    const ps2x::native::NativeTexture *packTextureAtHook(uint64_t tex0, uint32_t width, uint32_t height, uint32_t part)
    {
        for (const bool ownOnly : {true, false})
        {
            if (ownOnly && part == 0u)
                continue;
            for (size_t g = texpack::g_building.size(); g-- > 0;)
            {
                texpack::Group &group = texpack::g_building[g];
                if (!group.entry || (ownOnly && (part < group.game.lo || part >= group.game.hi)))
                    continue;
                if (const ps2x::native::NativeTexture *texture = texpack::g_pack.texture(*group.entry, tex0, width, height, &group.game))
                {
                    group.nativeUse = true;
                    if (censusTracingDraws())
                        std::fprintf(stderr, "[sc3:trace] hook tex0=%#llx %ux%u part %#x: group %zu/%zu %s entry %u leaf at %#x%s, %s\n",
                                     static_cast<unsigned long long>(tex0), width, height, part, g, texpack::g_building.size(),
                                     group.entry->list->name.c_str(), group.entry->index, group.game.leaf, ownOnly ? " (the part's)" : "",
                                     (texture->key >> 63) ? "game palette" : "pack");
                    return texture;
                }
            }
        }
        return nullptr;
    }

    void packNoteDirect(const uint32_t *words, size_t qwords)
    {
        if (!texpack::g_runtime)
            return;
        size_t q = 0;
        while (q < qwords)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, words + q * 4u, 8);
            std::memcpy(&hi, words + q * 4u + 2u, 8);
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu), format = static_cast<uint32_t>((lo >> 58) & 3u);
            const uint32_t nreg = ((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
            ++q;
            if (format == 0u && nreg == 1u && (hi & 0xFu) == 0xEu)
            {
                for (uint32_t i = 0; i < nloop && q < qwords; ++i, ++q)
                {
                    uint64_t value, reg;
                    std::memcpy(&value, words + q * 4u, 8);
                    std::memcpy(&reg, words + q * 4u + 2u, 8);
                    if ((reg & 0xFFu) == texpack::kSignal && (value & texpack::kVif1SyncId))
                        ++texpack::g_streamSyncs;
                }
            }
            else
                q += format == 0u ? size_t{nloop} * nreg : format == 1u ? (size_t{nloop} * nreg + 1u) / 2u : nloop;
            if ((lo >> 15) & 1u)
                return;
        }
    }

    void packFrameEnd()
    {
        if (!texpack::g_runtime)
            return;
        if (texpack::g_wantFrame)
        {
            std::lock_guard<std::mutex> lock(texpack::g_framesMutex);
            if (!texpack::g_kicked.empty())
                texpack::g_kicked.pop_front();
        }
        texpack::g_drawing.clear();
        texpack::g_streamSyncs = 0;
        texpack::g_wantFrame = true;
    }

    void installTexturePack(PS2Runtime &runtime, bool nativeGs)
    {
        const char *path = std::getenv("SC3_TEXTURE_PACK");
        const char *mode = std::getenv("SC3_TEXTURE_PACK_MODE");
        if (!path || !*path || !mode || !*mode || *mode == '0')
            return;
        const bool followed = mode && *mode == '@';
        if (!nativeGs)
        {
            std::fprintf(stderr, "[sc3] texture pack: only with the native GS renderer, not used\n");
            return;
        }
        if (!texpack::g_pack.open(path))
            return;
        std::fprintf(stderr, "[sc3] texture pack: %s, %zu upload lists, %zu entries, %zu records\n", path, texpack::g_pack.lists(),
                     texpack::g_pack.entries(), texpack::g_pack.records());
        if (followed)
        {
            texpack::g_switchPath = mode + 1;
            texpack::followSwitch();
        }
        else
            texpack::g_mode = std::strcmp(mode, "keep") == 0 ? texpack::PackMode::Keep : texpack::PackMode::On;
        if (const char *dump = std::getenv("SC3_TEXTURE_DUMP"); dump && *dump)
            texpack::g_dumpDir = dump;
        texpack::g_runtime = &runtime;
        texpack::g_rdram = runtime.memory().getRDRAM();
        texpack::g_scratchpad = runtime.memory().getScratchpad();
        texpack::wrap(runtime, texpack::kFileRequest, texpack::fileRequest);
        texpack::wrap(runtime, texpack::kCdRead, texpack::cdRead);
        texpack::wrap(runtime, texpack::kCopy, texpack::copy);
        texpack::wrap(runtime, texpack::kGifPacketClose, texpack::gifPacketClose);
        texpack::wrap(runtime, texpack::kOtInsertDepth, texpack::otInsert);
        texpack::wrap(runtime, texpack::kFrameKick, texpack::frameKick);
        texpack::wrap(runtime, texpack::kSyncPoint, texpack::syncPoint);
        std::fprintf(stderr, "[sc3] texture pack: direct draws textured from it%s\n",
                     followed ? ", as its switch file says" : texpack::g_mode == texpack::PackMode::On ? ", uploads it covers left out" : ", the game's uploads still sent");
    }
}
