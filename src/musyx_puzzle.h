// SPDX-License-Identifier: MIT

// Nintendo Puzzle Collection's music and sound effects. The disc holds a menu and three games: Dr. Mario, Panel de Pon
// and Yoshi's Cookie. main.dol loads each of the four as a module of code (rel_module.h) from a file that Yay0 packs
// (yay0.h). A module holds its songs and its MusyX groups' files, or a table of where another file holds them. The
// songs come in the order of the menu, Dr. Mario, Panel de Pon and Yoshi's Cookie. A song's name starts with its
// module's name, such as "panepon-flower_01".

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "musyx_game.h"

namespace dolphinsf2::musyx
{

// The songs and the effects of Nintendo Puzzle Collection, the groups that play them and the songs' SNG files. The
// `group` of a song or an effect is an index of `groups`. A song's SNG file has the song's index in `song_data`. The
// effects come in the same order as the songs: the menu's, then those of Dr. Mario, Panel de Pon and Yoshi's Cookie.
// An effect's name starts with its module's name, such as "panepon-se-0012".
struct PuzzleMusic
{
    std::vector<std::shared_ptr<const SongGroup>> groups;
    std::vector<GameSong> songs;
    std::vector<std::vector<uint8_t>> song_data;
    std::vector<GameEffect> effects;
};

// Read Nintendo Puzzle Collection's music and effects. `read_file` reads a file of the disc by its path. Every song
// plays at the master volume `master_volume`. Throw FormatError if a file is missing or damaged.
PuzzleMusic ReadPuzzleCollection(const std::function<std::vector<uint8_t>(const std::string& path)>& read_file,
                                 uint8_t master_volume);

} // namespace dolphinsf2::musyx
