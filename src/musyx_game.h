// SPDX-License-Identifier: MIT

// The music and sound effects of a GameCube game that plays them through Factor 5's MusyX: the songs and effects that
// the game's code lists or starts, and the groups that play them (GameSong, GameEffect). ntdlib's musyx module reads
// the files. dolphinsf2 recognizes a game by the SHA-1 of its main.dol. It converts Camelot's Mario Golf: Toadstool
// Tour and Mario Power Tennis, Mario Party 4, and WarioWare, Inc.: Mega Party Game$!. Camelot names each file on its
// discs with three capital letters, a path of three levels (PathOf). One song group plays all of a Camelot game's
// songs. Mario Party 4 keeps its groups and its songs in the file of Hudson's sound manager (ntdlib/musyx/msm.h). There
// each song names its group. WarioWare keeps its groups in one MusyX project and its songs in one file. Its sound room
// gives each song's group (ReadSoundRoom). Thirteen more games keep their sounds in layouts of their own: Star Fox
// Adventures, Paper Mario: The Thousand-Year Door, Pokémon Channel, Pokémon Colosseum, the two Rogue Squadron games,
// Resident Evil, Resident Evil Zero, Tales of Symphonia, the two Metroid Prime games, Nintendo Puzzle Collection and
// the NDDEMO disc. A catalogue lists each one's sounds and the groups that each sound pushes (musyx_catalogue.h).

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

// A MIDI controller that the game's code sets on a song's channel once the song plays, through sndSeqCntrl.
struct SongControl
{
    uint8_t channel = 0;
    uint8_t controller = 0;
    uint8_t value = 0;
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

    // The time over which the game raises the song's volume from 0 to `volume` as it starts the song, in milliseconds.
    // MusyX's fader moves in a line, a step every 5 ms (sndSeqVolume).
    double fade_in_ms = 0.0;

    // The controllers that the game's code sets on the song's channels `controls_ms` milliseconds after the song's
    // start, in their order. Resident Evil's rooms set the levels and the pans of some songs' channels.
    std::vector<SongControl> controls = {};
    double controls_ms = 0.0;
};

// The highest value of the pitch wheel and the other 14-bit controllers of MusyX, and the centre of the wheel.
constexpr int kWheelHighest = 0x3fff;
constexpr int kWheelCentre = 0x2000;

// A sound effect that the game's code starts. MusyX plays an effect as one note of its row in its group's table of
// effects: the row's object at the row's key (ntdlib::musyx::Effect). The game passes a volume and a pan to
// sndFXStartEx. MusyX takes the row's volume or pan for a value of 0xFF. The volume is the note's velocity. The game
// can then set the effect's MIDI controllers (sndFXCtrl). MusyX multiplies the velocity, the effect's volume
// controller, the volume of the effect's volume group and the master volume for effects. MusyX starts every volume
// group at a volume of 0. An effect's start leaves the effect's group alone. The game's set-up gives the groups their
// volumes (sndVolume).
struct GameEffect
{
    std::string name;
    std::size_t group = 0; // the group whose pool and samples play the effect: its index in Game::Groups
    ntdlib::musyx::Effect row;
    std::optional<uint8_t> volume; // the volume that the game starts the effect at, or nothing or 0xFF for the row's
    std::optional<uint8_t> pan;    // the pan, 0 to 127, or nothing or 0xFF for the row's
    uint8_t group_volume = 127;    // the volume of the effect's volume group, in 127ths
    uint8_t master_volume = 127;   // the game's master volume for effects, in 127ths

    // The controllers that the game sets once the effect starts. The pan controller moves the effect's pan by its
    // distance from the centre. A channel's pan moves a note's pan the same way. The pitch wheel's value is 0x2000 at
    // the centre.
    uint8_t volume_controller = 127; // CC7
    uint8_t pan_controller = 64;     // CC10
    uint8_t reverb = 0;              // CC91, the send to aux A
    uint8_t chorus = 0;              // CC93, the send to aux B
    uint16_t pitch_wheel = kWheelCentre;
};

// The row of the effect `id` in `group`'s table of effects. Null if the table lacks it or its row allows no voices.
const ntdlib::musyx::Effect* FindEffect(const ntdlib::musyx::Group& group, uint16_t id);

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

// The songs that a game's table of the songs that play as effects lists: `count` entries of 8 bytes at `address` in
// `dol`. An entry holds the song group's ID in its first byte, the song's number in a big-endian half at +2 and the
// song's volume at +4. Each song takes "se-" and its number in four digits as its name, such as "se-0013". Throw
// FormatError if `dol` has no data where an entry should be.
std::vector<SoundRoomSong> ReadEffectSongs(const ntdlib::Dol& dol, uint32_t address, uint32_t count);

// An effect that a game's play function starts: the number that the function takes for it, and the effect's volume
// and its sends to the reverb (CC91) and the chorus (CC93).
struct PlayedEffect
{
    uint16_t number = 0;
    uint8_t volume = 127;
    uint8_t reverb = 0;
    uint8_t chorus = 0;
};

// The effects that a game's table of effects lists: `count` entries of 6 bytes at `address` in `dol`. An entry holds
// the play function's number in a big-endian half, the volume at +2, the send to the reverb at +4 and the send to the
// chorus at +5. Throw FormatError if `dol` has no data where an entry should be.
std::vector<PlayedEffect> ReadPlayedEffects(const ntdlib::Dol& dol, uint32_t address, uint32_t count);

