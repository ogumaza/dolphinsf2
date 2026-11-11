// SPDX-License-Identifier: MIT

// The banks of Square's Red driver (see red_bank.h).

#include "red_bank.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ntdlib/wave/decode.h"
#include "xsflib/bytes.h"

namespace dolphinsf2::red
{
namespace
{

// A bank's header: the bytes of its samples at 4, its instruments at 8 and a table of their first regions at 0x20. The
// samples end the file.
constexpr std::size_t kBankTable = 0x20;
constexpr std::size_t kRegionSize = 0x60;

// A region's flags: the next record holds the right side of a stereo wave, the instrument's last region, and that the
// instrument's first region starts a list of splits. Only the first region's flags decide that (_WaveSplitSelect).
constexpr uint32_t kRegionStereo = 0x1;
constexpr uint32_t kRegionLast = 0x200;
constexpr uint32_t kRegionSplits = 0x30000;

// A region's fixed pan: its byte at 0x1b holds this flag and the pan.
constexpr uint8_t kFixedPan = 0x80;

// Whether regions `a` and `b` decode a wave that starts at the same byte the same way.
bool SameDecode(const Region& a, const Region& b)
{
    return a.end == b.end && a.loop == b.loop && a.coefs == b.coefs && a.yn1 == b.yn1 && a.yn2 == b.yn2;
}

// The sample of a nibble in DSP-ADPCM: a frame holds 16 nibbles, and its first 2 hold the frame's header.
uint32_t SampleOfNibble(uint32_t nibble)
{
    return nibble / 16 * 14 + std::max(nibble % 16, 2u) - 2;
}

} // namespace

Bank::Bank(int number, std::vector<uint8_t> data) : number_(number), data_(std::move(data))
{
    Read();
}

std::span<const Region> Bank::Regions(std::size_t instrument) const
{
    return instrument < instruments_.size() ? std::span<const Region>(instruments_[instrument])
                                            : std::span<const Region>();
}

int Bank::Number() const
{
    return number_;
}

const Pcm* Bank::Wave(const Region& region)
{
    const std::pair<uint32_t, int> key{region.sample, region.variant};
    auto found = waves_.find(key);
    if (found == waves_.end())
    {
        found = waves_.emplace(key, Decode(region)).first;
    }

    return found->second ? &*found->second : nullptr;
}

uint32_t Bank::Word(std::size_t at) const
{
    if (at + 4 > data_.size())
    {
        throw FormatError("a bank ends inside its header or a region");
    }

    return xsflib::LoadBe32(data_.data() + at);
}

Region Bank::ReadRegion(std::size_t at) const
{
    if (at + kRegionSize > data_.size())
    {
        throw FormatError("a bank ends inside a region");
    }

    const uint8_t* p = data_.data() + at;
    Region r;
    r.sample = xsflib::LoadBe32(p + 4);
    r.loop = static_cast<int32_t>(xsflib::LoadBe32(p + 8));
    r.end = xsflib::LoadBe32(p + 0xc);
    r.pitch = static_cast<int32_t>(xsflib::LoadBe32(p + 0x14)) >> 16;
    r.volume = p[0x1a] & 0x7f;
    if ((p[0x1b] & kFixedPan) != 0)
    {
        const int pan = p[0x1b] & 0x7f;
        r.pan = pan == 0 ? kCentre : pan;
    }
    for (std::size_t i = 0; i < r.coefs.size(); i++)
    {
        r.coefs[i] = static_cast<int16_t>(xsflib::LoadBe16(p + 0x22 + 2 * i));
    }
    r.yn1 = static_cast<int16_t>(xsflib::LoadBe16(p + 0x46));
    r.yn2 = static_cast<int16_t>(xsflib::LoadBe16(p + 0x48));
    for (std::size_t i = 0; i < r.times.size(); i++)
    {
        r.times[i] = xsflib::LoadBe16(p + 0x50 + 2 * i);
        r.levels[i] = p[0x58 + i];
    }

    return r;
}

void Bank::Read()
{
    if (data_.size() < kBankTable || data_[0] != 'W' || data_[1] != 'D')
    {
        throw FormatError("a bank doesn't start with the magic WD");
    }

    const uint32_t samples = Word(4);
    const uint32_t count = Word(8);
    if (samples > data_.size() || uint64_t{count} * 4 + kBankTable > data_.size())
    {
        throw FormatError("a bank's samples or its table of instruments run past its end");
    }
    samples_ = data_.size() - samples;

    instruments_.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t first = Word(kBankTable + std::size_t{i} * 4);
        if (first != 0)
        {
            ReadInstrument(first, instruments_[i]);
        }
    }
    NumberVariants();
}

