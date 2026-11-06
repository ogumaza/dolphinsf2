// SPDX-License-Identifier: MIT

// The music of a GameCube game that plays it through Square's Red driver: Final Fantasy Crystal Chronicles. Each piece
// is a file of tracks of commands (a .bgm file, with the magic "BGM "). Its header names a bank of waves (a .wd file,
// with the magic "WD"). dolphinsf2 finds a game by the folders of these files on its disc. It lists each piece by its
// file's name, such as "music101". The driver's effects aren't converted.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"

#include "jaudio.h"

namespace dolphinsf2::red
{

// A game with the Red driver being converted.
class GameInput
{
public:
    // Open the game whose files `disc` holds. Return nullptr if its disc has no folder of Red's pieces.
    static std::unique_ptr<GameInput> Open(const std::shared_ptr<ntdlib::disc::GameFiles>& disc);

    // The names of the pieces, in the order of their numbers.
    const std::vector<std::string>& Pieces() const
    {
        return names_;
    }

    // Convert piece `index`. Throw FormatError if its file or its bank can't be read or played.
    Conversion Convert(std::size_t index, const PerformOptions& perform);

private:
    GameInput(std::shared_ptr<ntdlib::disc::GameFiles> disc, std::vector<std::string> names,
              std::vector<std::string> paths);

    std::shared_ptr<ntdlib::disc::GameFiles> disc_;
    std::vector<std::string> names_;
    std::vector<std::string> paths_;
};

} // namespace dolphinsf2::red
