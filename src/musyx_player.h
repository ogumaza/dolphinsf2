// SPDX-License-Identifier: MIT

// MusyX's sequencer: it plays a song's tracks on the 16 MIDI channels of the song group, and records the notes,
// controllers, programs and tempo for MIDI output.

#pragma once

#include <cstdint>
#include <optional>

#include "ntdlib/musyx/song.h"

#include "jaudio.h"
#include "musyx_game.h"
#include "musyx_voices.h"

namespace dolphinsf2::musyx
{

// The bank of a preset that plays a song group's page. Channel 10 plays the group's drum pages, whose presets are in
// kDrumBank.
constexpr uint32_t kPageBank = 0;
constexpr uint32_t kDrumBank = 1;

// The song group's object that `preset` plays, or nothing if the group has no such page.
std::optional<uint16_t> PresetObject(const SongGroup& group, const PresetKey& preset);

// Play `song` as MusyX's sequencer does, with the voices of `instruments`. The game's code lists the song as `listed`.
// Each preset's bank and program name a page of the instruments' song group. Throw FormatError if the song plays a
// pattern that it lacks, or loops without time passing.
Performance PlaySong(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments,
                     const PerformOptions& options);

} // namespace dolphinsf2::musyx
