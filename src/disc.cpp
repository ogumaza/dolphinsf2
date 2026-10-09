// SPDX-License-Identifier: MIT

// GameCube disc images (see disc.h). A disc starts with its game ID and a magic word at 0x1c. At 0x424 it gives the
// offset and size of its file table: 12-byte entries, the first the root folder, whose last field counts the entries,
// then the names.

#include "disc.h"

#include <cstring>
#include <ios>
#include <utility>

#include "binary.h"

namespace dolphinsf2
{

namespace
{

constexpr uint32_t kGameCubeMagic = 0xc2339f3d;
constexpr uint32_t kWiiMagic = 0x5d1c9ea3;

// Folders deeper than this mean a damaged table.
constexpr std::size_t kMaxDepth = 64;

uint32_t BigEndian32(const std::vector<uint8_t>& d, std::size_t at)
{
    return static_cast<uint32_t>(d[at]) << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
}

} // namespace

bool DiscImage::IsGameCube(const std::vector<uint8_t>& header)
{
    return header.size() >= 0x20 && BigEndian32(header, 0x1c) == kGameCubeMagic;
}

std::optional<std::string> UnsupportedImage(const std::vector<uint8_t>& header)
{
    const std::string kConvert = "Convert it to an ISO image with Dolphin: right-click the game, then Convert File";
    auto starts = [&](const char* magic)
    {
        return header.size() >= 4 && std::memcmp(header.data(), magic, 4) == 0;
    };

    if (starts("RVZ\x01"))
    {
        return "an RVZ disc image. dolphinsf2 can't read those. " + kConvert;
    }
    if (starts("WIA\x01"))
    {
        return "a WIA disc image. dolphinsf2 can't read those. " + kConvert;
    }
    if (header.size() >= 4 && BigEndian32(header, 0) == 0x01c00bb1)
    {
        return "a GCZ disc image. dolphinsf2 can't read those. " + kConvert;
    }
    if (starts("CISO"))
    {
        return "a CISO disc image. dolphinsf2 can't read those. " + kConvert;
    }
    if (header.size() >= 0x1c && BigEndian32(header, 0x18) == kWiiMagic)
    {
        return "a Wii disc. dolphinsf2 can't read those. Extract the game's files with Dolphin, and convert its .baa "
               "file";
    }

    return std::nullopt;
}

DiscImage DiscImage::Open(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        throw FormatError("can't open the disc image");
    }

    DiscImage disc;
    disc.path_ = path;
    f.seekg(0, std::ios::end);
    disc.size_ = static_cast<uint64_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> header(0x440);
    f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!f || !IsGameCube(header))
    {
        throw FormatError("not a GameCube disc image");
    }

    // The file table.
    const uint32_t fst_offset = BigEndian32(header, 0x424);
    const uint32_t fst_size = BigEndian32(header, 0x428);
    if (fst_size < 12 || uint64_t{fst_offset} + fst_size > disc.size_)
    {
        throw FormatError("the disc image's file table is damaged");
    }

    std::vector<uint8_t> fst(fst_size);
    f.seekg(fst_offset);
    f.read(reinterpret_cast<char*>(fst.data()), static_cast<std::streamsize>(fst.size()));
    if (!f)
    {
        throw FormatError("can't read the disc image's file table");
    }

    const Reader r(fst);
    const uint32_t entries = r.U32(8);
    if (uint64_t{entries} * 12 > fst.size())
    {
        throw FormatError("the disc image's file table is damaged");
    }

    const std::size_t names = std::size_t{entries} * 12;
    std::vector<std::pair<uint32_t, std::string>> folders = {{entries, ""}}; // each folder's end and path
    for (uint32_t i = 1; i < entries; i++)
    {
        while (folders.size() > 1 && i >= folders.back().first)
        {
            folders.pop_back();
        }

        const uint32_t word = r.U32(12 * i);
        const uint32_t a = r.U32(12 * i + 4), b = r.U32(12 * i + 8);
        const std::string name = r.CString(names + (word & 0xffffff), 255);
        const std::string path = folders.back().second + name;
        if (word >> 24)
        {
            if (folders.size() >= kMaxDepth)
            {
                throw FormatError("the disc image's file table is damaged");
            }
            folders.emplace_back(b, path + "/");
        }
        else
        {
            disc.files_.push_back({path, a, b});
        }
    }

    return disc;
}

std::vector<uint8_t> DiscImage::Read(const File& file) const
{
    if (file.offset + file.size > size_)
    {
        throw FormatError(file.path + " runs past the end of the disc image");
    }

    std::ifstream f(path_, std::ios::binary);
    std::vector<uint8_t> data(file.size);
    f.seekg(static_cast<std::streamoff>(file.offset));
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f)
    {
        throw FormatError("can't read " + file.path + " from the disc image");
    }

    return data;
}

} // namespace dolphinsf2
