// SPDX-License-Identifier: MIT

// MusyX's sequencer: it plays a song's tracks on the 16 MIDI channels of the song group, and records the notes,
// controllers, programs and tempo for MIDI output.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ntdlib/musyx/song.h"

#include "jaudio.h"
#include "musyx_game.h"
#include "musyx_voices.h"

namespace dolphinsf2::musyx
{

// The bank of a preset that plays a song group's page. Channel 10 plays the group's drum pages, whose presets are in
// kDrumBank. An effect's presets play its notes (EffectNotes). Their bank is kEffectBank, and their programs count
// from 0.
constexpr uint32_t kPageBank = 0;
constexpr uint32_t kDrumBank = 1;
constexpr uint32_t kEffectBank = 2;

// The seconds after an effect's last change at which its notes whose voices sound on are released: a sample that
// loops, or a voice that waits for the release. The last change is the last start of one of the effect's notes, or the
// last end of a note that falls silent by itself. The JAudio games' sounds held without end stop the same way.
constexpr double kHeldEffectSeconds = 10.0;

// The song group's object that `preset` plays, or nothing if the group has no such page.
std::optional<uint16_t> PresetObject(const SongGroup& group, const PresetKey& preset);

// Play `song` as MusyX's sequencer does, with the voices of `instruments`. The game's code lists the song as `listed`.
// Each preset's bank and program name a page of the instruments' song group. Throw FormatError if the song plays a
// pattern that it lacks, or loops without time passing.
Performance PlaySong(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments,
                     const PerformOptions& options);

// A note of a sound effect: the voices of the effect that start their samples `start_ms` after the effect starts, and
// the note's key. Each voice's note starts at its sample's start (KeyVoice::from_ms).
struct EffectNote
{
    double start_ms = 0.0;
    uint8_t key = 60;
    std::vector<KeyVoice> voices;
};

// The notes that play `effect` with the voices of `instruments` (EffectVoicesOf), in order: one for each time at which
// voices start their samples, at the key of the first such voice. A MIDI file has 15 channels for them: channel 10
// plays drums. Add to `approximations` what the notes leave out.
std::vector<EffectNote> EffectNotes(const GameEffect& effect, Instruments& instruments,
                                    std::set<std::string>& approximations);

// Play `effect` as MusyX does when the game starts it, with the voices of `instruments`: each note of EffectNotes on a
// MIDI channel of its own, with a preset of its own. A note's velocity is the effect's volume. Each channel's CC7,
// CC10, CC91, CC93 and pitch wheel take the game's controllers. A voice's macro can set the sends instead
// (MacroVoice::reverb_controller and chorus_controller). A note lasts until its voices stop by themselves, or until
// kHeldEffectSeconds after the effect's last change if one of them sounds on.
Performance PlayEffect(const GameEffect& effect, Instruments& instruments);

} // namespace dolphinsf2::musyx
