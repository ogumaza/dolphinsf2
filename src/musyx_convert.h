// SPDX-License-Identifier: MIT

// The conversion of a MusyX song to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

// Play `effect` and build its MIDI, SoundFont and SFZ files from the voices of `instruments` (PlayEffect).
Conversion ConvertEffect(const GameEffect& effect, Instruments& instruments);

// The names that the SFZ files' shared folder gives the samples of `groups`, a game's groups: for each group, its
// samples' names by their IDs. The first group that holds an ID names its sample "s" and the ID, such as "s27". A
// later group can give the ID another sample. That sample's name also gives the index of the first group that holds
// it, such as "s7_27". Two samples are the same when their entries agree apart from the offset and their data is the
// same.
std::vector<std::map<uint16_t, std::string>> SfzSampleNames(
    const std::vector<std::shared_ptr<const SongGroup>>& groups);

// A MusyX game being converted, and the instruments of its song groups. It counts the instruments' errors that the
// conversions have reported. ntdlib's GameInput does the same for a JAudio game. The instruments read the game's song
// groups in place. The input therefore can't be copied or moved.
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

    // Convert the game's song `index`, with the instruments' errors since the last conversion as warnings. An index
    // past the songs converts an effect: index 0 of Game::Effects follows the last song. Throw FormatError if the song
    // can't be read or played.
    Conversion Convert(std::size_t index, const PerformOptions& perform);

private:
    // The names that the SFZ files give the samples that group `group` plays: its samples' names, then those of the
    // groups pushed with it for the IDs that it lacks (SongGroup::pushed).
    std::map<uint16_t, std::string> SfzNames(std::size_t group) const;

    Game game_;

    // The instruments of each song group and the number of their errors that the conversions have reported. A group's
    // instruments are made when its first song is converted.
    std::vector<std::unique_ptr<Instruments>> instruments_;
    std::vector<std::size_t> errors_reported_;

    // The names of each group's samples in the SFZ files (SfzSampleNames).
    std::vector<std::map<uint16_t, std::string>> sfz_names_;
};

} // namespace dolphinsf2::musyx
