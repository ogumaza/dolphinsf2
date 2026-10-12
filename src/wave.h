// SPDX-License-Identifier: MIT

// Wave decoding to 16-bit PCM.

#pragma once

#include <cstdint>
#include <span>

#include "seqlib/loop.h"

#include "jaudio.h"

namespace dolphinsf2
{

// A decoded wave. JAudio's waves are mono: `channels` holds one channel.
struct Pcm : seqlib::Pcm
{
    uint32_t Length() const
    {
        return channels.empty() ? 0 : static_cast<uint32_t>(channels[0].size());
    }

    float sample_rate = 32000.0f;
};

// Decode `info`'s samples from its AW file, `aw`. A looping wave ends at its loop end. An ADPCM wave's loop starts each
// repeat from the decoder history that the wave system gives. Where that history differs from the decoder's at the loop
// start, the loop is unrolled once. The first pass and the repeats then play as in the game. Throw FormatError if the
// samples run past the file, or for 2-bit ADPCM. No game known to dolphinsf2 uses it.
Pcm DecodeWave(const WaveInfo& info, std::span<const uint8_t> aw);

using seqlib::PreparedWave;
using seqlib::PrepareForSfz;
using seqlib::PrepareForSoundFont;

} // namespace dolphinsf2
