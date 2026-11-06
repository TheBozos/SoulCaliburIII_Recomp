#pragma once

#include "bozo/render/texture_pack.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc3
{
    using TexturePtr = std::shared_ptr<const bozo::render::Texture>;

    // SC3's texture keys over the engine's texture pack (scripts/sc3_texture_pack.py writes both): a texture is
    // found by the CALL list entry the game CALLs (where it sits in ROOT.OLK) and the TEX0 and used size a draw selects.
    class TextureKeys
    {
    public:
        struct List;
        struct Entry
        {
            const List *list = nullptr;
            uint32_t index = 0;
            bool complete = false; // every texture its uploads feed is in the pack
        };

        // The pack and the key index beside it (<pack without extension>.keys).
        bool open(const std::string &packPath);
        size_t lists() const { return m_lists.size(); }
        size_t entries() const { return m_entries; }
        size_t records() const { return m_pack.recordCount(); }
        // The CALL list entry starting `archiveOffset` bytes into ROOT.OLK, or null; `leafOffset` gets its leaf's.
        const Entry *entryAt(uint64_t archiveOffset, uint64_t *leafOffset = nullptr) const;
        // Where the game holds the leaf of a texture's CALL list: RDRAM, the EE address leaf byte 0 is at, the EE bytes
        // [lo, hi) that hold what the game read of it, and a frame number (the game's data is compared once a frame).
        struct GameData
        {
            const uint8_t *rdram = nullptr;
            uint32_t leaf = 0, lo = 0, hi = 0;
            uint64_t frame = 0;
        };
        struct LiveStats
        {
            uint64_t checked = 0, bytes = 0, palettes = 0, texels = 0, outside = 0;
        };

        // The texture TEX0 selects at that used size among the uploads from `entry` to its list's end, or null.
        // Decoded on first use and kept for the program's life. With `game`, the upload data the texture comes from is
        // the game's own: where the game changed it since it read the disc (a palette a colour edit rewrote), the
        // texture is decoded from the game's data instead (kept by content).
        TexturePtr texture(const Entry &entry, uint64_t tex0, uint32_t width, uint32_t height,
                                                   const GameData *game = nullptr);
        LiveStats takeLiveStats();

        struct Key
        {
            uint64_t tex0 = 0;
            uint16_t width = 0, height = 0;
            uint32_t firstEntry = 0, texels = 0, palette = 0;
            uint16_t paletteEntries = 0;
            uint32_t runs = 0, crc = 0;
            uint32_t sources = 0; // in its list's sources: the palette entries' leaf offsets, then the runs' (offset, bytes)
        };
        struct List
        {
            std::string name;
            std::vector<uint32_t> sources;
            const std::vector<bozo::render::packs::TextureRecord> *records = nullptr;
            std::vector<Key> keys;
            std::unordered_multimap<uint64_t, uint32_t> byKey;
        };

    private:
        struct Leaf
        {
            uint64_t offset = 0;
            uint32_t size = 0;
            std::unordered_map<uint32_t, Entry> entries;
        };
        struct Live
        {
            uint64_t frame = ~0ull;
            TexturePtr texture;
        };

        TexturePtr decoded(const List &list, const Key &key);
        TexturePtr live(const List &list, const Key &key, const GameData &game);

        bozo::render::packs::TexturePack m_pack;
        std::vector<Leaf> m_leaves; // by offset
        std::deque<List> m_lists;
        size_t m_entries = 0;
        std::unordered_map<const Key *, TexturePtr> m_decoded;
        std::map<std::pair<const Key *, uint32_t>, Live> m_live;
        std::unordered_map<uint64_t, TexturePtr> m_changed; // by content
        LiveStats m_liveStats;
        std::mutex m_mutex;
    };
}
