#include "sc3_textures.h"
#include "sc3_census.h"
#include "sc3_texture_pack.h"

#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/native/native_gs_hook.h"

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
        constexpr uint32_t kSector = 2048u;

        using Function = PS2Runtime::RecompiledFunction;

        // One upload group: the CALL list entry a texture CALL started from (null: not in the pack).
        struct Group
        {
            const TextureKeys::Entry *entry = nullptr;
            uint32_t syncs = 0; // GIF sync points before its CALL: its segment of the GIF / VIF1 handshake
        };

        struct Read
        {
            uint32_t end = 0;
            uint64_t olk = 0;
        };

        struct Stats
        {
            uint64_t calls = 0, packed = 0, leftOut = 0, unknown = 0;
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
        const TextureKeys::Entry *entryAt(uint32_t address)
        {
            auto read = g_reads.upper_bound(address);
            if (read == g_reads.begin())
                return nullptr;
            --read;
            if (address >= read->second.end)
                return nullptr;
            return g_pack.entryAt(read->second.olk + (address - read->first));
        }

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_originals.at(address)(rdram, ctx, runtime);
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
            {
                const uint32_t end = buffer + sectors * kSector;
                auto it = g_reads.lower_bound(buffer);
                if (it != g_reads.begin() && std::prev(it)->second.end > buffer)
                    --it;
                while (it != g_reads.end() && it->first < end)
                    it = g_reads.erase(it);
                g_reads[buffer] = Read{end, static_cast<uint64_t>(lbn - g_olkLbn) * kSector};
            }
            callOriginal(kCdRead, rdram, ctx, runtime);
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

        const TextureKeys::Entry *noteCall(R5900Context *ctx, uint32_t ra)
        {
            const uint32_t target = callTarget(ctx, ra);
            const TextureKeys::Entry *entry = entryAt(target);
            if (censusTracingDraws())
                std::fprintf(stderr, "[sc3:trace] pack group %zu after %u syncs: call %#x ra=%#x %s\n", g_building.size(), g_syncs, target, ra,
                             entry ? "in the pack" : "not in the pack");
            g_building.push_back(Group{entry, g_syncs});
            ++g_stats.calls;
            if (entry)
                ++g_stats.packed;
            else
                ++g_stats.unknown;
            return entry;
        }

        // 0x10F808 without its copy to the GIF list (0x10EF78): the scratchpad half flips as it would.
        void closeWithoutUpload(R5900Context *ctx)
        {
            const uint32_t half = peek32(kScratchpad + 4u) ^ 1u;
            poke32(kScratchpad + 4u, half);
            poke32(kScratchpad, peek32(kScratchpad + 8u + half * 4u));
            ctx->pc = getRegU32(ctx, 31);
        }

        void gifPacketClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (ra == kTextureCall || ra == kAnimatedTextureCall)
            {
                const TextureKeys::Entry *entry = noteCall(ctx, ra);
                if (g_mode == PackMode::On && entry && entry->complete && getRegU32(ctx, 4) == peek32(kScratchpad) + 16u)
                {
                    ++g_stats.leftOut;
                    closeWithoutUpload(ctx);
                    return;
                }
            }
            callOriginal(kGifPacketClose, rdram, ctx, runtime);
        }

        void otInsert(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (ra == kTextureCallOt || ra == kAnimatedTextureCallOt)
                noteCall(ctx, ra);
            callOriginal(kOtInsertDepth, rdram, ctx, runtime);
        }

        void report()
        {
            if (++g_frames % 250u != 0u)
                return;
            GS &gs = g_runtime->gs();
            const uint64_t transfers = gs.imageTransfers(), bytes = gs.imageBytes();
            std::fprintf(stderr,
                         "[sc3:packets] %llu frames: texture uploads called %llu, from the pack %llu (left out %llu, sent %llu), not "
                         "in the pack %llu; GS image transfers read from GIF packets %llu (%llu bytes)\n",
                         250ull, static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.packed),
                         static_cast<unsigned long long>(g_stats.leftOut),
                         static_cast<unsigned long long>(g_stats.packed - g_stats.leftOut),
                         static_cast<unsigned long long>(g_stats.unknown), static_cast<unsigned long long>(transfers - g_lastTransfers),
                         static_cast<unsigned long long>(bytes - g_lastBytes));
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
            g_lastTransfers = transfers;
            g_lastBytes = bytes;
            g_stats.calls = g_stats.packed = g_stats.leftOut = g_stats.unknown = 0;
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

        void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_switchPath.empty() && (g_frames & 15u) == 0u)
                followSwitch();
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
                        if (const ps2x::native::NativeTexture *texture = g_pack.texture(*entry, tex0, width, height))
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

    const ps2x::native::NativeTexture *packTextureAtHook(uint64_t tex0, uint32_t width, uint32_t height)
    {
        for (size_t g = texpack::g_building.size(); g-- > 0;)
            if (const TextureKeys::Entry *entry = texpack::g_building[g].entry)
                if (const ps2x::native::NativeTexture *texture = texpack::g_pack.texture(*entry, tex0, width, height))
                    return texture;
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
        texpack::g_runtime = &runtime;
        texpack::g_rdram = runtime.memory().getRDRAM();
        texpack::g_scratchpad = runtime.memory().getScratchpad();
        texpack::wrap(runtime, texpack::kFileRequest, texpack::fileRequest);
        texpack::wrap(runtime, texpack::kCdRead, texpack::cdRead);
        texpack::wrap(runtime, texpack::kGifPacketClose, texpack::gifPacketClose);
        texpack::wrap(runtime, texpack::kOtInsertDepth, texpack::otInsert);
        texpack::wrap(runtime, texpack::kFrameKick, texpack::frameKick);
        texpack::wrap(runtime, texpack::kSyncPoint, texpack::syncPoint);
        std::fprintf(stderr, "[sc3] texture pack: direct draws textured from it%s\n",
                     followed ? ", as its switch file says" : texpack::g_mode == texpack::PackMode::On ? ", uploads it covers left out" : ", the game's uploads still sent");
    }
}
