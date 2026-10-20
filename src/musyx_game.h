// SPDX-License-Identifier: MIT

// The music of a GameCube game that plays it through Factor 5's MusyX: the songs that the game's code lists, and the
// song groups that play them. ntdlib's musyx module reads the files. dolphinsf2 recognizes a game by the SHA-1 of its
// main.dol. It converts two games by Camelot, Mario Party 4, Pokémon Colosseum, WarioWare, Inc.: Mega Party Game$! and
// the NDDEMO disc. Camelot names each file on its discs with three capital letters, a path of three levels (PathOf).
// One song group plays all of a Camelot game's songs. Mario Party 4 keeps its groups and its songs in the file of
// Hudson's sound manager (musyx_msm.h). There each song names its group. Pokémon Colosseum keeps its songs, and its
// song groups' projects, pools and sample directories, in FSYS archives (ntdlib/disc/fsys.h). There the code's table
// gives each song's group. WarioWare keeps its groups in one MusyX project and its songs in one file. Its sound room
// gives each song's group (ReadSoundRoom). The NDDEMO disc's code lists the paths of its songs' files (ReadSongPaths).
// One song group plays them all.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"

namespace dolphinsf2::musyx
{

// MusyX's volume table: a voice's amplitude at each volume from 0 to 128 in 127ths. MusyX plays a voice's volume
// through it unless the voice's macro picks the DLS curve (ModeSelect). The table lies in the game's code.
using VolumeTable = std::array<float, 129>;

// How a game's build of MusyX moves its sequencer's clock on in each run. The sequencer runs once a millisecond.
enum class TickStep : uint8_t
{
    // The tempo's ticks in a millisecond, worked out in single precision.
    kFloat,

    // The tempo's ticks in 65/65536 of a second, in whole 65536ths of a tick: floor(floor(tempo × 3072 / 60) × 65 / 8).
    // The early MusyX on the NDDEMO disc works them out so. Its songs play 0.8% slower than their tempo says.
    kInteger,
};

// A song that the game's code lists: its name, its file, the ID that names its MIDI setup, its volume and the game's
// master volume for music. MusyX multiplies a voice's volume by both before the volume curve. The song also holds how
// the game's sequencer counts time.
struct GameSong
{
    std::string name;            // the code's name for the song, without ".sng"
    std::string path;            // the song's file on the disc, or the file or archive that holds it
    uint16_t id = 0;             // the song's ID in the song group's MIDI setups
    uint8_t volume = 127;        // the code's volume for the song, in 127ths
    uint8_t master_volume = 127; // in 127ths
    std::size_t group = 0;       // the song group that plays the song: its index in Game::Groups
    TickStep tick_step = TickStep::kFloat;
};

// The paths that a table in `dol` lists: `count` words at `address`, each the address of a path. Throw FormatError if
// `dol` has no data where a word or a path should be.
std::vector<std::string> ReadSongPaths(const ntdlib::Dol& dol, uint32_t address, uint32_t count);

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

// A song that a game's table of sounds lists: its entry's index in the table, the MusyX ID of its song group, its song
// ID and the FSYS ID of its file.
struct TableSong
{
    uint32_t sound = 0;
    uint16_t group = 0;
    uint16_t id = 0;
    uint32_t file = 0;
};

// The songs that the table of `sound_count` sounds at offset `sounds` of `module` lists, in the table's order. The
// table of `file_count` files at offset `files` gives their files. A sound's entry has 12 bytes. Its first byte has bit
// 7 set for a sound effect. A song's entry gives its song group at +1, its file's number in the table of files at +3
// and its song ID in a big-endian half at +4. The byte at +2 picks the reverb that the game plays the song with. A
// file's entry has 8 bytes. Its first word is the file's FSYS ID. An ID of 0 names no file, and the list leaves out a
// song with such a file. `name` names the module in the errors. Throw FormatError if a table runs past the module's
// end, or if a song names a file past the table of files.
std::vector<TableSong> ReadSoundTable(std::span<const uint8_t> module, const std::string& name, uint32_t sounds,
                                      uint32_t sound_count, uint32_t files, uint32_t file_count);

// A song that a game's sound room lists: its name, its number, the ID of the song group that plays it, and its volume.
// The number is the song's ID in the group's MIDI setups and its place in the file of songs.
struct SoundRoomSong
{
    std::string name;
    uint16_t number = 0;
    uint16_t group_id = 0;
    uint8_t volume = 127; // in 127ths
};

// The songs that the sound room in `dol` lists: `count` entries of 0x20 bytes at `address`. An entry's first word
// holds the song group's ID in its top byte and the song's number in its low 16 bits. Byte 4 is the song's volume. The
// word at 0xc points at the song's English name. The game's text marks each space with the bytes 0x94 0xBC. Throw
// FormatError if `dol` has no data where an entry or a name should be.
std::vector<SoundRoomSong> ReadSoundRoom(const ntdlib::Dol& dol, uint32_t address, uint32_t count);

// The SNG file of song `number` in `songs`. That file holds `count` songs back to back. The table at `sizes` in `dol`
// gives each song's size in a word. The sizes include padding to 32 bytes. The file can end within the last song's
// padding. Throw FormatError if the table has no such song, or if the song starts past the file's end.
std::span<const uint8_t> SongIn(const ntdlib::Dol& dol, uint32_t sizes, uint32_t count, std::span<const uint8_t> songs,
                                uint16_t number);

// The song group that plays a game's music: the group of the project file, the pool, the sample directory and the
// samples.
struct SongGroup
{
    ntdlib::musyx::Group group;
    ntdlib::musyx::Pool pool;
    std::vector<ntdlib::musyx::SampleInfo> samples;
    std::vector<uint8_t> sample_data;
};

// What the code of a game with Hudson's sound manager says about its music (see musyx_game.cpp).
struct SoundManagerRules;

// What the code of a game that keeps its music in FSYS archives says about it (see musyx_game.cpp).
struct FsysRules;

// What the code of a game with Intelligent Systems' sound layer says about its music (see musyx_game.cpp).
struct SoundRoomRules;

// What the code of a game that lists the paths of its songs' files says about its music (see musyx_game.cpp).
struct PathTableRules;

// What the code of Nintendo Puzzle Collection says about its music (see musyx_game.cpp).
struct PuzzleRules;

class Game
{
public:
    // Open the MusyX game whose files `disc` holds, with its main.dol. Return nothing if the game isn't one that
    // dolphinsf2 converts. Throw FormatError if a song group or the code's list of songs can't be read.
    static std::optional<Game> Open(std::shared_ptr<ntdlib::disc::GameFiles> disc);

