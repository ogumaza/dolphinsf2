// SPDX-License-Identifier: MIT

// The music of a GameCube game that plays it through a port of Nintendo's sound library for the Nintendo 64: Animal
// Crossing, Doubutsu no Mori+ and Doubutsu no Mori e+. dolphinsf2 recognizes a game by the SHA-1 of its main.dol. The
// game numbers its pieces of music, and a table in main.dol gives each number's sequence (SEQ_TABLE). The game's code
// then changes how some pieces play. A piece that plays the hour holds an arrangement for each kind of weather. The
// game mutes the channels of the others. The museum's piece holds one for each room. In e+, the island's piece holds
// one for when the islander is there and one for when he isn't. A music disc's piece plays from a stereo. Each of
// Kapp'n's songs plays in two parts. dolphinsf2 lists each arrangement as a piece (GameInput::Pieces).

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"

#include "jaudio.h"
#include "n64_player.h"
#include "n64_sound.h"

namespace dolphinsf2::n64
{

// A game with the Nintendo 64's sound library being converted: its sound data and its pieces. It counts the banks'
// errors that the conversions have reported.
class GameInput
{
public:
    // Open the game whose files `disc` holds, with its main.dol. Return nullptr if the game isn't one that dolphinsf2
    // converts. Throw FormatError if the game's tables or its sound data can't be read.
    static std::unique_ptr<GameInput> Open(const std::shared_ptr<ntdlib::disc::GameFiles>& disc);

    // The pieces, by the game's numbers for them. A piece that plays the hour comes in each weather with snow, cherry
    // blossom or neither, and the museum's piece in each room. Another number is listed if the game plays it as a
    // piece of music and no lower such number plays the same sequence. A piece's name is "bgm" and its number in three
    // digits. For a piece of the hour, of the museum or of the island, a hyphen and the weather's or the room's name
    // follow, or whether the islander is there: "bgm013-snow", "bgm088-fossils", "bgm376-no-islander".
    const std::vector<Piece>& Pieces() const
    {
        return pieces_;
    }

    // Convert piece `index`, with the banks' errors since the last conversion as warnings. Throw FormatError if the
    // piece can't be played.
    Conversion Convert(std::size_t index, const PerformOptions& perform);

private:
    GameInput(SoundData data, std::vector<Piece> pieces);

    SoundData data_;
    std::vector<Piece> pieces_;
    std::size_t errors_reported_ = 0;
};

} // namespace dolphinsf2::n64
