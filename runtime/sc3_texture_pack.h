#pragma once

#include "runtime/native/native_gs_hook.h"
#include "runtime/native/native_texture_pack.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc3
{
    // SC3's texture keys over PS2Recomp's generic texture pack (scripts/sc3_texture_pack.py writes both): a texture is
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
        // The CALL list entry starting `archiveOffset` bytes into ROOT.OLK, or null.
        const Entry *entryAt(uint64_t archiveOffset) const;
        // The texture TEX0 selects at that used size among the uploads from `entry` to its list's end, or null.
        // Decoded on first use and kept for the program's life.
        const ps2x::native::NativeTexture *texture(const Entry &entry, uint64_t tex0, uint32_t width, uint32_t height);

        struct Key
        {
            uint64_t tex0 = 0;
            uint16_t width = 0, height = 0;
            uint32_t firstEntry = 0, texels = 0, palette = 0;
        };
        struct List
        {
            std::string name;
            const std::vector<ps2x::native::packs::TextureRecord> *records = nullptr;
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
        struct Decoded
        {
            ps2x::native::TextureCache::Texture image;
            ps2x::native::NativeTexture native;
        };

        ps2x::native::packs::TexturePack m_pack;
        std::vector<Leaf> m_leaves; // by offset
        std::deque<List> m_lists;
        size_t m_entries = 0;
        std::unordered_map<const Key *, Decoded> m_decoded;
        std::mutex m_mutex;
    };
}
