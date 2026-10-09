// SPDX-License-Identifier: MIT

// Wave decoding to 16-bit PCM.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "jaudio.h"

namespace dolphinsf2
{

// A decoded wave. JAudio's waves are mono.
struct Pcm
{
    uint32_t Length() const
    {
        return static_cast<uint32_t>(samples.size());
    }

    float sample_rate = 32000.0f;
    bool loop = false;       // only when loop_start < loop_end
    uint32_t loop_start = 0; // first sample of the loop
    uint32_t loop_end = 0;   // one past the loop's last sample: the end of the wave when it loops
    std::vector<int16_t> samples;
};

// Decode `info`'s samples from its AW file, `aw`. A looping wave ends at its loop end. An ADPCM wave's loop starts each
// repeat from the decoder history that the wave system gives. Where that history differs from the decoder's at the loop
// start, the loop is unrolled once. The first pass and the repeats then play as in the game. Throw FormatError if the
// samples run past the file, or for 2-bit ADPCM. No game known to dolphinsf2 uses it.
Pcm DecodeWave(const WaveInfo& info, std::span<const uint8_t> aw);

// Decode 4-bit ADPCM (AFC: 16 samples in 9 bytes) from `hist1` and `hist2`, the two samples before the first. Exposed
// for tests.
std::vector<int16_t> DecodeAdpcm4(std::span<const uint8_t> data, uint32_t samples, int16_t hist1, int16_t hist2);

// Samples and loop bounds prepared for SoundFont or SFZ output.
struct PreparedWave
{
    std::vector<int16_t> samples;
    bool loop = false;
    uint32_t loop_start = 0, loop_end = 0;
};

// Repeat loop data to meet SoundFont requirements: 8 samples before the loop, at least 32 within it and 8 after it.
// Playback is unchanged.
PreparedWave PrepareForSoundFont(const Pcm& pcm);

// Prepare samples for sfizz. Its loop crossfade spans at least 1 ms and blends the loop end with data before the loop
// start. Prepend 4 ms of repeated loop data so the crossfade blends identical samples. Extend the loop to at least 64
// samples and append 64 samples from its start for interpolation. Playback is unchanged.
PreparedWave PrepareForSfz(const Pcm& pcm);

} // namespace dolphinsf2
