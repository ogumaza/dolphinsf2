// SPDX-License-Identifier: MIT

// Bounds-checked big-endian reads for the GameCube's formats.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace dolphinsf2
{

// Invalid or unsupported input.
class FormatError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Big-endian reader over a byte span. Out-of-bounds reads throw FormatError.
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data)
    {
    }

    std::size_t Size() const
    {
        return data_.size();
    }

    std::span<const uint8_t> Sub(std::size_t off, std::size_t len) const
    {
        Check(off, len);

        return data_.subspan(off, len);
    }

    uint8_t U8(std::size_t off) const
    {
        Check(off, 1);

        return data_[off];
    }

    uint16_t U16(std::size_t off) const
    {
        Check(off, 2);

        return static_cast<uint16_t>(data_[off] << 8 | data_[off + 1]);
    }

    int16_t S16(std::size_t off) const
    {
        return static_cast<int16_t>(U16(off));
    }

    uint32_t U32(std::size_t off) const
    {
        Check(off, 4);

        return static_cast<uint32_t>(data_[off]) << 24 | data_[off + 1] << 16 | data_[off + 2] << 8 | data_[off + 3];
    }

    float F32(std::size_t off) const
    {
        return std::bit_cast<float>(U32(off));
    }

    // Read a null-terminated string at `off`, up to `max_len` characters or the end of the data.
    std::string CString(std::size_t off, std::size_t max_len) const
    {
        Check(off, 0);
        std::string s;
        while (off < data_.size() && data_[off] && s.size() < max_len)
        {
            s.push_back(static_cast<char>(data_[off++]));
        }

        return s;
    }

    // Returns true if the 4 bytes at `off` are `m`.
    bool Magic(std::size_t off, const char* m) const
    {
        Check(off, 4);

        return std::memcmp(data_.data() + off, m, 4) == 0;
    }

private:
    void Check(std::size_t off, std::size_t len) const
    {
        if (off > data_.size() || len > data_.size() - off)
        {
            throw FormatError("read out of bounds at offset " + std::to_string(off));
        }
    }

    std::span<const uint8_t> data_;
};

} // namespace dolphinsf2
