#include "sc3_texture_pack.h"


#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

namespace sc3
{
    namespace texture_keys
    {
    namespace
    {
        constexpr char kMagic[8] = {'S', 'C', '3', 'K', 'E', 'Y', 'S', '2'};
        constexpr uint64_t kTex0Key = 0x3FFFull | (0x3Full << 14) | (0x3Full << 20) | (0xFull << 26) | (0xFull << 30) |
                                      (0x3FFFull << 37) | (0xFull << 51) | (1ull << 55) | (0x1Full << 56);
        constexpr uint32_t kNoPalette = 0xFFFFFFFFu;
        constexpr uint32_t kNoSource = 0xFFFFFFFFu;

        // zlib's CRC-32 (the converter's zlib.crc32), slicing by 4.
        struct Crc32
        {
            uint32_t table[4][256];
            Crc32()
            {
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[0][i] = c;
                }
                for (uint32_t i = 0; i < 256u; ++i)
                    for (int t = 1; t < 4; ++t)
                        table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFFu];
            }
            uint32_t operator()(uint32_t crc, const uint8_t *data, size_t size) const
            {
                crc = ~crc;
                for (; size >= 4u; size -= 4u, data += 4)
                {
                    uint32_t word;
                    std::memcpy(&word, data, 4);
                    crc ^= word;
                    crc = table[3][crc & 0xFFu] ^ table[2][(crc >> 8) & 0xFFu] ^ table[1][(crc >> 16) & 0xFFu] ^ table[0][crc >> 24];
                }
                for (; size; --size, ++data)
                    crc = table[0][(crc ^ *data) & 0xFFu] ^ (crc >> 8);
                return ~crc;
            }
        };
        const Crc32 kCrc32;

        template <typename T>
        bool read(std::ifstream &file, T &out)
        {
            file.read(reinterpret_cast<char *>(&out), sizeof(T));
            return static_cast<bool>(file);
        }

        uint64_t key(uint64_t tex0, uint32_t width, uint32_t height)
        {
            return (tex0 & kTex0Key) ^ (static_cast<uint64_t>(width) << 1) ^ (static_cast<uint64_t>(height) << 17);
        }

