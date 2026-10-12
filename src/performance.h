// SPDX-License-Identifier: MIT

// Run BMS sequences as JAudio2 does and record note, controller and preset events for MIDI output.
//
// Jumps, calls, registers, ports and conditions require execution rather than static translation. The performer follows
// JAudio2's sequence player as Four Swords Adventures links it: the parser's command table, the tracks with their timed
// parameters, and the channels that its sub-frame updates move on. The parts that JAudio2 shares with Twilight
// Princess's build follow that, and the instruments follow The Wind Waker's JAudio.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sequence_export/performance_base.h"
#include "seqlib/midi/performance.h"

#include "jaudio.h"

namespace dolphinsf2
{

class BankSet;

// The vibrato depth that CC1's full value gives, in cents. CC1 holds 128 times the depth over this. The performer
// records CC1 that way, and the SoundFont's and SFZ's modulators read it that way.
constexpr double kVibratoRangeCents = 100.0;

// Note-on settings that a MIDI channel can't represent. Each distinct PresetKey makes a SoundFont preset.
struct PresetKey
{
    bool operator==(const PresetKey&) const = default;

    uint32_t bank = 0;
    uint32_t program = 0;

    // The track's oscillators that replace the instrument's (envset and simpleadsr), as indexes into
    // Performance::oscillators, or -1 for the instrument's.
    int volume_oscillator = -1;
    int pitch_oscillator = -1;

    uint16_t direct_release = 0; // the track's direct release, 0 for none

    // The vibrato's speed and delay at note-on. Its depth goes by CC1.
    float vibrato_rate = 1.0f;  // in 72nds of a cycle per sub-frame
    uint16_t vibrato_delay = 0; // in sub-frames
};

// The events of a performance's tracks, from seqlib.
using EventKind = seqlib::midi::EventKind;
using TrackEvent = seqlib::midi::TrackEvent;

// Recorded sequence. ntdlib's PerformanceBase holds seqlib's part (the tempo, the tracks' events and names, each tick's
// sub-frame, the length and the main loop) and what the performance found. This part holds the presets.
struct Performance : ntdlib::sequence_export::PerformanceBase
{
    std::vector<PresetKey> presets;
    std::vector<Oscillator> oscillators; // the tracks' oscillators that presets name
};

// Options for playing a sequence.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays the intro and one loop, with MIDI loop markers.
    int loops = 1;

    // The state the game's random generator starts in (JMath::TRandom_fast_, which JAudio2's register command seeds
    // with 0).
    uint32_t seed = 0;

    // Values for the sequence's ports (0-15), set before it starts. Games write ports to steer their music, and some
    // sequences read one when they start.
    std::map<uint8_t, uint16_t> ports;

    // The voices that notes play on, from --voices. With a count, a note that finds no voice free takes the voice of an
    // older note of no higher priority, or is lost, as in the game with that many voices. The game has 64. With 0,
    // every note is kept.
    int voice_limit = 0;
};

// A sequence to play: its data, the offset its first track starts at, and the sound's volume from the sound table.
struct SequenceSound
{
    std::string name;
    std::span<const uint8_t> data;
    uint32_t start = 0;
    float volume = 1.0f;
};

// Play `sound`. Throw FormatError if it can't be read.
Performance Perform(const SequenceSound& sound, BankSet& banks, const PerformOptions& options);

using seqlib::midi::NoteCount;

} // namespace dolphinsf2