void Bank::NumberVariants()
{
    std::map<uint32_t, std::vector<const Region*>> ways; // the first region of each way, by the start of the wave
    for (std::vector<Region>& regions : instruments_)
    {
        for (Region& r : regions)
        {
            std::vector<const Region*>& known = ways[r.sample];
            const auto same =
                std::find_if(known.begin(), known.end(), [&r](const Region* k) { return SameDecode(*k, r); });
            r.variant = static_cast<int>(same - known.begin());
            if (same == known.end())
            {
                known.push_back(&r);
            }
        }
    }
}

void Bank::ReadInstrument(std::size_t at, std::vector<Region>& regions) const
{
    const bool splits = (Word(at) & kRegionSplits) != 0;
    int low = 0;
    while (low <= 127)
    {
        const uint32_t flags = Word(at);
        const std::size_t records = (flags & kRegionStereo) != 0 ? 2 : 1;
        const bool last = !splits || (flags & kRegionLast) != 0;
        const int high = last ? 127 : static_cast<int8_t>(ReadByte(at + 0x18));
        if (high >= low)
        {
            for (std::size_t side = 0; side < records; side++)
            {
                Region r = ReadRegion(at + side * kRegionSize);
                r.low = static_cast<uint8_t>(low);
                r.high = static_cast<uint8_t>(high);
                if (records == 2)
                {
                    r.pan = side == 0 ? 0 : kHighestPan;
                }
                if (r.end != 0)
                {
                    regions.push_back(r);
                }
            }
            low = high + 1;
        }

        if (last)
        {
            break;
        }

        at += records * kRegionSize;
    }
}

uint8_t Bank::ReadByte(std::size_t at) const
{
    if (at >= data_.size())
    {
        throw FormatError("a bank ends inside a region");
    }

    return data_[at];
}

std::optional<Pcm> Bank::Decode(const Region& region) const
{
    const uint64_t start = uint64_t{samples_} + region.sample;
    if (start >= data_.size())
    {
        return std::nullopt;
    }

    // The right side of a stereo wave can end past the bank's samples. The wave then ends with the bank.
    const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(region.end, (data_.size() - start) * 2 - 1));

    ntdlib::wave::EncodedChannel channel;
    channel.data = std::span<const uint8_t>(data_).subspan(static_cast<std::size_t>(start));
    channel.coefs = &region.coefs;
    channel.yn1 = region.yn1;
    channel.yn2 = region.yn2;

    ntdlib::wave::EncodedWave wave;
    wave.encoding = ntdlib::wave::Encoding::kDspAdpcm;
    wave.sum = ntdlib::wave::DspAdpcmSum::kAccelerator;
    wave.loop_end = SampleOfNibble(end) + 1;
    if (region.loop >= 0 && SampleOfNibble(static_cast<uint32_t>(region.loop)) < wave.loop_end)
    {
        wave.loop = true;
        wave.loop_start = SampleOfNibble(static_cast<uint32_t>(region.loop));
    }
    wave.channels.push_back(channel);

    ntdlib::wave::Pcm decoded = ntdlib::wave::DecodeWave(wave);
    Pcm pcm;
    pcm.loop = decoded.loop;
    pcm.loop_start = decoded.loop_start;
    pcm.loop_end = decoded.loop_end;
    pcm.channels = std::move(decoded.channels);
    pcm.sample_rate = static_cast<float>(kDspRate);

    return pcm;
}

} // namespace dolphinsf2::red
