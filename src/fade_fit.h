// SPDX-License-Identifier: MIT

// The fit of a SoundFont zone's or an SFZ region's volume envelope to fades in lines in amplitude. The envelopes of the
// Nintendo 64's sound library and of Square's Red driver fade that way. A SoundFont's or SFZ's decay and release fall
// in lines in decibels.

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "seqlib/sf2/presets.h"
#include "seqlib/sfz/opcodes.h"

namespace dolphinsf2
{

// An envelope's points: each one's time in milliseconds and its amplitude, in rising time from the first. The fades run
// in lines between them. The last amplitude holds.
using FadeCurve = std::vector<std::pair<double, double>>;

// A volume envelope: an attack in a line in amplitude, up to the peak, a hold, and a decay and a release in lines in
// decibels. The sustain is relative to the peak. A release at a rate of 0 never falls. A decay at a rate of 0 falls to
// the sustain at once.
struct FadeShape
{
    double peak = 1.0; // the amplitude that the attack reaches
    double attack_ms = 0.0;
    double hold_ms = 0.0;
    double decay_db_per_ms = 0.0;
    double sustain_db = 0.0;
    double release_db_per_ms = 0.0;

    // Where the decay is slower than the slowest in SoundFont: the hold before the slowest decay that brings a
    // SoundFont zone closest to the curve. Without it, a SoundFont zone takes hold_ms.
    std::optional<double> soundfont_hold_ms;
};

// The shape that follows `curve` from its first point down to 20 dB under its peak, or down to the level that it holds.
// The release stays at 0. A decay slower than SoundFont's slowest also gets the hold for SoundFont.
FadeShape FitFades(const FadeCurve& curve);

// The amplitude of `curve` at `ms`.
double FadeAmplitude(const FadeCurve& curve, double ms);

// The rate in decibels a millisecond of a release that follows a line in amplitude to silence in `ms`.
double FitRelease(double ms);

// `shape`'s times, sustain and release in the generators of a SoundFont zone.
void SetEnvelope(const FadeShape& shape, seqlib::sf2::ZoneValues& values);

// `shape`'s times, sustain and release in SFZ's opcodes.
seqlib::sfz::AmpEnvelope SfzEnvelope(const FadeShape& shape);

} // namespace dolphinsf2
