// SPDX-License-Identifier: MIT

// The conversion of a MusyX song to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>

#include "ntdlib/musyx/song.h"

#include "convert.h"
#include "musyx_game.h"
#include "musyx_voices.h"

namespace dolphinsf2::musyx
{

// Play `song` and build its MIDI, SoundFont and SFZ files from the voices of `instruments`. The game's code lists the
// song as `listed`. Throw FormatError if the song can't be played.
Conversion ConvertSong(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments,
                       const PerformOptions& options);

// Convert song `index` of `game` in the same way. Throw FormatError if the song can't be read.
Conversion ConvertSong(const Game& game, std::size_t index, Instruments& instruments, const PerformOptions& options);

} // namespace dolphinsf2::musyx
