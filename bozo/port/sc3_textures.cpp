// SC3's texture pack at the game's per-part hooks: which upload lists the frame being built has CALLed, from the game's own
// command list, and where the game read them from ROOT.OLK. Nothing is uploaded: a part's draw takes its texture from here.
#include "sc3_draw.h"
#include "sc3_textures.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sc3::textures
{
    namespace
    {
        constexpr uint32_t kGifPacketClose = 0x0010F808u;
        constexpr uint32_t kModelCopyMin = 0x1000u;
        constexpr uint32_t kTextureCall = 0x00104DCCu;
        constexpr uint32_t kAnimatedTextureCall = 0x00104E48u;
        constexpr uint32_t kTextureCallOt = 0x00104E94u;
        constexpr uint32_t kAnimatedTextureCallOt = 0x00104E34u;
        constexpr uint32_t kSector = 2048u;

        struct Group
        {
            const TextureKeys::Entry *entry = nullptr;
            uint32_t target = 0;
            TextureKeys::GameData game;
        };

        struct Read
        {
            uint32_t end = 0;
            uint64_t olk = 0;
        };

        std::optional<TextureKeys> g_packOwner{std::in_place};
        TextureKeys &g_pack = *g_packOwner;
        bool g_ready = false;
        uint8_t *g_rdram = nullptr;
        int64_t g_olkLbn = -1;
        uint32_t g_requestDest = 0, g_requestPos = 0;
        bool g_requestPending = false;
        std::map<uint32_t, Read> g_reads;
        std::vector<Group> g_building;
        uint64_t g_frames = 0;
        uint64_t g_calls = 0, g_packed = 0;
        std::map<uint32_t, uint64_t> g_unknownCalls;

        uint32_t peek32(uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, g_rdram + (address & 0x01FFFFFFu & ~3u), 4);
            return value;
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

        void noteRead(uint32_t buffer, uint32_t end, uint64_t olk)
        {
            auto it = g_reads.lower_bound(buffer);
            if (it != g_reads.begin() && std::prev(it)->second.end > buffer)
                --it;
            while (it != g_reads.end() && it->first < end)
                it = g_reads.erase(it);
            g_reads[buffer] = Read{end, olk};
        }

        uint32_t callTarget(R5900Context *ctx, uint32_t ra)
        {
            const uint32_t command = getRegU32(ctx, 20);
            if (ra == kTextureCall)
                return peek32(command - 8u);
            if (ra == kTextureCallOt)
                return peek32(command - 4u);
            const uint32_t table = peek32(command + 4u);
            const uint32_t frame = peek32(peek32(command + 8u));
            return peek32(table + static_cast<uint32_t>(fp::ps2FpuCvtWS(fp::flt(frame))) * 4u);
        }

        void noteCall(R5900Context *ctx, uint32_t ra)
        {
            const uint32_t target = callTarget(ctx, ra);
            Group group;
            group.target = target;
            group.entry = entryAt(target, &group.game);
            g_building.push_back(group);
            ++g_calls;
            if (group.entry)
                ++g_packed;
            else
                ++g_unknownCalls[target];
        }

        // 0x10F808: a texture CALL appended to the GIF list by 0x104AF0's commands 2 and 20.
        void gifPacketClose(uint8_t *rdram, R5900Context *ctx, Runtime *runtime)
        {
            g_rdram = rdram;
            const uint32_t ra = getRegU32(ctx, 31);
            if (ra == kTextureCall || ra == kAnimatedTextureCall)
                noteCall(ctx, ra);
            func_0x10f808(rdram, ctx, runtime);
        }
    }

    bool ready() { return g_ready; }

    void noteDepthLink(R5900Context *ctx)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        if (g_ready && (ra == kTextureCallOt || ra == kAnimatedTextureCallOt))
            noteCall(ctx, ra);
    }

    void noteRequest(R5900Context *ctx)
    {
        if (getRegU32(ctx, 5) == 1u && g_olkLbn < 0)
        {
            g_requestDest = getRegU32(ctx, 8);
            g_requestPos = getRegU32(ctx, 6);
            g_requestPending = true;
        }
    }

    void noteReadBefore(uint8_t *rdram, R5900Context *ctx)
    {
        g_rdram = rdram;
        const uint32_t lbn = getRegU32(ctx, 4), sectors = getRegU32(ctx, 5), buffer = getRegU32(ctx, 6) & kRamMask;
        if (g_olkLbn < 0 && g_requestPending && buffer == (g_requestDest & kRamMask) && (g_requestPos % kSector) == 0u)
            g_olkLbn = static_cast<int64_t>(lbn) - g_requestPos / kSector;
        if (g_olkLbn >= 0 && lbn >= g_olkLbn && sectors != 0u)
            noteRead(buffer, buffer + sectors * kSector, static_cast<uint64_t>(lbn - g_olkLbn) * kSector);
    }

    void noteCopy(uint8_t *rdram, R5900Context *ctx)
    {
        g_rdram = rdram;
        const uint32_t to = getRegU32(ctx, 4) & kRamMask, from = getRegU32(ctx, 5) & kRamMask, size = getRegU32(ctx, 6);
        if (size >= kModelCopyMin && size < 0x2000000u)
        {
            auto read = g_reads.upper_bound(from);
            if (read != g_reads.begin() && from + size <= (--read)->second.end)
                noteRead(to, to + size, read->second.olk + (from - read->first));
        }
    }

    void frameDone()
    {
        if (!g_ready)
            return;
        g_building.clear();
        if (++g_frames % 250u != 0u)
            return;
        std::fprintf(stderr, "[sc3:textures] 250 frames: %llu upload lists called, %llu in the pack, %zu not;\n", static_cast<unsigned long long>(g_calls),
                     static_cast<unsigned long long>(g_packed), g_unknownCalls.size());
        g_calls = g_packed = 0;
        g_unknownCalls.clear();
    }

    TexturePtr atHook(uint64_t tex0, uint32_t width, uint32_t height, uint32_t part)
    {
        for (const bool ownOnly : {true, false})
        {
            if (ownOnly && part == 0u)
                continue;
            for (size_t g = g_building.size(); g-- > 0;)
            {
                Group &group = g_building[g];
                if (!group.entry || (ownOnly && (part < group.game.lo || part >= group.game.hi)))
                    continue;
                if (TexturePtr texture = g_pack.texture(*group.entry, tex0, width, height, &group.game))
                    return texture;
            }
        }
        return nullptr;
    }

    void install(Registry &runtime)
    {
        if (g_ready)
            return;
        const char *path = std::getenv("SC3_TEXTURE_PACK");
        if (!path || !*path)
        {
            std::fprintf(stderr, "[sc3:textures] no SC3_TEXTURE_PACK\n");
            return;
        }
        if (!g_pack.open(path))
            return;
        std::fprintf(stderr, "[sc3:textures] %s, %zu upload lists, %zu entries, %zu records\n", path, g_pack.lists(), g_pack.entries(), g_pack.records());
        g_ready = true;
        runtime.registerFunction(kGifPacketClose, &gifPacketClose);
    }

    void shutdown()
    {
        g_ready = false;
        g_building.clear();
        g_packOwner.reset();
    }
}
