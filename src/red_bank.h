// SPDX-License-Identifier: MIT

// The banks of Square's Red driver: their instruments' regions and their DSP-ADPCM waves (see red_music.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "jaudio.h"

namespace dolphinsf2::red
{

// The rate of the DSP's mixing in hertz, and the samples of one of AX's frames. A wave plays at this rate at a ratio of
// 1, on key 60 with a pitch of 0.
constexpr double kDspRate = 32028.5;
constexpr double kFrameSamples = 160.0;
constexpr double kFramesPerSecond = kDspRate / kFrameSamples;

// The driver's scale of a level: a value of x gives (x + 1) / 128. Its centre pan and its highest pan.
constexpr double kLevelScale = 128.0;
constexpr int kCentre = 64;
constexpr int kHighestPan = 127;

// A region of a bank's instrument: the keys that it plays, the wave's place in the bank and how it plays.
struct Region
{
    uint32_t sample = 0; // the byte offset of its wave in the bank's samples
    int32_t loop = -1;   // the nibble at which the loop starts, or less than 0 for no loop
    uint32_t end = 0;    // the last nibble
    int pitch = 0;       // in 1/256 of a semitone
    uint8_t low = 0;
    uint8_t high = 127;
    uint8_t volume = 127;
    int pan = -1; // 0 to 127, or -1 for the track's pan
    int16_t yn1 = 0;
    int16_t yn2 = 0;
    std::array<uint16_t, 4> times{};
    std::array<uint8_t, 4> levels{};
    std::array<int16_t, 16> coefs{};

    // The way in which the region decodes its wave, among the regions of the bank that start their waves at the same
    // byte: 0 for the first way in the bank, 1 for the next, and so on. The end, the loop, the coefficients and the
    // history of the region decide the way.
    int variant = 0;
};

// A bank's instruments, and its waves once they're decoded.
class Bank
{
public:
    Bank(int number, std::vector<uint8_t> data);

    // The regions of instrument `instrument`, or nothing if the bank has no such instrument.
    std::span<const Region> Regions(std::size_t instrument) const;

    int Number() const;

    // The decoded wave of `region`, or nullptr if its wave starts past the end of the bank.
    const Pcm* Wave(const Region& region);

private:
    uint32_t Word(std::size_t at) const;

    Region ReadRegion(std::size_t at) const;

    void Read();

    // Set the variant of each region (Region::variant), in the order of the instruments and their regions.
    void NumberVariants();

    // The driver takes the first region whose highest key isn't below the key. The last region takes every key above.
    void ReadInstrument(std::size_t at, std::vector<Region>& regions) const;

    uint8_t ReadByte(std::size_t at) const;

    std::optional<Pcm> Decode(const Region& region) const;

    int number_ = 0;
    std::vector<uint8_t> data_;
    std::size_t samples_ = 0;
    std::vector<std::vector<Region>> instruments_;
    std::map<std::pair<uint32_t, int>, std::optional<Pcm>> waves_; // by the start of the wave and the variant
};

} // namespace dolphinsf2::red
