// SPDX-License-Identifier: MIT

// The shapes that musyx_convert.cpp gives a MusyX voice in the SoundFont and SFZ files: its volume envelope, its
// pitch envelope, its tremolo and the LFO of its pitch wheel.

#pragma once

#include <limits>
#include <optional>

#include "musyx_voices.h"
#include "output_units.h"

namespace dolphinsf2::musyx::shapes
{

// The rate of a fall that falls at once.
constexpr double kAtOnce = std::numeric_limits<double>::infinity();

// MusyX's milliseconds in real ones. Its clock counts the output's samples as if they came at kNominalRate.
constexpr double kMsScale = kNominalRate / kOutputRate;

// A volume envelope in SoundFont's and SFZ's terms, in real milliseconds: a delay, an attack that rises linearly in
// amplitude, a hold, and a decay and a release that fall linearly in decibels.
struct VolumeShape
{
    double delay_ms = 0.0;
    double attack_ms = 0.0;
    double hold_ms = 0.0;
    double decay_db_per_ms = kAtOnce;
    double sustain_db = 0.0;
    double release_db_per_ms = kAtOnce;
};

// The volume envelope of `voice` at `volume` with the voice's key `key` and the note's velocity `velocity`.
VolumeShape VolumeOf(const Instruments& instruments, const MacroVoice& voice, double volume, int key, int velocity);

// A pitch envelope in SoundFont's and SFZ's terms, in real milliseconds. MusyX's decay and release fall linearly in
// decibels of their range. SoundFont's fall in a line, fitted to reach half the range at the same time as MusyX's.
// SFZ's decays and releases fall by a factor of e^-8 in their time. MusyX's decays and releases fall by the same factor
// in kSfzFallDb / kDlsFallDb of their time.
struct PitchShape
{
    double attack_ms = 0.0;
    double decay_ms = 0.0;   // SoundFont's
    double release_ms = 0.0; // SoundFont's
    double sfz_decay_ms = 0.0;
    double sfz_release_ms = 0.0;
    double sustain = 0.0; // the share of the range
    double range_cents = 0.0;
};

// The pitch envelope of `envelope` with the voice's key `key`.
PitchShape PitchOf(const PitchEnvelope& envelope, int key);

// A tremolo in decibels: MusyX's volume swings between full level and the depth's fall, a sine on the amplitude. The
// conversions swing it in decibels about the middle: half the range down, give or take half the range.
struct TremoloShape
{
    double hz = 0.0;
    double half_range_db = 0.0;
    double half_mod_range_db = 0.0; // what the mod wheel at its top adds
};

// The rate in hertz of an LFO whose period is `period_ms` of MusyX's milliseconds.
double LfoHz(double period_ms);

// The tremolo of `voice`, if it has one.
std::optional<TremoloShape> TremoloOf(const MacroVoice& voice);

// The depth in cents of `lfo`, an LFO on the pitch wheel of `voice`: its share of the wheel's ranges. MusyX swings the
// pitch up by the upper range and down by the lower one. The files swing it by their mean either way.
double WheelLfoCents(const MacroVoice& voice, const WheelLfo& lfo);

// The LFO on the pitch wheel of `voice` that a file plays, if any. A SoundFont plays it on the modulation LFO, which
// a tremolo may hold. The LFO then plays only if it runs at the tremolo's rate from the note's start.
std::optional<WheelLfo> PlayedWheelLfo(const MacroVoice& voice);

// The timecents of a fall of a VolumeShape at `db_per_ms`. The fall covers kSoundFontFallDb in that time. A fall at
// kAtOnce takes the shortest time of SoundFont.
int ShapeFallTimecents(double db_per_ms);

// The value of a keynumTo generator for a time that changes by `per_key` timecents a key. A positive value shortens
// the time as the key rises. SoundFont allows -1200 to 1200.
int KeyScaleTimecents(double per_key);

} // namespace dolphinsf2::musyx::shapes
