// SPDX-License-Identifier: MIT

// The units and limits that the conversions of the MusyX games, the games of the N64 library and the pieces of Red
// share: the envelopes, levels, pans and tunings of SoundFont and SFZ, and the longest performance.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace dolphinsf2
{

// The longest performance, in seconds: a piece that hasn't repeated or ended by then stops there. JAudio2's sequences
// have the same limit.
constexpr double kLimitSeconds = 15.0 * 60.0;

// SoundFont's limits: the shortest and the longest time of an envelope in timecents, the longest delay or hold of an
// envelope and the longest delay of an LFO, silence in centibels, and the widest tuning in cents.
constexpr int kShortestTimecents = -12000;
constexpr int kLongestTimecents = 8000;
constexpr int kLongestHoldTimecents = 5000;
constexpr int kSilentCentibels = 1440;
constexpr int kWidestCents = 12000;

// The pan in tenths of a percent at either side: in SoundFont's pan generator, and in the CC10 modulator of each global
// zone (seqlib's kCc10ToPan).
constexpr double kPanRange = 500.0;

// The decibels that the decay and the release of a SoundFont envelope fall in their time.
constexpr double kSoundFontFallDb = 100.0;

// The decibels that the decay and the release of an SFZ envelope fall in their time: a factor of e^-8, or
// 8 × 20 / ln 10 dB. sfzformat.com gives that curve on its page on ampeg_attack.
constexpr double kSfzFallDb = 8.0 * 20.0 / std::numbers::ln10;

// The fits of fades in lines in amplitude follow a fade down to 20 dB below its peak.
constexpr double kFitDb = 20.0;

// The lowest volume that sfzformat.com documents for an SFZ region, in decibels. Every quieter region takes it. A
// region at a gain of 0 would otherwise get -inf. sfizz 1.2 can't read that.
constexpr double kSfzLowestVolumeDb = -144.0;

// The sustain of a SoundFont envelope `sustain_db` below its peak, in centibels, held to SoundFont's range.
inline int SustainCentibels(double sustain_db)
{
    return std::clamp(static_cast<int>(std::lround(-10.0 * sustain_db)), 0, kSilentCentibels);
}

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
