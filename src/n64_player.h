// SPDX-License-Identifier: MIT

// The sequence player of the Nintendo 64's sound library, as Animal Crossing's port runs it. A sequence player runs its
// script once a tick. The script starts up to 16 channels, each with a script of its own, and each channel starts up
// to four layers, whose scripts play the notes. The player records the notes, the controllers, the presets and the
// tempo for MIDI output: each channel on a MIDI track of its own.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "jaudio.h"
#include "n64_sound.h"

namespace dolphinsf2::n64
{

// The vibrato depth that CC1's full value gives, in cents. CC1 holds 127 times the depth over this.
constexpr double kVibratoRangeCents = 400.0;

// What a voice plays: an instrument of a bank, the bank's drums or its effects.
enum class VoiceKind : uint8_t
{
    kInstrument,
    kDrums,
    kEffects,
};

// What a preset of a performance plays, and how its notes start. Each distinct PresetVoice makes a SoundFont preset.
struct PresetVoice
{
    bool operator==(const PresetVoice&) const = default;

    VoiceKind kind = VoiceKind::kInstrument;
    uint8_t bank = 0;
    uint8_t program = 0; // the instrument, or for effects the high bits of the effect's number (EffectKey)

    // The envelope that the notes start and the decay index of their release. A note whose layer or drum sets a
    // release takes that envelope and release. For the others, these are the channel's.
    std::vector<EnvelopePoint> envelope;
    uint8_t release = 0;

    // The vibrato's rate (in 65536ths of a cycle an update) and its delay (in updates) at note-on. Its depth goes by
    // CC1.
    uint16_t vibrato_rate = 0;
    uint16_t vibrato_delay = 0;

    // The channel's pan weight: the 128ths of a note's pan that the channel's pan gives. The note's pan gives the
    // rest: `note_pan` for an instrument, or each drum's pan.
    uint8_t pan_weight = 128;
    uint8_t note_pan = 64;
};

// The MIDI key of effect number `number`: its low 7 bits. The rest of the number gives the program that holds it.
constexpr uint8_t EffectKey(uint32_t number)
{
    return static_cast<uint8_t>(number & 0x7f);
}

// The MIDI key of the library's semitone `semitone`: 39 is middle C, key 60.
constexpr int KeyOf(int semitone)
{
    return semitone + 21;
}

// A piece of music to play: the sequence, the channels that the game mutes, the game's volume for it, and whether the
// game holds every channel's pan in the centre. The game writes `ports` to the sequence player's ports before the first
// tick. A script that starts `end_sequence` in its player ends the piece as the game means it to.
struct Piece
{
    std::string name;
    uint32_t sequence = 0;
    uint16_t muted = 0; // a bit for each channel
    float volume = 1.0f;
    bool centre_pans = false;
    std::vector<std::pair<uint8_t, int8_t>> ports;
    int end_sequence = -1;
};

// A performance and what each of its presets plays (PresetVoice). Element i of `voices` belongs to preset i.
struct Played
{
    Performance performance;
    std::vector<PresetVoice> voices;
};

// Play `piece` as the library does, with the banks of `data`. Throw FormatError if the sequence can't be read.
Played PlayPiece(const Piece& piece, SoundData& data, const PerformOptions& options);

} // namespace dolphinsf2::n64