        std::string keysPath(const std::string &packPath)
        {
            const size_t dot = packPath.find_last_of('.');
            const size_t slash = packPath.find_last_of("/\\");
            return (dot != std::string::npos && (slash == std::string::npos || dot > slash) ? packPath.substr(0, dot) : packPath) + ".keys";
        }
    }
    }

    bool TextureKeys::open(const std::string &packPath)
    {
        if (!m_pack.load(packPath))
        {
            std::fprintf(stderr, "[sc3] texture pack: %s is not a PS2XTEX1 pack\n", packPath.c_str());
            return false;
        }
        const std::string path = texture_keys::keysPath(packPath);
        std::ifstream file(path, std::ios::binary);
        char magic[8] = {};
        uint64_t archiveSize = 0;
        uint32_t lists = 0;
        file.read(magic, 8);
        if (!file || std::memcmp(magic, texture_keys::kMagic, 8) != 0 || !texture_keys::read(file, archiveSize) ||
            !texture_keys::read(file, lists))
        {
            std::fprintf(stderr, "[sc3] texture pack: %s is not an SC3KEYS1 index\n", path.c_str());
            return false;
        }
        std::map<uint64_t, Leaf> leaves;
        for (uint32_t l = 0; l < lists; ++l)
        {
            uint16_t nameLength = 0;
            if (!texture_keys::read(file, nameLength))
                return false;
            std::string name(nameLength, '\0');
            file.read(name.data(), nameLength);
            uint64_t offset = 0;
            uint32_t size = 0, entries = 0, keys = 0;
            if (!texture_keys::read(file, offset) || !texture_keys::read(file, size) || !texture_keys::read(file, entries) ||
                !texture_keys::read(file, keys))
                return false;
            List &list = m_lists.emplace_back();
            list.name = name;
            list.records = m_pack.file(name);
            Leaf &leaf = leaves[offset];
            leaf.offset = offset;
            leaf.size = size;
            for (uint32_t e = 0; e < entries; ++e)
            {
                uint32_t at = 0, complete = 0;
                if (!texture_keys::read(file, at) || !texture_keys::read(file, complete))
                    return false;
                leaf.entries[at] = Entry{&list, e, complete != 0u};
            }
            m_entries += entries;
            for (uint32_t k = 0; k < keys; ++k)
            {
                Key key;
                if (!texture_keys::read(file, key.tex0) || !texture_keys::read(file, key.width) || !texture_keys::read(file, key.height) ||
                    !texture_keys::read(file, key.firstEntry) || !texture_keys::read(file, key.texels) || !texture_keys::read(file, key.palette) ||
                    !texture_keys::read(file, key.paletteEntries) || !texture_keys::read(file, key.runs) || !texture_keys::read(file, key.crc) ||
                    key.paletteEntries > 256u)
                    return false;
                key.sources = static_cast<uint32_t>(list.sources.size());
                list.sources.resize(list.sources.size() + key.paletteEntries + size_t{key.runs} * 2u);
                file.read(reinterpret_cast<char *>(list.sources.data() + key.sources), static_cast<std::streamsize>((list.sources.size() - key.sources) * 4u));
                if (!file)
                    return false;
                list.keys.push_back(key);
            }
            for (uint32_t k = 0; k < list.keys.size(); ++k)
                list.byKey.emplace(texture_keys::key(list.keys[k].tex0, list.keys[k].width, list.keys[k].height), k);
        }
        for (auto &[offset, leaf] : leaves)
            m_leaves.push_back(std::move(leaf));
        return true;
    }

    const TextureKeys::Entry *TextureKeys::entryAt(uint64_t archiveOffset, uint64_t *leafOffset) const
    {
        const auto leaf = std::upper_bound(m_leaves.begin(), m_leaves.end(), archiveOffset,
                                           [](uint64_t offset, const Leaf &l) { return offset < l.offset; });
        if (leaf == m_leaves.begin())
            return nullptr;
        const Leaf &found = *std::prev(leaf);
        if (archiveOffset >= found.offset + found.size)
            return nullptr;
        const auto entry = found.entries.find(static_cast<uint32_t>(archiveOffset - found.offset));
        if (entry == found.entries.end())
            return nullptr;
        if (leafOffset)
            *leafOffset = found.offset;
        return &entry->second;
    }

    namespace texture_keys
    {
    namespace
    {
        void packPalette(const std::vector<bozo::render::packs::TextureRecord> &records, uint32_t record, uint32_t (&palette)[256])
        {
            std::memset(palette, 0, sizeof(palette));
            if (record != kNoPalette && record < records.size())
                std::memcpy(palette, records[record].texels.data(), std::min<size_t>(sizeof(palette), records[record].texels.size()));
        }

        TexturePtr decodeInto(const TextureKeys::Key &key, const bozo::render::packs::TextureRecord &texels, const uint32_t (&palette)[256], uint64_t id)
        {
            auto image = std::make_shared<bozo::render::Texture>();
            bozo::render::packs::decodeRecord(*image, key.width, key.height, texels.psm, texels, palette, 0);
            image->key = id;
            return image;
        }
    }
    }

    TexturePtr TextureKeys::decoded(const List &list, const Key &key)
    {
        auto decoded = m_decoded.find(&key);
        if (decoded == m_decoded.end())
        {
            uint32_t palette[256];
            texture_keys::packPalette(*list.records, key.palette, palette);
            decoded = m_decoded.emplace(&key, texture_keys::decodeInto(key, (*list.records)[key.texels], palette, reinterpret_cast<uintptr_t>(&key))).first;
        }
        return decoded->second->rgba.empty() ? nullptr : decoded->second;
    }

    // The game's palette words where its upload data has them, and the CRC-32 of its texels' upload data (one frame in
    // eight), against the disc's (palette record, key CRC). Changed palette: the pack's texels under the game's colours.
    // Changed texels are counted and keep the pack's.
    TexturePtr TextureKeys::live(const List &list, const Key &key, const GameData &game)
    {
        Live &live = m_live[{&key, game.leaf}];
        if (live.frame == game.frame)
            return live.texture;
        const bool texelsDue = live.frame == ~0ull || ((game.frame + (reinterpret_cast<uintptr_t>(&key) >> 4)) & 7u) == 0u;
        live.frame = game.frame;
        ++m_liveStats.checked;
        const auto at = [&](uint32_t offset, uint32_t bytes) -> const uint8_t *
        {
            const uint32_t address = game.leaf + offset;
            if (offset == texture_keys::kNoSource || address < game.lo || address > game.hi || game.hi - address < bytes)
                return nullptr;
            return game.rdram + (address & 0x01FFFFFFu);
        };
        uint32_t palette[256];
        texture_keys::packPalette(*list.records, key.palette, palette);
        const uint32_t *sources = list.sources.data() + key.sources;
        bool paletteChanged = false, outside = false;
        for (uint32_t i = 0; i < key.paletteEntries; ++i)
        {
            if (sources[i] == texture_keys::kNoSource)
                continue;
            const uint8_t *word = at(sources[i], 4u);
            if (!word)
            {
                outside = true;
                continue;
            }
            uint32_t colour;
            std::memcpy(&colour, word, 4);
            paletteChanged |= colour != palette[i];
            palette[i] = colour;
        }
        uint32_t crc = 0;
        for (uint32_t r = 0; texelsDue && r < key.runs; ++r)
        {
            const uint32_t offset = sources[key.paletteEntries + r * 2u], bytes = sources[key.paletteEntries + r * 2u + 1u];
            const uint8_t *data = at(offset, bytes);
            if (!data)
            {
                outside = true;
                break;
            }
            crc = texture_keys::kCrc32(crc, data, bytes);
            m_liveStats.bytes += bytes;
        }
        m_liveStats.outside += outside ? 1u : 0u;
        if (texelsDue && !outside && crc != key.crc)
            ++m_liveStats.texels;
        if (!paletteChanged)
            return live.texture = decoded(list, key);
        ++m_liveStats.palettes;
        uint64_t id = (static_cast<uint64_t>(texture_keys::kCrc32(0u, reinterpret_cast<const uint8_t *>(palette), sizeof(palette))) << 32) ^
                      reinterpret_cast<uintptr_t>(&key);
        id = (id ^ (id >> 30)) * 0xBF58476D1CE4E5B9ull;
        id = (id ^ (id >> 27)) * 0x94D049BB133111EBull;
        id = (id ^ (id >> 31)) | (1ull << 63);
        auto changed = m_changed.find(id);
        if (changed == m_changed.end())
        {
            changed = m_changed.emplace(id, texture_keys::decodeInto(key, (*list.records)[key.texels], palette, id)).first;
        }
        return live.texture = changed->second->rgba.empty() ? nullptr : changed->second;
    }

    TextureKeys::LiveStats TextureKeys::takeLiveStats()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const LiveStats stats = m_liveStats;
        m_liveStats = LiveStats{};
        return stats;
    }

    TexturePtr TextureKeys::texture(const Entry &entry, uint64_t tex0, uint32_t width, uint32_t height,
                                                            const GameData *game)
    {
        const List &list = *entry.list;
        if (!list.records)
            return nullptr;
        const auto [first, last] = list.byKey.equal_range(texture_keys::key(tex0, width, height));
        for (auto it = first; it != last; ++it)
        {
            const Key &key = list.keys[it->second];
            if (key.firstEntry < entry.index || key.texels >= list.records->size())
                continue;
            std::lock_guard<std::mutex> lock(m_mutex);
            return game ? live(list, key, *game) : decoded(list, key);
        }
        return nullptr;
    }
}