    // The songs, in the order of the code's lists.
    const std::vector<GameSong>& Songs() const
    {
        return songs_;
    }

    // The song groups. A song's `group` is an index of this list.
    const std::vector<std::shared_ptr<const SongGroup>>& Groups() const
    {
        return groups_;
    }

    // MusyX's volume table, from the game's code.
    const VolumeTable& Volumes() const
    {
        return volumes_;
    }

    // Read song `index`'s SNG file. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadSong(std::size_t index) const;

private:
    // Read MusyX's volume table at `address` in main.dol: 129 big-endian floats that rise from 0 to 1. Throw
    // FormatError if main.dol holds no such table there.
    void ReadVolumes(uint32_t address);

    // Read the song groups and the songs that the sound test of a game with Hudson's sound manager lists. Throw
    // FormatError if the files can't be read.
    void ReadSoundManager(const SoundManagerRules& rules);

    // Read the song groups and the songs that the code's table lists in a game whose music lies in FSYS archives.
    // Throw FormatError if the files or the tables can't be read.
    void ReadFsysGame(const FsysRules& rules);

    // Read the song groups and the songs that the sound room of a game with Intelligent Systems' sound layer lists.
    // Throw FormatError if the files or the code's tables can't be read.
    void ReadSoundRoomGame(const SoundRoomRules& rules);

    // Read the song group and the songs of a game whose code lists the paths of its songs' files. Throw FormatError if
    // the project's files or the code's table can't be read.
    void ReadPathTableGame(const PathTableRules& rules);

    // Read the song groups and the songs of Nintendo Puzzle Collection's modules (musyx_puzzle.h). Throw FormatError if
    // a module or a file of sound data can't be read.
    void ReadPuzzleGame(const PuzzleRules& rules);

    // Read the song group whose first file has number `first_file` (PathOf): the group's pool, project, sample
    // directory and samples, one file after another. Throw FormatError if they can't be read or the project has no
    // group of songs.
    std::shared_ptr<const SongGroup> ReadSongGroup(uint32_t first_file) const;

    // Read the disc's file at `path`. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadFile(const std::string& path) const;

    std::shared_ptr<ntdlib::disc::GameFiles> disc_;
    std::vector<std::shared_ptr<const SongGroup>> groups_;
    std::vector<GameSong> songs_;
    VolumeTable volumes_{};

    // The songs' SNG files when one file or an archive holds them. Empty for a game with a file for each song.
    std::vector<std::vector<uint8_t>> song_data_;
};

} // namespace dolphinsf2::musyx
