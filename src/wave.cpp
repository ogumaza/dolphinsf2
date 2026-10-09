// SPDX-License-Identifier: MIT

// Wave decoding (see wave.h).

#include "wave.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include "binary.h"

namespace dolphinsf2
{

namespace
{

// The predictor pairs that JAudio2 gives the DSP program for its ADPCM waves, in 2048ths (Four Swords Adventures'
// main.dol 0x8045cb40).
constexpr std::array<std::array<int32_t, 2>, 16> kAdpcmCoefficients{{
    {0, 0},
    {2048, 0},
    {0, 2048},
    {1024, 1024},
    {4096, -2048},
    {3584, -1536},
    {3072, -1024},
    {4608, -2560},
    {4200, -2248},
    {4800, -2300},
    {5120, -3072},
    {2048, -2048},
    {1024, -1024},
    {-1024, 1024},
    {-1024, 0},
    {-2048, 0},
}};

// Samples in each 9-byte frame of 4-bit ADPCM.
constexpr uint32_t kFrameSamples = 16;
constexpr std::size_t kFrameBytes = 9;

} // namespace

std::vector<int16_t> DecodeAdpcm4(std::span<const uint8_t> data, uint32_t samples, int16_t hist1, int16_t hist2)
{
    // Each frame starts with a byte whose high nibble gives the scale (a shift) and whose low nibble picks the
    // predictor pair. Sixteen signed nibbles follow, the high nibble of each byte first.
    std::vector<int16_t> out;
    out.reserve(samples);
    int32_t h1 = hist1, h2 = hist2;
    for (std::size_t frame = 0; out.size() < samples; frame++)
    {
        const std::size_t base = frame * kFrameBytes;
        if (base + kFrameBytes > data.size())
        {
            break;
        }

        const int32_t shift = data[base] >> 4;
        const auto& [c1, c2] = kAdpcmCoefficients[data[base] & 0x0f];
        for (uint32_t i = 0; i < kFrameSamples && out.size() < samples; i++)
        {
            const uint8_t byte = data[base + 1 + i / 2];
            int32_t nibble = (i & 1) ? (byte & 0x0f) : (byte >> 4);
            if (nibble >= 8)
            {
                nibble -= 16;
            }

            const int64_t v = (int64_t{nibble} << shift << 11) + int64_t{c1} * h1 + int64_t{c2} * h2;
            const auto s = static_cast<int16_t>(std::clamp<int64_t>(v >> 11, -32768, 32767));
            h2 = h1;
            h1 = s;
            out.push_back(s);
        }
    }

    return out;
}

Pcm DecodeWave(const WaveInfo& info, std::span<const uint8_t> aw)
{
    if (info.offset > aw.size() || info.size > aw.size() - info.offset)
    {
        throw FormatError("wave runs past the end of " + info.archive);
    }

    // A looping wave plays to its loop end, a one-shot wave to its last sample (JASDsp's setWaveInfo).
    Pcm pcm;
    pcm.sample_rate = info.sample_rate;
    pcm.loop = info.loop && info.loop_start < info.loop_end;
    const uint32_t length = pcm.loop ? info.loop_end : info.sample_count;
    const std::span<const uint8_t> data = aw.subspan(info.offset, info.size);
    switch (info.format)
    {
    case WaveFormat::kAdpcm4:
        pcm.samples = DecodeAdpcm4(data, length, 0, 0);
        break;

    case WaveFormat::kAdpcm2:
        throw FormatError("2-bit ADPCM waves aren't supported");

    case WaveFormat::kPcm8:
        for (uint32_t i = 0; i < length && i < data.size(); i++)
        {
            pcm.samples.push_back(static_cast<int16_t>(static_cast<int8_t>(data[i]) * 256));
        }
        break;

    case WaveFormat::kPcm16:
        for (uint32_t i = 0; i < length && 2 * std::size_t{i} + 1 < data.size(); i++)
        {
            pcm.samples.push_back(static_cast<int16_t>(data[2 * i] << 8 | data[2 * i + 1]));
        }
        break;
    }
    if (pcm.samples.size() < length)
    {
        throw FormatError("wave data is shorter than its sample count");
    }
    if (!pcm.loop)
    {
        return pcm;
    }

    pcm.loop_start = info.loop_start;
    pcm.loop_end = info.loop_end;
    if (info.format != WaveFormat::kAdpcm4)
    {
        return pcm;
    }

    // Each repeat of an ADPCM loop decodes the frame that holds the loop start from the history that the wave system
    // gives: the two samples before that frame. Where that history leads to other samples than the first pass, play the
    // first pass once and loop on a repeat. In Four Swords Adventures, every loop stores the first pass's history.
    const uint32_t skip = info.loop_start % kFrameSamples;
    const std::size_t loop_frame = std::size_t{info.loop_start} / kFrameSamples * kFrameBytes;
    if (loop_frame > data.size())
    {
        return pcm;
    }

    const uint32_t loop_length = info.loop_end - info.loop_start;
    std::vector<int16_t> repeat =
        DecodeAdpcm4(data.subspan(loop_frame), skip + loop_length, info.loop_last, info.loop_penult);
    if (repeat.size() < skip + loop_length)
    {
        return pcm;
    }

    repeat.erase(repeat.begin(), repeat.begin() + skip);
    if (std::equal(repeat.begin(), repeat.end(), pcm.samples.begin() + info.loop_start))
    {
        return pcm;
    }

    pcm.samples.insert(pcm.samples.end(), repeat.begin(), repeat.end());
    pcm.loop_start = info.loop_end;
    pcm.loop_end = info.loop_end + loop_length;

    return pcm;
}

namespace
{

// Unroll `pcm`'s loop so it starts at or after `min_start`, has at least `min_before` repeated samples before it, and
// is at least `min_length` samples long. Append `after` samples from the loop start for interpolation.
PreparedWave UnrollLoop(const Pcm& pcm, uint32_t min_start, uint32_t min_before, uint32_t min_length, uint32_t after)
{
    PreparedWave w;
    w.loop = pcm.loop;
    std::vector<int16_t> src = pcm.samples;
    if (src.empty())
    {
        src.push_back(0); // a sample requires at least one point
    }
    if (!pcm.loop)
    {
        w.samples = std::move(src);
        return w;
    }

    const uint32_t ls = pcm.loop_start, le = pcm.loop_end, len = le - ls;
    const std::vector<int16_t> body(src.begin() + ls, src.begin() + le);
    uint32_t before = 0;
    while (ls + before * len < min_start || before * len < min_before)
    {
        before++;
    }

    uint32_t reps = 1;
    while (reps * len < min_length)
    {
        reps++;
    }

    std::vector<int16_t> out(src.begin(), src.begin() + ls);
    for (uint32_t i = 0; i < before; i++)
    {
        out.insert(out.end(), body.begin(), body.end());
    }

    w.loop_start = static_cast<uint32_t>(out.size());
    for (uint32_t i = 0; i < reps; i++)
    {
        out.insert(out.end(), body.begin(), body.end());
    }
    w.loop_end = static_cast<uint32_t>(out.size());

    for (uint32_t i = 0; i < after; i++)
    {
        out.push_back(body[i % len]);
    }
    w.samples = std::move(out);

    return w;
}

} // namespace

PreparedWave PrepareForSoundFont(const Pcm& pcm)
{
    return UnrollLoop(pcm, 8, 0, 32, 32);
}

PreparedWave PrepareForSfz(const Pcm& pcm)
{
    const auto crossfade = static_cast<uint32_t>((pcm.sample_rate * 4.0f + 999.0f) / 1000.0f); // 4 ms
    return UnrollLoop(pcm, 0, std::max<uint32_t>(crossfade, 64), 64, 64);
}

} // namespace dolphinsf2
