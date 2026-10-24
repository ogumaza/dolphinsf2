// SPDX-License-Identifier: MIT

// Hudson's sound manager over MusyX, in Mario Party 4. The sound manager reads one file of sound data. That file holds
// every MusyX group and every song. A group's data holds its pool, its project, its sample directory and the SNG files
// of its songs. The group's samples lie in another part of the file. The sound manager's table of songs gives each
// song's group and volume. The game's sound test lists the songs by their numbers in that table. Each entry also gives
// the ID of the song's name in the game's messages.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace dolphinsf2::musyx
{

// A song of the sound manager's table: the MusyX group that plays it, the song's ID in that group's MIDI setups, its
// volume in 127ths, and its SNG file.
struct MsmSong
{
    uint16_t group_id = 0;
    uint16_t song_id = 0;
    uint8_t volume = 127;
    std::span<const uint8_t> data;
};

// An effect of the sound manager's table of effects: its number in the table, the MusyX group that holds it, its ID in
// that group's table of effects, and the sound manager's settings for it. msmSePlay passes the settings to MusyX as
// the effect's controllers: the volume (CC7), the pan (CC10), the sends (CC91 and CC93) and the pitch wheel's offset
// from its centre.
struct MsmEffect
{
    uint32_t number = 0;
    uint16_t group_id = 0;
    uint16_t id = 0;
    uint8_t volume = 127;
    uint8_t pan = 64;
    int16_t pitch = 0;
    uint8_t reverb = 0;
    uint8_t chorus = 0;
};

// The four parts of a group. The pool, the project and the sample directory each run from the part's start to the end
// of the group's data. Their readers find each part's end in the part.
struct MsmGroup
{
    std::span<const uint8_t> pool;
    std::span<const uint8_t> project;
    std::span<const uint8_t> directory;
    std::span<const uint8_t> samples;
};

// The sound manager's file, read in place. `file` must outlive the object.
class MsmFile
{
public:
    // Read the header of `file`. `path` is the file's path on the disc. The errors name it. Throw FormatError if the
    // file is too short to hold a header.
    MsmFile(std::span<const uint8_t> file, std::string path);

    // Song `number` of the table. Throw FormatError if the table has no such song or the song's data lies outside the
    // file.
    MsmSong Song(uint32_t number) const;

    // The group whose MusyX ID is `id`. Throw FormatError if the file has no such group or the group's parts lie
    // outside the file.
    MsmGroup Group(uint16_t id) const;

    // The effects of the table of effects, in its order. An entry whose ID is 0xffff holds no effect. The list leaves
    // it out. Throw FormatError if the table lies outside the file.
    std::vector<MsmEffect> Effects() const;

    // The base groups in the sound manager's order. The sound manager loads them first and keeps them loaded. Throw
    // FormatError if the settings' list of them names a group that the table lacks or a group's parts lie outside the
    // file.
    std::vector<MsmGroup> BaseGroups() const;

private:
    // A reader of the file. A read past the file's end throws FormatError with `cut_short_`.
    xsflib::Reader Reader() const;

    // The number of groups in the table.
    std::size_t GroupCount() const;

    // The parts of the table's group `index`. Throw FormatError if the table has no such group or the group's parts
    // lie outside the file.
    MsmGroup PartsOf(std::size_t index) const;

    // The offset in the file of the data of the table's group `index`. Throw FormatError if the table has no such
    // group.
    std::size_t GroupData(std::size_t index) const;

    std::span<const uint8_t> file_;
    std::string path_;
    std::string cut_short_;
};

// A song of the game's sound test: its number in the sound manager's table and its name.
struct SoundTestSong
{
    uint32_t number = 0;
    std::string name;
};

// The songs that the sound test lists. They keep the sound test's order. `module` is the file of the code module that
// holds the sound test: `count` entries from offset `offset`. `module_path` is that file's path on the disc. The
// game's message file `messages` holds the names. `messages_path` is its path. Throw FormatError if an entry or a name
// lies outside its file.
std::vector<SoundTestSong> ReadSoundTest(std::span<const uint8_t> module, const std::string& module_path,
                                         uint32_t offset, uint32_t count, std::span<const uint8_t> messages,
                                         const std::string& messages_path);

// The names of the effects that a list in the form of the sound test names, by the effects' numbers in the sound
// manager's table of effects. The game's list of voices is one. Of two entries for one effect, the later counts. The
// arguments are those of ReadSoundTest. Throw FormatError if an entry or a name lies outside its file.
std::map<uint32_t, std::string> ReadSoundTestEffects(std::span<const uint8_t> module, const std::string& module_path,
                                                     uint32_t offset, uint32_t count, std::span<const uint8_t> messages,
                                                     const std::string& messages_path);

} // namespace dolphinsf2::musyx
