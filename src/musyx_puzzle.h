// SPDX-License-Identifier: MIT

// Nintendo Puzzle Collection's music. The disc holds a menu and three games: Dr. Mario, Panel de Pon and Yoshi's
// Cookie. main.dol loads each of the four as a module of code (rel_module.h) from a file that Yay0 packs (yay0.h). A
// module holds its songs and its MusyX groups' files, or a table of where another file holds them. The songs come in
// the order of the menu, Dr. Mario, Panel de Pon and Yoshi's Cookie. A song's name starts with its module's name, such
// as "panepon-flower_01".

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "musyx_game.h"

namespace dolphinsf2::musyx
{

// The songs of Nintendo Puzzle Collection, the song groups that play them and the songs' SNG files. A song's `group` is
// an index of `groups`. Its SNG file has the song's index in `song_data`.
struct PuzzleMusic
{
    std::vector<std::shared_ptr<const SongGroup>> groups;
    std::vector<GameSong> songs;
    std::vector<std::vector<uint8_t>> song_data;
};

// Read Nintendo Puzzle Collection's music. `read_file` reads a file of the disc by its path. Every song plays at the
// master volume `master_volume`. Throw FormatError if a file is missing or damaged.
PuzzleMusic ReadPuzzleCollection(const std::function<std::vector<uint8_t>(const std::string& path)>& read_file,
                                 uint8_t master_volume);

} // namespace dolphinsf2::musyx
