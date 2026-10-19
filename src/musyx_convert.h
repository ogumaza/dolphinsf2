// SPDX-License-Identifier: MIT

// The conversion of a MusyX song to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>

#include "ntdlib/musyx/song.h"

#include "jaudio.h"
#include "musyx_game.h"
#include "musyx_voices.h"

namespace dolphinsf2::musyx
{

// Play `song` and build its MIDI, SoundFont and SFZ files from the voices of `instruments`. The game's code lists the
// song as `listed`. Throw FormatError if the song can't be played.
Conversion ConvertSong(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments,
                       const PerformOptions& options);

// A MusyX game being converted, and the instruments of its song group. It counts the instruments' errors that the
// conversions have reported. ntdlib's GameInput does the same for a JAudio game. The instruments read the game's song
// group in place. The input therefore can't be copied or moved.
class GameInput
{
public:
    explicit GameInput(Game game);
    GameInput(const GameInput&) = delete;
    GameInput& operator=(const GameInput&) = delete;

    const Game& Files() const
    {
        return game_;
    }

    // Convert the game's song `index`, with the instruments' errors since the last conversion as warnings. Throw
    // FormatError if the song can't be read or played.
    Conversion Convert(std::size_t index, const PerformOptions& perform);

private:
    Game game_;
    Instruments instruments_;
    std::size_t errors_reported_ = 0;
};

} // namespace dolphinsf2::musyx
