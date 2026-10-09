// SPDX-License-Identifier: MIT

// Run BMS sequences as JAudio2 does and record note, controller and preset events for MIDI output.
//
// Jumps, calls, registers, ports and conditions require execution rather than static translation. The performer follows
// JAudio2's sequence player as Four Swords Adventures links it: the parser's command table, the tracks with their timed
// parameters, and the channels that its sub-frame updates move on. The parts that JAudio2 shares with Twilight
// Princess's build follow that, and the instruments follow The Wind Waker's JAudio.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "jaudio.h"

namespace dolphinsf2
{

class BankSet;

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

// Event types, with the fields used by each.
enum class EventKind : uint8_t
{
    kNoteOn,     // key, value: velocity
    kNoteOff,    // key
    kPreset,     // preset: index into Performance::presets
    kVolume,     // value: 0-127 (CC7)
    kExpression, // value: 0-127 (CC11)
    kPan,        // value: 0-127, 64 centre (CC10)
    kPitch,      // semitones: the track's bend and a sweeping note's pitch
    kModulation, // value: vibrato depth (CC1)
    kReverb,     // value: the effect send (CC91)
};

// An event on a track's timeline.
//
// MIDI has one set of controllers per channel. JAudio2's channels take their track's settings until the track detaches
// them. Notes that the game plays with other settings than the track's later notes go on another part of the track.
// The MIDI writer gives that part another channel. A track's changes go to the part its new notes go to; a note's
// events go to its part.
//
// An event belongs to the sub-frame its tick is processed in. A pitch that a sweep reaches in a sub-frame between ticks
// comes `after` the tick by the fraction of a tick at which the sub-frame starts.
struct TrackEvent
{
    uint32_t tick = 0;
    EventKind kind = EventKind::kNoteOn;
    uint8_t key = 0; // 0-127
    uint8_t value = 0;
    uint32_t preset = 0;
    float semitones = 0.0f;
    uint32_t note = 0; // NoteOn/NoteOff ID, starting at 1; distinguishes overlapping notes of the same key
    uint8_t part = 0;  // the part of the track, 0 for its first
    double after = 0;  // 0, between 0 and 1 for a sub-frame between ticks, or more after the last tick
};

// Recorded sequence: tempo, track events, presets, main loop and duration.
struct Performance
{
    // A tempo change.
    struct Tempo
    {
        uint32_t tick = 0;
        double bpm = 120.0; // quarter notes (`timebase` ticks) per minute
    };

    // The main loop, in ticks.
    struct Loop
    {
        uint32_t start = 0, end = 0;
    };

    uint32_t timebase = 48; // ticks per quarter note
    std::vector<Tempo> tempo;

    // Each MIDI track's events, and the name of the sequence track it records.
    std::array<std::vector<TrackEvent>, 16> tracks;
    std::array<std::string, 16> track_names;

    std::vector<PresetKey> presets;
    std::vector<Oscillator> oscillators; // the tracks' oscillators that presets name

    // MIDI attenuation in dB. CC11 stops at 127. Scale all CC11 values equally to fit. The tracks keep their balance.
    // SFZ adds this gain back.
    double level_cut_db = 0.0;

    // Start of each tick's sub-frame, expressed as a fractional tick. MIDI events are placed at these positions.
    std::vector<double> frame_ticks;

    uint32_t end_tick = 0;
    double seconds = 0;       // length, release tails included
    std::optional<Loop> loop; // if it loops
    int loops = 0;            // times the main loop was played
    bool truncated = false;   // stopped at the time limit
    bool holds = false;       // holds until stopped by the game; subject to the hold limit
    std::vector<std::string> warnings;

    // Counts of features approximated or omitted in MIDI/SoundFont output.
    std::map<std::string, uint32_t> approximations;
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

// Returns the number of notes that `perf` plays.
uint32_t NoteCount(const Performance& perf);

} // namespace dolphinsf2
