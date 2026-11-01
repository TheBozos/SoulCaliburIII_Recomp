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
        constexpr char kMagic[8] = {'S', 'C', '3', 'K', 'E', 'Y', 'S', '1'};
        constexpr uint64_t kTex0Key = 0x3FFFull | (0x3Full << 14) | (0x3Full << 20) | (0xFull << 26) | (0xFull << 30) |
                                      (0x3FFFull << 37) | (0xFull << 51) | (1ull << 55) | (0x1Full << 56);
        constexpr uint32_t kNoPalette = 0xFFFFFFFFu;

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
                    !texture_keys::read(file, key.firstEntry) || !texture_keys::read(file, key.texels) || !texture_keys::read(file, key.palette))
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

    const TextureKeys::Entry *TextureKeys::entryAt(uint64_t archiveOffset) const
    {
        const auto leaf = std::upper_bound(m_leaves.begin(), m_leaves.end(), archiveOffset,
                                           [](uint64_t offset, const Leaf &l) { return offset < l.offset; });
        if (leaf == m_leaves.begin())
            return nullptr;
        const Leaf &found = *std::prev(leaf);
        if (archiveOffset >= found.offset + found.size)
            return nullptr;
        const auto entry = found.entries.find(static_cast<uint32_t>(archiveOffset - found.offset));
        return entry == found.entries.end() ? nullptr : &entry->second;
    }

    const ps2x::native::NativeTexture *TextureKeys::texture(const Entry &entry, uint64_t tex0, uint32_t width, uint32_t height)
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
            auto decoded = m_decoded.find(&key);
            if (decoded == m_decoded.end())
            {
                const ps2x::native::packs::TextureRecord &texels = (*list.records)[key.texels];
                uint32_t palette[256] = {};
                if (key.palette != texture_keys::kNoPalette && key.palette < list.records->size())
                {
                    const ps2x::native::packs::TextureRecord &colours = (*list.records)[key.palette];
                    std::memcpy(palette, colours.texels.data(), std::min<size_t>(sizeof(palette), colours.texels.size()));
                }
                Decoded &out = m_decoded[&key];
                ps2x::native::packs::decodeRecord(out.image, key.width, key.height, texels.psm, texels, palette, 0);
                out.native.width = out.image.width;
                out.native.height = out.image.height;
                out.native.rgba = out.image.rgba.data();
                out.native.key = reinterpret_cast<uintptr_t>(&key);
                decoded = m_decoded.find(&key);
            }
            return decoded->second.native.rgba ? &decoded->second.native : nullptr;
        }
        return nullptr;
    }
}
