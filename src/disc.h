// SPDX-License-Identifier: MIT

// GameCube disc images: their file table, and the files in them.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace dolphinsf2
{

// A GameCube disc image, plain (.iso, .gcm) or NKit. NKit images keep the file table's offsets valid. Both kinds read
// the same way. Compressed images (RVZ, WIA, GCZ, CISO) and Wii discs are refused with a message saying what to do.
class DiscImage
{
public:
    // A file on the disc: its path from the disc's root, with '/' between folders.
    struct File
    {
        std::string path;
        uint64_t offset = 0;
        uint32_t size = 0;
    };

    // Open an image. Throw FormatError if it isn't a GameCube disc image or can't be read.
    static DiscImage Open(const std::filesystem::path& path);

    // True if `header` (at least the image's first 0x20 bytes) is a GameCube disc's.
    static bool IsGameCube(const std::vector<uint8_t>& header);

    const std::vector<File>& Files() const
    {
        return files_;
    }

    // Read a file. Throw FormatError if it runs past the end of the image.
    std::vector<uint8_t> Read(const File& file) const;

private:
    std::filesystem::path path_;
    uint64_t size_ = 0;
    std::vector<File> files_;
};

// If `header` starts a disc image format that dolphinsf2 can't read, say which and what to do.
std::optional<std::string> UnsupportedImage(const std::vector<uint8_t>& header);

} // namespace dolphinsf2