// The SNG file of song `number` in `songs`. That file holds `count` songs back to back. The table at `sizes` in `dol`
// gives each song's size in a word. The sizes include padding to 32 bytes. The file can end within the last song's
// padding. Throw FormatError if the table has no such song, or if the song starts past the file's end.
std::span<const uint8_t> SongIn(const ntdlib::Dol& dol, uint32_t sizes, uint32_t count, std::span<const uint8_t> songs,
                                uint16_t number);

// A group of a game's sounds: a group of the project file, the pool, the sample directory and the samples. A song
// group plays a game's music. An effect plays from the objects of a group's pool and the group's samples
// (GameEffect).
struct SongGroup
{
    ntdlib::musyx::Group group;
    ntdlib::musyx::Pool pool;
    std::vector<ntdlib::musyx::SampleInfo> samples;
    std::vector<uint8_t> sample_data;

    // The number that the game's build of MusyX divides by in ScaleVolume. The step multiplies a voice's volume by
    // its scale, then divides the product by this number. The early MusyX on the NDDEMO disc shifts the product right
    // by 7 bits: it divides by 128.
    int volume_scale_divisor = 127;

    // Groups that the game has pushed with this one (AddPushedGroups). A voice plays a sample that this group lacks
    // from the first of them that holds it.
    std::vector<std::shared_ptr<const SongGroup>> pushed;
};

// The first group of `type` among `groups`, a project's groups, whose MusyX ID is `id`. Without `id`, the first group
// of that type. Null if `groups` has no such group.
const ntdlib::musyx::Group* FindGroup(const std::vector<ntdlib::musyx::Group>& groups, ntdlib::musyx::GroupType type,
                                      std::optional<uint16_t> id);

// Give `group` the samples of `samples` that it lacks. `data` holds their data. That data goes after the group's data
// if the group takes any of the samples.
void AddMissingSamples(SongGroup& group, const std::vector<ntdlib::musyx::SampleInfo>& samples,
                       std::span<const uint8_t> data);

// Give `pool` the objects of `other` that it doesn't have. An object of `pool` keeps its place over an object of
// `other` with the same ID.
void MergePool(ntdlib::musyx::Pool& pool, const ntdlib::musyx::Pool& other);

// Give `group` the objects of `pushed` that it lacks. Its voices then find the samples that it lacks in `pushed` too
// (SongGroup::pushed). MusyX finds a voice's objects and samples in every group that the game has pushed. The group's
// objects and samples come first, then those of each group of `pushed` in turn.
void AddPushedGroups(SongGroup& group, const std::vector<std::shared_ptr<const SongGroup>>& pushed);

// What the code of a game with Hudson's sound manager says about its music (see musyx_game.cpp).
struct SoundManagerRules;

// What the code of a game with Intelligent Systems' sound layer says about its music (see musyx_game.cpp).
struct SoundRoomRules;

// What the code of a game by Camelot says about its effects (see musyx_game.cpp).
struct CamelotEffectRules;

// What the code of a game with a catalogue of its sounds says about how it plays them (see musyx_catalogue.h).
struct CatalogueRules;

// A MusyX game: its songs, its effects, the groups that play them and its volume table. The game's code gives them.
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

    // The sound effects, in the order of the code's tables. They come after the songs.
    const std::vector<GameEffect>& Effects() const
    {
        return effects_;
    }

    // The groups of songs and of effects. A song's or an effect's `group` is an index of this list.
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

    // Read the song groups and the songs that the sound room of a game with Intelligent Systems' sound layer lists.
    // Throw FormatError if the files or the code's tables can't be read.
    void ReadSoundRoomGame(const SoundRoomRules& rules);

    // Read the song groups, the songs and the effects of a game with a catalogue of its sounds (musyx_catalogue.h).
    // Throw FormatError if a file is missing or damaged.
    void ReadCatalogueGame(const CatalogueRules& rules);

    // Read the group whose first file has number `first_file` (PathOf): the group's pool, project, sample directory
    // and samples, one file after another. The group is the project's first group of `type`, or the one whose ID is
    // `id`. Throw FormatError if the files can't be read or the project has no such group.
    std::shared_ptr<SongGroup> ReadGroup(uint32_t first_file, ntdlib::musyx::GroupType type,
                                         std::optional<uint16_t> id) const;

    // Read the effects that the tables of a game by Camelot list, and their groups. They follow the songs' group, in
    // the order of the effects' IDs. Throw FormatError if a table lies outside main.dol, or a group can't be read.
    void ReadCamelotEffects(const CamelotEffectRules& rules);

    // Read the disc's file at `path`. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadFile(const std::string& path) const;

    std::shared_ptr<ntdlib::disc::GameFiles> disc_;
    std::vector<std::shared_ptr<const SongGroup>> groups_;
    std::vector<GameSong> songs_;
    std::vector<GameEffect> effects_;
    VolumeTable volumes_{};

    // The songs' SNG files when one file or an archive holds them. Empty for a game with a file for each song.
    std::vector<std::vector<uint8_t>> song_data_;
};

} // namespace dolphinsf2::musyx
