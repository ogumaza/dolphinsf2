// SPDX-License-Identifier: MIT

// The music of a GameCube game that plays it through Factor 5's MusyX: the songs that the game's code lists, and the
// song group that plays them. ntdlib's musyx module reads the files. dolphinsf2 recognizes a game by the SHA-1 of its
// main.dol. It converts two games by Camelot. Camelot names each file on its discs with three capital letters, a path
// of three levels (PathOf).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"

namespace dolphinsf2::musyx
{

// A song that the game's code lists: its name, its file, the ID that names its MIDI setup, and its volume. `level` is
// a factor on every channel volume that brings a game's songs to the game's level (see musyx_game.cpp).
struct GameSong
{
    std::string name;     // the code's name for the song, without ".sng"
    std::string path;     // the song's file on the disc
    uint16_t id = 0;      // the song's ID in the song group's MIDI setups
    uint8_t volume = 127; // the code's volume for the song, in 127ths
    double level = 1.0;
};

// A list of songs in main.dol: `count` entries of `entry_size` bytes at `address`. An entry ends with three words: the
// song's file number, its ID, and its volume in the top byte. Before them, the entry holds the song's name, or the
// address of the name.
struct SongList
{
    uint32_t address = 0;
    uint32_t count = 0;
    uint32_t entry_size = 0;
    bool name_inline = false;
};

// The path of a file by its number in a song list: the number halved, in base 26 with the letters A to Z. The path has
// three letters with a slash between each.
std::string PathOf(uint32_t number);

// The songs of `list` in `dol`, the game's main.dol. Throw FormatError if `dol` has no data where an entry or a name
// should be.
std::vector<GameSong> ReadSongList(const ntdlib::Dol& dol, const SongList& list);

// The song group that plays a game's music: the group of the project file, the pool, the sample directory and the
// samples.
struct SongGroup
{
    ntdlib::musyx::Group group;
    ntdlib::musyx::Pool pool;
    std::vector<ntdlib::musyx::SampleInfo> samples;
    std::vector<uint8_t> sample_data;
};

class Game
{
public:
    // Open the MusyX game whose files `disc` holds, with its main.dol. Return nothing if the game isn't one that
    // dolphinsf2 converts. Throw FormatError if the song group or the code's list of songs can't be read.
    static std::optional<Game> Open(std::shared_ptr<ntdlib::disc::GameFiles> disc);

    // The songs, in the order of the code's lists.
    const std::vector<GameSong>& Songs() const
    {
        return songs_;
    }

    const SongGroup& Group() const
    {
        return *group_;
    }

    // Read song `index`'s file. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadSong(std::size_t index) const;

private:
    // Read the song group whose first file has number `first_file` (PathOf): the group's pool, project, sample
    // directory and samples, one file after another. Throw FormatError if they can't be read or the project has no
    // group of songs.
    std::shared_ptr<const SongGroup> ReadSongGroup(uint32_t first_file) const;

    // Read the disc's file at `path`. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadFile(const std::string& path) const;

    std::shared_ptr<ntdlib::disc::GameFiles> disc_;
    std::shared_ptr<const SongGroup> group_;
    std::vector<GameSong> songs_;
};

} // namespace dolphinsf2::musyx
