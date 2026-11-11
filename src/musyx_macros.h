// SPDX-License-Identifier: MIT

// The interpreter of MusyX's macros, which musyx_voices.cpp runs for each note of a voice.

#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "ntdlib/musyx/pool.h"

#include "musyx_voices.h"

namespace dolphinsf2::musyx::macros
{

// The time of an event that never comes, such as the release of a note that the player holds.
constexpr double kNever = std::numeric_limits<double>::infinity();

// The approximation that a voice's report gives for the samples after its first.
constexpr char kLaterSamplesLeftOut[] = "a second sample of a voice left out";

// What a run of a macro's steps found.
struct Run
{
    MacroVoice voice;
    std::optional<double> release_at; // the first KeyOff, in milliseconds after the note's start
    std::optional<double> stop_at;    // the first StopSample
    std::vector<VolumeRamp> ramps;    // the Envelope steps, each from its time after the note's start
    bool sample_started = false;
    bool envelope_before_sample = false;
    std::vector<MacroVoice> parts; // a part for each sample, in order (MacroVoice::parts)
};

// The settings of the pitch of `voice`, from `ms` after its start.
inline PitchSettings PitchSettingsOf(const MacroVoice& voice, double ms)
{
    return {.ms = ms,
            .key_shift = voice.key_shift,
            .fixed_key = voice.fixed_key,
            .detune_cents = voice.detune_cents,
            .rate_hz = voice.rate_hz};
}

// Run `macro`'s steps for a note that the player releases `release_ms` after its start: kNever for a note that it
// holds. ScaleVolume divides by `volume_scale_divisor`.
Run RunMacro(const ntdlib::musyx::Pool& pool, uint16_t macro, double release_ms, int volume_scale_divisor,
             const ChannelControls& controls);

} // namespace dolphinsf2::musyx::macros
