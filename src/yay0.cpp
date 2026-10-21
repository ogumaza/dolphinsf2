// SPDX-License-Identifier: MIT

// Yay0 (see yay0.h).

#include "yay0.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace dolphinsf2
{
namespace
{

constexpr uint32_t kMagic = 0x59617930; // "Yay0"
constexpr std::size_t kHeaderSize = 16;
constexpr uint32_t kMostBytes = 64u << 20;

// The bounds of a damaged file's tables give this message.
constexpr char kDamaged[] = "the Yay0 data is damaged";

} // namespace

bool IsYay0(std::span<const uint8_t> file)
{
    return file.size() >= 4 && xsflib::LoadBe32(file.data()) == kMagic;
}

std::vector<uint8_t> UnpackYay0(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true, {.bounds_message = xsflib::BoundsMessage::kFixed, .fixed_message = kDamaged});
    if (!IsYay0(file) || file.size() < kHeaderSize)
    {
        throw xsflib::FormatError("not Yay0 data");
    }

    const uint32_t size = r.U32(4);
    if (size > kMostBytes)
    {
        throw xsflib::FormatError("the Yay0 data unpacks to more than 64 MiB");
    }

    std::size_t flags_at = kHeaderSize;
    std::size_t references_at = r.U32(8);
    std::size_t bytes_at = r.U32(12);
    std::vector<uint8_t> out;
    out.reserve(size);
    uint32_t flags = 0;
    int flags_left = 0;
    while (out.size() < size)
    {
        if (flags_left == 0)
        {
            flags = r.U32(flags_at);
            flags_at += 4;
            flags_left = 32;
        }

        const bool literal = (flags & 0x80000000u) != 0;
        flags <<= 1;
        flags_left--;
        if (literal)
        {
            out.push_back(r.U8(bytes_at++));
            continue;
        }

        const uint16_t reference = r.U16(references_at);
        references_at += 2;
        const std::size_t distance = (reference & 0xfffu) + 1u;
        std::size_t length = reference >> 12;
        length = length == 0 ? r.U8(bytes_at++) + 18u : length + 2u;
        if (distance > out.size())
        {
            throw xsflib::FormatError("the Yay0 data refers to bytes before its start");
        }

        // A reference may overlap the bytes that it writes. The last one may run past the unpacked size.
        for (std::size_t i = 0; i < length && out.size() < size; i++)
        {
            out.push_back(out[out.size() - distance]);
        }
    }

    return out;
}

} // namespace dolphinsf2
