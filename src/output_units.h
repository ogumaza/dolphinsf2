// SPDX-License-Identifier: MIT

// The units and limits that the conversions of the MusyX games, the games of the N64 library and the pieces of Red
// share: the envelopes, levels, pans and tunings of SoundFont and SFZ, and the longest performance.

#pragma once

#include <algorithm>
#include <cmath>

namespace dolphinsf2
{

// The longest performance, in seconds: a piece that hasn't repeated or ended by then stops there. JAudio2's sequences
// have the same limit.
constexpr double kLimitSeconds = 15.0 * 60.0;

// SoundFont's widest tuning in cents. seqlib's sf2/units.h gives the limits of the envelopes and the levels.
constexpr int kWidestCents = 12000;

// The pan in tenths of a percent at either side: in SoundFont's pan generator, and in the CC10 modulator of each global
// zone (seqlib's kCc10ToPan).
constexpr double kPanRange = 500.0;

// The fits of fades in lines in amplitude follow a fade down to 20 dB below its peak.
constexpr double kFitDb = 20.0;

// The lowest volume that sfzformat.com documents for an SFZ region, in decibels. Every quieter region takes it. A
// region at a gain of 0 would otherwise get -inf. sfizz 1.2 can't read that.
constexpr double kSfzLowestVolumeDb = -144.0;

// `cents` held to SoundFont's widest tuning, and rounded.
inline int ClampedCents(double cents)
{
    return static_cast<int>(std::lround(std::clamp(cents, -1.0 * kWidestCents, 1.0 * kWidestCents)));
}

// The MIDI value from 0 to 127 whose square is `amplitude`. SoundFont's and SFZ's default curves square the velocity,
// CC7 and CC11.
inline int SquaredValue(double amplitude)
{
    return static_cast<int>(std::lround(127.0 * std::sqrt(std::clamp(amplitude, 0.0, 1.0))));
}

} // namespace dolphinsf2
