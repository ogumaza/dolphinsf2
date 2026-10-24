// SPDX-License-Identifier: MIT

// Unit tests for dolphinsf2, without game data. The tests build small MusyX song groups, Yay0 data and modules of code
// in memory. ntdlib's tests cover the conversion of JAudio's sound data.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/dol.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "ntdlib/testing/dol_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/track_file.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "input.h"
#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"
#include "musyx_msm.h"
#include "musyx_player.h"
#include "musyx_voices.h"
#include "rel_module.h"
#include "yay0.h"

namespace
{

using namespace dolphinsf2;
using ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;
using xsflib::testing::failures;

// Count `perf`'s events of `kind` on MIDI track `slot`.
int Count(const Performance& perf, std::size_t slot, EventKind kind)
{
    int n = 0;
    for (const TrackEvent& e : perf.tracks[slot])
    {
        n += e.kind == kind ? 1 : 0;
    }

    return n;
}

// A DOL whose first data section holds `data` at load address `address`.
std::vector<uint8_t> DolWithData(uint32_t address, const std::vector<uint8_t>& data)
{
    constexpr std::size_t kHeader = 0x100;
    std::vector<uint8_t> dol(kHeader);
    ntdlib::testing::SetDolSection(dol, 7, static_cast<uint32_t>(kHeader), address, static_cast<uint32_t>(data.size()));
    dol.insert(dol.end(), data.begin(), data.end());

    return dol;
}

// The bytes of `text`, without a terminating zero.
std::vector<uint8_t> TextBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// A file's number in a song list is twice its index. The index counts in base 26, with the letters A to Z, through
// paths of three letters.
void TestMusyxPaths()
{
    constexpr uint32_t kFgh = 2 * ((5 * 26 + 6) * 26 + 7);
    Check(musyx::PathOf(kFgh) == "F/G/H" && musyx::PathOf(kFgh + 1) == "F/G/H",
          "the number's lowest bit names no other file");
    Check(musyx::PathOf(kFgh + 2) == "F/G/I" && musyx::PathOf(kFgh + 52) == "F/H/H", "the last letter counts first");
    Check(musyx::PathOf(kFgh + 6) == "F/G/K", "a group's fourth file is 6 past its first");
    Check(musyx::PathOf(2 * (26 * 26 * 26 - 1)) == "Z/Z/Z", "the last path");
}

// Song lists in both of the games' forms. The first list's entries of 16 bytes give the address of each song's name.
// The second list's entry of 0x24 bytes holds its name in its first 0x18 bytes. Each entry ends with the song's file
// number, its ID in the low half of a word, and its volume in the top byte of the last word.
void TestMusyxSongLists()
{
    constexpr uint32_t kBase = 0x80100000;
    BigEndianBytes b;
    b.U32(kBase + 0x40).U32(8112).U32(3).U32(90u << 24);
    b.U32(kBase + 0x50).U32(8115).U32(7).U32(127u << 24);
    b.PadTo(0x40).Append(TextBytes("opening.sng")).PadTo(0x50).Append(TextBytes("theme")).PadTo(0x60);
    b.Append(TextBytes("fanfare.sng")).PadTo(0x78).U32(8162).U32(0x10005).U32(0x40123456);
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<musyx::GameSong> by_address = musyx::ReadSongList(*dol, {kBase, 2, 0x10, false});
    const std::vector<musyx::GameSong> inline_names = musyx::ReadSongList(*dol, {kBase + 0x60, 1, 0x24, true});
    const std::string past_the_data = ErrorOf([&] { musyx::ReadSongList(*dol, {kBase + 0x100, 1, 0x10, false}); });

    if (Check(by_address.size() == 2, "the list of names by address has two songs"))
    {
        const musyx::GameSong& opening = by_address[0];
        const musyx::GameSong& theme = by_address[1];
        Check(opening.name == "opening" && opening.path == "G/A/A" && opening.id == 3 && opening.volume == 90,
              "the first song: its name without .sng, its file, its ID and its volume");
        Check(theme.name == "theme" && theme.path == "G/A/B" && theme.id == 7 && theme.volume == 127,
              "a name without .sng stays whole");
    }
    if (Check(inline_names.size() == 1, "the list of names in its entries has one song"))
    {
        const musyx::GameSong& fanfare = inline_names[0];
        Check(fanfare.name == "fanfare" && fanfare.path == "G/A/Z" && fanfare.id == 5 && fanfare.volume == 0x40,
              "the entry's name, and the low half of the ID's word and the top byte of the volume's");
    }
    Check(past_the_data == "main.dol has no data at the song list's address",
          "a list past the DOL's data fails: " + past_the_data);
}

// A sound manager's file with one group, two songs and two effects. The header gives the offsets and sizes of the
// tables of groups and songs. It also gives the offsets of the groups' data, the groups' samples, the songs that no
// group holds and the table of effects. The table of groups' first entry holds no group. Group 7's data starts with the
// offsets of its pool, its project, its sample directory and its songs. Song 0's SNG file lies at offset 2 of the
// group's songs and holds the bytes 1 to 4. Song 1's file lies at offset 1 of the songs that no group holds and holds
// the bytes 5 to 7. The group's samples lie at offset 1 of the groups' samples. They're the bytes 0xaa and 0xbb. The
// settings list one base group: the table's group 1. They give the table of effects three entries. The second holds no
// effect.
void TestSoundManagerFile()
{
    BigEndianBytes b;
    b.PadTo(0x60);
    b.Put32(0x20, 0x60);
    b.Put32(0x24, 0x40);
    b.PadTo(0x80);
    b.U16(7).U16(0).U32(0).U32(0x20).U32(1).U32(2).PadTo(0xa0);
    b.Put32(0x28, 0xa0);
    b.Put32(0x2c, 0x20);
    b.U16(7).U16(3).U32(2).U32(4).U8(0).U8(90).U16(0);
    b.U16(7).U16(4).U32(1).U32(3).U8(0xff).U8(127).U16(0);
    b.Put32(0x38, 0xc0);
    b.U32(0x10).U32(0x14).U32(0x18).U32(0x1a);
    b.U32(0x11111111).U32(0x22222222).U16(0x3333).U16(0).U32(0x01020304);
    b.Put32(0x40, 0xe0);
    b.U8(0).U8(0xaa).U8(0xbb);
    b.Put32(0x48, 0xe3);
    b.U8(0).U8(5).U8(6).U8(7);
    b.Put32(0x10, 0xe7);
    b.PadTo(0xe7 + 0x28).U8(1).U8(1);
    b.Put16(0xe7 + 6, 3);
    b.PadTo(0x120);
    b.Put32(0x30, 0x120);
    b.U16(7).U16(12).U8(100).U8(32).U16(0xfff0).U8(64).U8(20).U8(30).U8(0).U32(0);
    b.U16(7).U16(0xffff).PadTo(0x140);
    b.U16(7).U16(13).U8(127).U8(64).U16(0x10).U8(0).U8(0).U8(0).U8(0).U32(0);
    const std::vector<uint8_t>& file = b.Data();
    const musyx::MsmFile msm(file, "sound/test.msm");

    const musyx::MsmSong in_group = msm.Song(0);
    Check(in_group.group_id == 7 && in_group.song_id == 3 && in_group.volume == 90 &&
              std::vector<uint8_t>(in_group.data.begin(), in_group.data.end()) == std::vector<uint8_t>{1, 2, 3, 4},
          "a song in a group: its group, its ID, its volume and its file in the group's songs");
    const musyx::MsmSong outside = msm.Song(1);
    Check(outside.song_id == 4 && outside.volume == 127 &&
              std::vector<uint8_t>(outside.data.begin(), outside.data.end()) == std::vector<uint8_t>{5, 6, 7},
          "a song that no group holds: its file among those songs");
    Check(ErrorOf([&] { msm.Song(2); }) == "sound/test.msm has no song 2", "a song past the table fails");

    const musyx::MsmGroup group = msm.Group(7);
    Check(group.pool.size() == 0x10 && group.pool[0] == 0x11 && group.project.size() == 0xc &&
              group.project[0] == 0x22 && group.directory.size() == 8 && group.directory[0] == 0x33,
          "a group's parts run from their offsets to the end of the group's data");
    Check(std::vector<uint8_t>(group.samples.begin(), group.samples.end()) == std::vector<uint8_t>{0xaa, 0xbb},
          "a group's samples");
    Check(ErrorOf([&] { msm.Group(8); }) == "sound/test.msm has no group 8", "a group that the table lacks fails");
    const std::vector<musyx::MsmGroup> base = msm.BaseGroups();
    Check(base.size() == 1 && base[0].pool.data() == group.pool.data(), "the settings' base group");
    const std::vector<musyx::MsmEffect> effects = msm.Effects();
    if (Check(effects.size() == 2, "the table lists two effects"))
    {
        const musyx::MsmEffect& first = effects[0];
        Check(first.number == 0 && first.group_id == 7 && first.id == 12 && first.volume == 100 && first.pan == 32 &&
                  first.pitch == -16 && first.reverb == 20 && first.chorus == 30,
              "the first effect: its number, group, ID, volume, pan, pitch and sends");
        Check(effects[1].number == 2 && effects[1].id == 13 && effects[1].pitch == 16,
              "the entry without an effect keeps its number");
    }
    std::vector<uint8_t> wrong_base = file;
    wrong_base[0xe7 + 0x29] = 5;
    Check(ErrorOf([&] { musyx::MsmFile(wrong_base, "sound/test.msm").BaseGroups(); }) ==
              "sound/test.msm has no group at 5 in its table",
          "a base group that the table lacks fails");

    const std::vector<uint8_t> cut(file.begin(), file.begin() + 0x40);
    Check(
        ErrorOf([&] { musyx::MsmFile short_file(cut, "sound/test.msm"); }) == "sound/test.msm is cut short or damaged",
        "a file shorter than the header fails");
}

// A sound test of four entries: a song, an unused entry, a stream and a sound effect. Only the song is listed. Its name
// is message 1 of bank 47. The message file's table gives bank 47 slot 1. The word at 8 then gives that bank's offset.
// The message's control codes set a colour, make spaces and move to the next tab stop. Bytes 0x5c and 0xc2 draw an
// apostrophe and '!'.
void TestSoundTest()
{
    BigEndianBytes messages;
    messages.U32(1).U32(0x10).U32(0x20).U32(0).U16(47).U16(1).PadTo(0x20);
    messages.U32(2).U32(0xc).U32(0xd).U8(0);
    messages.U8(0x1e).U8(5).Append(TextBytes("Boo")).U8(0x5c).Append(TextBytes("s")).U8(0x10);
    messages.Append(TextBytes("Haunted")).U8(0x0a).Append(TextBytes("Bash")).U8(0xc2).U8(0x0c).U8(0);

    BigEndianBytes module;
    module.U32(0);
    module.U32(1).U32(47u << 16 | 1).U16(3).U16(80).U32(45);
    module.U32(0).U32(47u << 16 | 1).U16(3).U16(80).U32(46);
    module.U32(1).U32(47u << 16).U16(1).U16(0).U32(12);
    module.U32(1).U32(47u << 16).U16(28).U16(0).U32(100);
    BigEndianBytes missing;
    missing.U32(1).U32(47u << 16 | 2).U16(3).U16(80).U32(45);

    const std::vector<musyx::SoundTestSong> songs =
        musyx::ReadSoundTest(module.Data(), "dll/test.rel", 4, 4, messages.Data(), "mess/test.dat");
    Check(songs.size() == 1 && songs[0].number == 45 && songs[0].name == "Boo's Haunted Bash!",
          "the sound test lists its song by number, with its name from the messages");
    Check(
        ErrorOf([&] { musyx::ReadSoundTest(module.Data(), "dll/test.rel", 4, 5, messages.Data(), "mess/test.dat"); }) ==
            "dll/test.rel's sound test runs past the file's end",
        "an entry past the module's end fails");

    // A list of voices in the same form names effects by their numbers. Of two entries for one effect, the later
    // counts. An unused entry names nothing.
    BigEndianBytes voices;
    voices.U32(1).U32(47u << 16).U16(28).U16(0).U32(100);
    voices.U32(1).U32(47u << 16 | 1).U16(28).U16(0).U32(100);
    voices.U32(0).U32(47u << 16 | 1).U16(28).U16(0).U32(101);
    voices.U32(1).U32(47u << 16 | 1).U16(3).U16(80).U32(102);
    const std::map<uint32_t, std::string> named =
        musyx::ReadSoundTestEffects(voices.Data(), "dll/test.rel", 0, 4, messages.Data(), "mess/test.dat");
    Check(named.size() == 1 && named.contains(100) && named.at(100) == "Boo's Haunted Bash!",
          "the list names effect 100 by its later entry");
    const auto read_missing = [&]
    {
        musyx::ReadSoundTest(missing.Data(), "dll/test.rel", 0, 1, messages.Data(), "mess/test.dat");
    };
    Check(ErrorOf(read_missing) == "mess/test.dat's message 47-2 is missing", "a name that the bank lacks fails");
}

// A module's table of sounds lists songs and sound effects in entries of 12 bytes. A song's entry gives its song group,
// its reverb, its file's number in the table of files and its song ID. The table of files gives each file's FSYS ID.
void TestMusyxSoundTable()
{
    constexpr uint32_t kSounds = 0x10;
    constexpr uint32_t kFiles = 0x50;
    BigEndianBytes b;
    b.PadTo(kSounds);
    b.U8(0x00).U8(0).U8(3).U8(1).U16(0x0102).U16(0).U32(0); // a song
    b.U8(0x80).U8(2).U8(0).U8(9).U16(0x0007).U16(0).U32(0); // a sound effect, with a file past the table
    b.U8(0x00).U8(0).U8(1).U8(0).U16(0x0003).U16(0).U32(0); // a song whose file has the ID 0
    b.U8(0x00).U8(1).U8(5).U8(2).U16(0x004d).U16(0).U32(0); // a song of the second group
    b.PadTo(kFiles).U32(0).U32(0x6d4).U32(0x01a40800).U32(0x6bc).U32(0x00100800).U32(0x6a4);
    const std::vector<uint8_t> module = b.Data();

    const std::vector<musyx::TableSong> songs = musyx::ReadSoundTable(module, "test_rel", kSounds, 4, kFiles, 3);
    if (Check(songs.size() == 2, "the table lists two songs: no effect, and no song without a file"))
    {
        Check(songs[0].sound == 0 && songs[0].group == 0 && songs[0].id == 0x0102 && songs[0].file == 0x01a40800,
              "the first song: its entry, its group, its song ID and its file's FSYS ID");
        Check(songs[1].sound == 3 && songs[1].group == 1 && songs[1].id == 0x004d && songs[1].file == 0x00100800,
              "the song of the second group");
    }

    std::vector<uint8_t> past_the_files = module;
    past_the_files[kSounds + 3] = 3;
    const std::string no_file =
        ErrorOf([&] { musyx::ReadSoundTable(past_the_files, "test_rel", kSounds, 4, kFiles, 3); });
    Check(no_file == "test_rel's sound 0 names no file", "a song's file past the table of files fails: " + no_file);
    const std::string long_sounds = ErrorOf([&] { musyx::ReadSoundTable(module, "test_rel", kSounds, 8, kFiles, 3); });
    Check(long_sounds == "test_rel's table of sounds runs past the file's end",
          "a table of sounds past the module's end fails: " + long_sounds);
    const std::string long_files = ErrorOf([&] { musyx::ReadSoundTable(module, "test_rel", kSounds, 4, kFiles, 4); });
    Check(long_files == "test_rel's table of files runs past the file's end",
          "a table of files past the module's end fails: " + long_files);
}

// A sound room of two entries. An entry's first word holds the song group's ID in its top byte and the song's number in
// its low 16 bits. Byte 4 is the song's volume. The second of the six words from 8 on points at the song's English
// name. The other words point at an empty name. The names mark each space with the bytes 0x94 0xBC.
void TestSoundRoom()
{
    constexpr uint32_t kBase = 0x80200000;
    BigEndianBytes b;
    b.U32(7u << 24 | 2).U8(110).U8(5).U8(1).U8(0);
    b.U32(kBase + 0x7f).U32(kBase + 0x40).U32(kBase + 0x7f).U32(kBase + 0x7f).U32(kBase + 0x7f).U32(kBase + 0x7f);
    b.U32(1u << 24 | 0).U8(90).U8(10).U8(0).U8(0);
    b.U32(kBase + 0x7f).U32(kBase + 0x60).U32(kBase + 0x7f).U32(kBase + 0x7f).U32(kBase + 0x7f).U32(kBase + 0x7f);
    b.Append(TextBytes("Staff\x94\xbcTarget")).PadTo(0x60);
    b.Append(TextBytes("Fries\x94\xbcWith\x94\xbcThat?")).PadTo(0x80);
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<musyx::SoundRoomSong> songs = musyx::ReadSoundRoom(*dol, kBase, 2);
    const std::string past_the_data = ErrorOf([&] { musyx::ReadSoundRoom(*dol, kBase + 0x80, 1); });

    if (Check(songs.size() == 2, "the sound room lists two songs"))
    {
        Check(
            songs[0].name == "Staff Target" && songs[0].number == 2 && songs[0].group_id == 7 && songs[0].volume == 110,
            "the first song: its English name, its number, its group and its volume");
        Check(songs[1].name == "Fries With That?" && songs[1].number == 0 && songs[1].group_id == 1 &&
                  songs[1].volume == 90,
              "the second song, with two spaces in its name");
    }
    Check(past_the_data == "main.dol has no data at the song list's address",
          "a sound room past the DOL's data fails: " + past_the_data);
}

// A table of two words, each the address of a song's path. The paths follow the table.
void TestSongPaths()
{
    constexpr uint32_t kBase = 0x80220000;
    BigEndianBytes b;
    b.U32(kBase + 0x10).U32(kBase + 0x20).PadTo(0x10);
    b.Append(TextBytes("audio/title.sng")).PadTo(0x20);
    b.Append(TextBytes("audio/staff.sng")).PadTo(0x30);
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<std::string> paths = musyx::ReadSongPaths(*dol, kBase, 2);
    const std::string past_the_data = ErrorOf([&] { musyx::ReadSongPaths(*dol, kBase + 0x30, 1); });

    Check(paths == std::vector<std::string>{"audio/title.sng", "audio/staff.sng"}, "the table gives both paths");
    Check(past_the_data == "main.dol has no data at the song list's address",
          "a table past the DOL's data fails: " + past_the_data);
}

// Three songs back to back in a file of songs: 0x20, 0x40 and 0x20 bytes, as the table of sizes in the DOL gives them.
// The file ends 8 bytes into the last song's padding.
void TestSongsInOneFile()
{
    constexpr uint32_t kSizes = 0x80210000;
    BigEndianBytes sizes;
    sizes.U32(0x20).U32(0x40).U32(0x20);
    const std::vector<uint8_t> file = DolWithData(kSizes, sizes.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    std::vector<uint8_t> songs(0x20, 1);
    songs.insert(songs.end(), 0x40, 2);
    songs.insert(songs.end(), 8, 3);
    const std::vector<uint8_t> short_file(songs.begin(), songs.begin() + 0x60);

    const std::span<const uint8_t> second = musyx::SongIn(*dol, kSizes, 3, songs, 1);
    const std::span<const uint8_t> last = musyx::SongIn(*dol, kSizes, 3, songs, 2);

    Check(second.size() == 0x40 && second.data() == songs.data() + 0x20,
          "the second song follows the first and takes its size from the table");
    Check(last.size() == 8 && last[0] == 3, "the file's end cuts the last song's padding");
    Check(ErrorOf([&] { musyx::SongIn(*dol, kSizes, 3, songs, 3); }) == "the table of song sizes has no song 3",
          "a song past the table fails");
    Check(ErrorOf([&] { musyx::SongIn(*dol, kSizes, 3, short_file, 2); }) ==
              "song 2 starts past the end of the file of songs",
          "a song past the file's end fails");
}

// Yay0 data that unpacks to 30 bytes: three bytes, a reference 3 back for 6, a byte, then a reference 1 back for 20
// with the long form of its length. The flags give the six steps from the first word's top bit down.
void TestYay0()
{
    BigEndianBytes b;
    b.Tag("Yay0").U32(30).U32(20).U32(24);
    b.U32(0xe8000000);
    b.U16(0x4002).U16(0x0000);
    b.U8('A').U8('B').U8('C').U8('X').U8(20 - 18);
    std::vector<uint8_t> before_start = b.Data();
    before_start[20] = 0x40; // a reference 6 back after 3 bytes
    before_start[21] = 0x05;
    const std::vector<uint8_t> cut_short(b.Data().begin(), b.Data().end() - 1);

    const std::vector<uint8_t> unpacked = UnpackYay0(b.Data());

    Check(std::string(unpacked.begin(), unpacked.end()) == "ABCABCABCX" + std::string(20, 'X'),
          "the bytes and both forms of reference unpack");
    Check(IsYay0(b.Data()) && !IsYay0(TextBytes("Yaz0")), "Yay0 data starts with Yay0");
    Check(ErrorOf([&] { UnpackYay0(before_start); }) == "the Yay0 data refers to bytes before its start",
          "a reference before the start fails");
    Check(ErrorOf([&] { UnpackYay0(cut_short); }) == "the Yay0 data is damaged", "a table cut short fails");
    Check(ErrorOf([] { UnpackYay0(TextBytes("Yaz0 and more bytes")); }) == "not Yay0 data", "other data fails");
}

// A module with 3 sections: none, code, and data of 0x20 bytes. Its relocations against itself set the data's first two
// words to the data's offsets 0x10 and 0x18. The table of the two words then runs up to 0x10, the first target.
void TestRelModule()
{
    BigEndianBytes b;
    b.U32(5).U32(0).U32(0).U32(3).U32(0x40).PadTo(0x28).U32(0x88).U32(8).PadTo(0x40);
    b.U32(0).U32(0).U32(0x60 | 1).U32(8).U32(0x68).U32(0x20).PadTo(0x68);
    b.U32(0).U32(0).U32(0x12345678).U32(0).U32(1).U32(2).U32(3).U32(4);
    b.U32(5).U32(0x90);
    b.U16(0).U8(202).U8(2).U32(0);
    b.U16(0).U8(1).U8(2).U32(0x10);
    b.U16(4).U8(1).U8(2).U32(0x18);
    b.U16(0).U8(203).U8(0).U32(0);
    const std::vector<uint8_t> file = b.Data();

    const RelModule module(file, "test.rel");

    const std::size_t table = module.At(2, 0);
    const std::vector<std::pair<uint16_t, std::size_t>> pointers = module.TablePointers(table);
    Check(table == 0x68, "section 2 starts at 0x68");
    Check(pointers == std::vector<std::pair<uint16_t, std::size_t>>{{0, 0x78}, {1, 0x80}},
          "the table's two words point at the two objects");
    Check(module.Object(0x78).size() == 8 && module.Object(0x80).size() == 8,
          "an object runs up to the next target or to the section's end");
    Check(!module.Pointer(table + 8).has_value(), "a word that no relocation sets points nowhere");
    Check(ErrorOf([&] { module.At(2, 0x20); }) == "test.rel's section 2 has no data at offset 32",
          "a place past a section fails");
    Check(ErrorOf([&] { module.At(0, 0); }) == "test.rel's section 0 has no data at offset 0",
          "a section without data fails");
}

// MusyX turns a DLS envelope's timecents into whole milliseconds, and keeps their low 16 bits. The table's lowest
// value stands for no time.
void TestDlsMilliseconds()
{
    Check(musyx::DlsMilliseconds(0.0) == 1000.0 && musyx::DlsMilliseconds(1200.0) == 2000.0 &&
              musyx::DlsMilliseconds(-1200.0) == 500.0,
          "1200 timecents double the time");
    Check(musyx::DlsMilliseconds(100.0) == 1059.0, "the time is truncated to whole milliseconds");
    Check(musyx::DlsMilliseconds(-1.0e9) == 0.0, "the lowest value is no time");
    Check(musyx::DlsMilliseconds(7200.0) == 64000.0 && musyx::DlsMilliseconds(8400.0) == 128000.0 - 65536.0,
          "a time keeps its low 16 bits");
    Check(musyx::DlsMilliseconds(38400.0) == 65535.0, "a time past 31 bits saturates before it's cut to 16 bits");
}

// A SoundMacro step with command `op` and bytes 1 to 7 of its two words.
ntdlib::musyx::MacroStep Step(ntdlib::musyx::MacroOp op, std::array<uint32_t, 7> b = {})
{
    ntdlib::musyx::MacroStep step;
    step.word0 = static_cast<uint32_t>(op) | b[0] << 8 | b[1] << 16 | b[2] << 24;
    step.word1 = b[3] | b[4] << 8 | b[5] << 16 | b[6] << 24;

    return step;
}

// PianoPan pans a voice by its key: the centre pan at the centre key, and the scale in 128ths of a step for each key
// above it. The pan takes the key that the macro has set up when it reaches the step. The pan stays within 0 to 127.
void TestPianoPan()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kAddKey, {12}), Step(MacroOp::kPianoPan, {100, 60, 54}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kPianoPan, {0x80, 60, 64}), Step(MacroOp::kSetKey, {72}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kSetKey, {72}), Step(MacroOp::kPianoPan, {100, 60, 54}), Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kEnd)};

    const musyx::MacroVoice shifted = musyx::FollowMacro(pool, 1, 127);
    Check(shifted.piano_pan && shifted.piano_pan->scale == 100 && shifted.piano_pan->centre_key == 60 &&
              shifted.piano_pan->centre_pan == 54,
          "PianoPan takes its scale, centre key and centre pan from bytes 1 to 3");
    Check(musyx::PianoPanAt(*shifted.piano_pan, 60) == 63.375 && musyx::PianoPanAt(*shifted.piano_pan, 0) == 16.5,
          "an AddKey before the step moves the key that the pan follows");

    const musyx::MacroVoice wide = musyx::FollowMacro(pool, 2, 127);
    Check(wide.piano_pan && !wide.piano_pan->fixed_key, "a SetKey after the step leaves the key of the pan alone");
    Check(wide.piano_pan && musyx::PianoPanAt(*wide.piano_pan, 0) == 124.0,
          "a scale of -128 moves the pan one step left for each key");
    Check(wide.piano_pan && musyx::PianoPanAt(*wide.piano_pan, 127) == 0.0, "the pan stops at 0");

    const musyx::MacroVoice fixed = musyx::FollowMacro(pool, 3, 127);
    Check(fixed.piano_pan && musyx::PianoPanAt(*fixed.piano_pan, 0) == 63.375,
          "a SetKey before the step fixes the key that the pan follows");
    Check(!musyx::FollowMacro(pool, 4, 127).piano_pan, "a macro without the step keeps the pan of its keymap or layer");
}

// ScaleVolume multiplies the note's volume, or the voice's, by its scale over 127. The early MusyX on the NDDEMO disc
// divides by 128. The step then adds byte 2 in 127ths of full volume. MusyX plays a volume above full at full volume.
// Bytes 3 and 4 name a curve. 0xffff names none, and MusyX skips a curve that the pool lacks.
void TestScaleVolume()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kScaleVolume, {90, 10, 0xff, 0xff, 1}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kScaleVolume, {64, 20, 0xff, 0xff, 1}),
                      Step(MacroOp::kScaleVolume, {64, 0, 0xff, 0xff, 0}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kScaleVolume, {127, 50, 0xff, 0xff, 1}), Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kScaleVolume, {127, 0, 5, 0, 1}), Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kScaleVolume, {127, 0, 6, 0, 1}), Step(MacroOp::kEnd)};
    pool.tables[5] = {0, 0x7f};

    const musyx::MacroVoice later = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice early = musyx::FollowMacro(pool, 1, 128);
    const musyx::MacroVoice twice = musyx::FollowMacro(pool, 2, 128);
    const musyx::MacroVoice loud = musyx::FollowMacro(pool, 3, 128);

    Check(later.volume == 90.0 / 127.0 && later.volume_add == 10.0 / 127.0, "the scale divides by 127");
    Check(early.volume == 90.0 / 128.0 && early.volume_add == 10.0 / 127.0, "the early build's scale divides by 128");
    Check(std::abs(musyx::VoiceVolume(early, 0.5, 1.0) - (0.5 * 90.0 / 128.0 + 10.0 / 127.0)) < 1e-12,
          "the voice's volume is the scaled note's volume plus the addition");
    Check(twice.volume == 0.25 && std::abs(twice.volume_add - 10.0 / 127.0) < 1e-12,
          "a step on the voice's volume scales the addition before it");
    Check(musyx::VoiceVolume(loud, 1.0, 1.0) == 1.0 && musyx::VoiceVolume(loud, 0.25, 1.0) < 1.0,
          "a volume above full plays at full volume");
    Check(later.approximations.empty() && musyx::FollowMacro(pool, 5, 127).approximations.empty(),
          "a step that names no curve, or one that the pool lacks, reports nothing");
    const std::vector<std::string> curved = musyx::FollowMacro(pool, 4, 127).approximations;
    Check(curved == std::vector<std::string>{"volume curves left out"}, "a curve in the pool is reported");
}

// The steps of a fade after the note's release, as the NDDEMO disc's macros make it: wait for the release, ramp the
// volume to 80/128 of itself in 120 ms, ramp it to 0 in 800 ms, then stop the sample.
std::vector<ntdlib::musyx::MacroStep> FadeSteps()
{
    using ntdlib::musyx::MacroOp;
    return {
        Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
        Step(MacroOp::kEnvelope, {80, 0, 0xff, 0xff, 1, 120, 0}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 120, 0}),
        Step(MacroOp::kEnvelope, {0, 0, 0xff, 0xff, 1, 0x20, 0x03}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0x20, 0x03}),
        Step(MacroOp::kStopSample),
        Step(MacroOp::kEnd),
    };
}

// A voice that plays on after its note's release follows its macro's fade (KeyOff::kFade). Each Envelope step ramps
// the volume in a line, from where it is to its scale in 128ths times that. The fade ends at the StopSample, 920 ms
// after the release. A voice that stops by itself at the same time whether the note is released or not ignores the
// release (KeyOff::kAsHeld). Its fade ramps while the note is held, from 100 ms after the note's start, and leaves it
// silent at the stop.
void TestReleaseFade()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kStartSample, {1})};
    const std::vector<ntdlib::musyx::MacroStep> fade = FadeSteps();
    pool.macros[1].insert(pool.macros[1].end(), fade.begin(), fade.end());
    pool.macros[2] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0})};
    pool.macros[2].insert(pool.macros[2].end(), fade.begin() + 1, fade.end());

    const musyx::MacroVoice faded = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice own = musyx::FollowMacro(pool, 2, 127);

    const musyx::ReleaseFade& f = faded.release_fade;
    Check(faded.key_off == musyx::KeyOff::kFade && f.stop_ms == 920.0 && f.ramps.size() == 2,
          "the voice fades for 920 ms after its release, in two ramps");
    Check(f.ramps.size() == 2 && f.ramps[0].start_ms == 0.0 && f.ramps[0].ms == 120.0 && f.ramps[0].scale == 0.625 &&
              f.ramps[1].start_ms == 120.0 && f.ramps[1].ms == 800.0 && f.ramps[1].scale == 0.0,
          "the ramps start at the release and 120 ms later");
    Check(musyx::FadeVolume(f, 1.0, 60.0) == 0.8125 && musyx::FadeVolume(f, 1.0, 120.0) == 0.625 &&
              musyx::FadeVolume(f, 1.0, 520.0) == 0.3125 && !musyx::FadeVolume(f, 1.0, 920.0),
          "each ramp moves the volume in a line from where it is, and the voice stops at the end");
    Check(faded.approximations.empty() && faded.held_ramps.empty(), "a fade after the release reports nothing");
    Check(own.key_off == musyx::KeyOff::kAsHeld && own.stop_ms == 1020.0,
          "a voice that stops by itself ignores the release");
    Check(own.held_ramps.size() == 2 && own.held_ramps[0].start_ms == 100.0 && own.held_ramps[1].start_ms == 220.0,
          "its fade ramps while the note is held");
    Check(musyx::HeldSilenceMs(own) == 1020.0 && musyx::FadesThroughRelease(own) && own.approximations.empty(),
          "it falls silent at its stop, goes on with its fade through the release and reports nothing");
}

// A held note's fade goes on through the release of a voice that ignores the release: macro 1 waits for the release
// after its fade, then ends. A fade to silence leaves the voice silent at its end (HeldSilenceMs). A fade to 64/128 of
// the volume leaves it sounding. Ramps after the voice stops change nothing.
void TestHeldFade()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    const auto fade_to = [](uint32_t scale)
    {
        return std::vector<ntdlib::musyx::MacroStep>{
            Step(MacroOp::kStartSample, {1}),
            Step(MacroOp::kEnvelope, {scale, 0, 0xff, 0xff, 1, 0xe8, 0x03}),
            Step(MacroOp::kWaitTicks, {1, 0, 0, 0, 1, 0xff, 0xff}),
            Step(MacroOp::kEnd),
        };
    };
    pool.macros[1] = fade_to(0);
    pool.macros[2] = fade_to(64);
    pool.macros[3] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xf4, 0x01}),
                      Step(MacroOp::kStopSample), Step(MacroOp::kEnvelope, {0, 0, 0xff, 0xff, 1, 0xe8, 0x03}),
                      Step(MacroOp::kEnd)};

    const musyx::MacroVoice silent = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice half = musyx::FollowMacro(pool, 2, 127);
    const musyx::MacroVoice stopped = musyx::FollowMacro(pool, 3, 127);

    Check(silent.key_off == musyx::KeyOff::kIgnored && silent.held_ramps.size() == 1 &&
              silent.held_ramps[0].start_ms == 0.0 && silent.held_ramps[0].ms == 1000.0,
          "the voice ignores the release and fades over 1000 ms from the note's start");
    Check(musyx::HeldSilenceMs(silent) == 1000.0 && musyx::FadesThroughRelease(silent),
          "a fade to silence ends at 1000 ms and goes on through the release");
    Check(musyx::RampedVolume(silent.held_ramps, 1.0, 250.0) == 0.75 &&
              musyx::RampedVolume(silent.held_ramps, 1.0, 2000.0) == 0.0,
          "the volume falls in a line");
    Check(!musyx::HeldSilenceMs(half) && !musyx::FadesThroughRelease(half),
          "a fade to 64/128 leaves the voice sounding");
    Check(silent.approximations.empty() && half.approximations.empty(), "the fades report nothing");
    Check(stopped.held_ramps.empty() && stopped.stop_ms == 500.0, "a ramp after the stop is left out");
}

// A song group with one program. Program 0 plays macro 1. The macro picks the DLS volume curve without ITD, sets DLS
// envelope 2, starts sample 1, waits for the note's release and releases the envelope. Envelope 2 starts at full level
// at once, holds it and releases over 100 ms. Sample 1 is a PCM16 sine of 3200 samples at 32000 Hz, at key 60, that
// loops from sample 800 to its end. Song 5's MIDI setup gives channel 0 a volume of 100.
musyx::SongGroup TestSongGroup()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group;
    group.group.pages.push_back({.object = 1, .priority = 0, .max_voices = 8, .program = 0});
    ntdlib::musyx::MidiSetup setup;
    setup.song = 5;
    setup.channels[0].volume = 100;
    group.group.midi_setups.push_back(setup);

    // The macro, and its envelope's table. The table is little-endian. Its attack and decay of INT32_MIN take no time,
    // its sustain of 0x1000 is full level, its release takes 100 ms, and its scales of INT32_MIN are off.
    group.pool.macros[1] = {
        Step(MacroOp::kModeSelect, {1, 1}),
        Step(MacroOp::kSetAdsr, {2, 0, 1}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
        Step(MacroOp::kKeyOff),
        Step(MacroOp::kEnd),
    };
    group.pool.tables[2] = {0, 0, 0, 0x80, 0, 0, 0, 0x80, 0x00, 0x10, 100, 0, 0, 0, 0, 0x80, 0, 0, 0, 0x80};

    ntdlib::musyx::SampleInfo sample;
    sample.id = 1;
    sample.key = 60;
    sample.rate = 32000;
    sample.format = ntdlib::musyx::SampleFormat::kPcm16;
    sample.length = 3200;
    sample.loop_start = 800;
    sample.loop_length = 2400;
    group.samples.push_back(sample);
    BigEndianBytes data;
    for (uint32_t i = 0; i < sample.length; i++)
    {
        data.U16(static_cast<uint16_t>(std::lround(8000.0 * std::sin(i * 2.0 * std::numbers::pi / 80.0))));
    }
    group.sample_data = data.Data();

    return group;
}

// A volume table for the tests. Its entries rise from 0 to 1. The table isn't a power of the volume: the level of a
// product isn't the product of the levels.
musyx::VolumeTable TestVolumes()
{
    musyx::VolumeTable table{};
    for (std::size_t i = 0; i < 128; i++)
    {
        const double x = static_cast<double>(i) / 127.0;
        table[i] = static_cast<float>(x * x * (0.5 + 0.5 * x));
    }
    table[128] = 1.0f;

    return table;
}

// A song of one track on channel 0. The track plays pattern 0 at tick 0 with a volume of 100, then loops back to it or
// ends at tick 768. The pattern plays key 60 for half a beat. With `volume_change`, it then sets the channel's volume
// to 64.
ntdlib::musyx::Song TestSong(bool loops, bool volume_change)
{
    ntdlib::musyx::Song song;
    song.tempo = 120;
    ntdlib::musyx::Track track;
    track.entries.push_back({.tick = 0, .pattern = 0, .volume = 100});
    track.entries.push_back(
        {.tick = 768, .pattern = loops ? ntdlib::musyx::kLoopBack : ntdlib::musyx::kEndOfTrack, .loop_target = 0});
    song.tracks.push_back(track);

    ntdlib::musyx::Pattern pattern;
    pattern.events.push_back(
        {.tick = 0, .kind = ntdlib::musyx::PatternEventKind::kNote, .key = 60, .velocity = 100, .length = 192});
    if (volume_change)
    {
        pattern.events.push_back(
            {.tick = 192, .kind = ntdlib::musyx::PatternEventKind::kController, .controller = 7, .value = 64});
    }
    pattern.end_tick = 768;
    song.patterns.push_back(pattern);

    return song;
}

// The values of the CC7 events of channel 0.
std::vector<int> VolumesOf(const Performance& perf)
{
    std::vector<int> volumes;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volumes.push_back(e.value);
        }
    }

    return volumes;
}

// The game's song table gives each song a volume. MusyX multiplies the channels' volumes by it inside the volume curve.
// On the DLS curve, each CC7 then takes the song's volume in 127ths: the MIDI setup's 100, the track entry's 100 and
// the pattern's 64. A game's master volume multiplies them too.
void TestMusyxSongVolume()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group, TestVolumes());
    const ntdlib::musyx::Song song = TestSong(false, true);

    const Performance full = musyx::PlaySong(song, {"full", "G/A/A", 5, 127}, instruments, {});
    const Performance quieter = musyx::PlaySong(song, {"quieter", "G/A/A", 5, 90}, instruments, {});
    const Performance lower = musyx::PlaySong(song, {"lower", "G/A/A", 5, 127, 101}, instruments, {});
    const Performance both = musyx::PlaySong(song, {"both", "G/A/A", 5, 90, 101}, instruments, {});

    Check(VolumesOf(full) == std::vector<int>{100, 64}, "a song at 127 keeps its channels' volumes");
    Check(VolumesOf(quieter) == std::vector<int>{71, 45}, "a song at 90 scales them by 90/127, rounded");
    Check(VolumesOf(lower) == std::vector<int>{80, 51}, "a master volume of 101 scales them by 101/127");
    Check(VolumesOf(both) == std::vector<int>{56, 36}, "the song's volume and the master volume multiply");
}

// The velocities of the note-ons of channel 0.
std::vector<int> VelocitiesOf(const Performance& perf)
{
    std::vector<int> velocities;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            velocities.push_back(e.value);
        }
    }

    return velocities;
}

// The velocity whose square times the squares of CC7 `volume` and CC11 127 gives the table's level of `velocity` times
// the volume that CC7 stands for, on the table or on the DLS curve.
double TableVelocity(int velocity, int volume, bool on_table)
{
    const musyx::VolumeTable table = TestVolumes();
    const double written = volume / 127.0;
    const double channel = on_table ? musyx::TableVolume(table, written * written) : written;
    return 127.0 * std::sqrt(musyx::TableLevel(table, velocity / 127.0 * channel)) / written;
}

// The decibels by which velocity `velocity` and CC7 `volume` miss the table's level of `product`.
double TableMiss(int velocity, int volume, double product)
{
    const double level = velocity / 127.0 * volume / 127.0;
    return 20.0 * std::log10(level * level / musyx::TableLevel(TestVolumes(), product));
}

// MusyX's volume table gives a voice's level at a volume in 127ths. MusyX interpolates between the entries.
void TestMusyxVolumeTable()
{
    const musyx::VolumeTable table = TestVolumes();
    musyx::SongGroup group = TestSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    group.pool.macros[2][0] = Step(ntdlib::musyx::MacroOp::kModeSelect, {0, 1});
    musyx::Instruments instruments(group, table);

    Check(musyx::TableLevel(table, 0.0) == 0.0 && musyx::TableLevel(table, 1.0) == 1.0, "the table runs from 0 to 1");
    Check(std::abs(musyx::TableLevel(table, 63.5 / 127.0) - (table[63] + table[64]) / 2.0) < 1e-9,
          "a volume between two entries takes the line between them");
    Check(musyx::TableLevel(table, 1.5) == 1.0, "a volume above full plays at full level");
    Check(std::abs(musyx::TableVolume(table, musyx::TableLevel(table, 0.3)) - 0.3) < 1e-6 &&
              musyx::TableVolume(table, 0.0) == 0.0 && musyx::TableVolume(table, 1.0) == 1.0,
          "TableVolume inverts the table");
    Check(instruments.Level(instruments.Macro(1), 0.5) == 0.25, "a voice on the DLS curve squares its volume");
    Check(instruments.Level(instruments.Macro(2), 0.5) == musyx::TableLevel(table, 0.5),
          "a voice without the DLS curve plays on the table");
}

// A channel of voices on MusyX's volume table takes CC7 values whose squares follow the table. The song's volume and
// the master volume of 100 multiply the channel's volume. Within CC7's steps, the note's velocity then gives the note
// the table's level of the whole product. A channel with notes on both curves keeps the DLS curve's CC7. Only the
// channel's notes on the table take such a velocity. The pattern's second note plays program 1 there.
void TestMusyxTableLevels()
{
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = TestSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    group.pool.macros[2][0] = Step(ntdlib::musyx::MacroOp::kModeSelect, {0, 1});
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::SongGroup table_group = group;
    table_group.group.pages[0].object = 2;
    musyx::Instruments table_instruments(table_group, TestVolumes());
    musyx::Instruments mixed_instruments(group, TestVolumes());
    const ntdlib::musyx::Song song = TestSong(false, true);
    ntdlib::musyx::Song mixed_song = TestSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = mixed_song.patterns[0].events;
    events.push_back({.tick = 240, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 288, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});

    const Performance table = musyx::PlaySong(song, {"table", "G/A/A", 5, 127, 100}, table_instruments, {});
    const Performance mixed = musyx::PlaySong(mixed_song, {"mixed", "G/A/A", 5, 127, 100}, mixed_instruments, {});

    constexpr double kChannel = 100.0 / 127.0 * 100.0 / 127.0;
    const auto written = [](double volume)
    {
        return static_cast<int>(std::lround(127.0 * std::sqrt(musyx::TableLevel(TestVolumes(), volume))));
    };
    const std::vector<int> volumes = VolumesOf(table);
    Check(volumes == std::vector<int>{written(kChannel), written(64.0 / 127.0 * 100.0 / 127.0)},
          "CC7 takes the square root of the table's level of the channel's volume times the master volume");
    const std::vector<int> velocities = VelocitiesOf(table);
    Check(velocities.size() == 1 && !volumes.empty() &&
              std::abs(velocities[0] - TableVelocity(100, volumes[0], true)) <= 0.5,
          "the velocity gives the note the table's level of the velocity times the volume that CC7 stands for");
    Check(velocities.size() == 1 && !volumes.empty() &&
              std::abs(TableMiss(velocities[0], volumes[0], 100.0 / 127.0 * kChannel)) < 0.15,
          "the note plays within 0.15 dB of the table's level of the whole product");
    Check(table.approximations.empty(), "a channel on the table reports no approximation");

    const std::vector<int> mixed_volumes = VolumesOf(mixed);
    const std::vector<int> mixed_velocities = VelocitiesOf(mixed);
    Check(mixed_volumes == std::vector<int>{static_cast<int>(std::lround(127.0 * kChannel))},
          "a channel with both curves keeps the DLS curve's CC7");
    Check(mixed_velocities.size() == 2 && mixed_velocities[0] == 100, "the note on the DLS curve keeps its velocity");
    Check(mixed_velocities.size() == 2 && !mixed_volumes.empty() &&
              std::abs(mixed_velocities[1] - TableVelocity(100, mixed_volumes[0], false)) <= 0.5,
          "the note on the table takes the table's level");
    const auto counted = mixed.approximations.find(
        "notes on MusyX's volume table follow volume changes on the DLS curve in channels with both curves");
    Check(counted != mixed.approximations.end() && counted->second == 1, "the report counts the note on the table");
}

// A song that loops plays at the output's rate, marks its loop and converts to files that independent readers accept.
void TestMusyxSong()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group, TestVolumes());

    const Conversion c = musyx::ConvertSong(TestSong(true, false), {"loop", "G/A/A", 5, 127}, instruments, {});

    const Performance& perf = c.performance;
    constexpr double kBpm = 120.0 * musyx::kOutputRate / musyx::kNominalRate;
    Check(!perf.tempo.empty() && std::abs(perf.tempo[0].bpm - kBpm) < 0.01,
          "the song plays 0.089% fast, at the output's rate: " +
              (perf.tempo.empty() ? std::string("no tempo") : std::to_string(perf.tempo[0].bpm)));
    Check(perf.loop && perf.loop->start == 0 && perf.loop->end == 768 && perf.end_tick == 768,
          "the loop runs from tick 0 to 768");
    Check(Count(perf, 0, EventKind::kNoteOn) == 1 && Count(perf, 0, EventKind::kNoteOff) == 1,
          "the pass plays its note once");
    Check(perf.approximations.empty(), "the conversion reports no approximation");
    Check(c.samples == 1 && c.sfz.files.size() == 1 && c.sfz.samples.size() == 1,
          "the SoundFont and the SFZ files hold the one sample");
    Check(ntdlib::testing::CheckConversion(c, "loop") == 0, "independent readers accept the files");
}

// A group of effects plays the objects and samples that it lacks from the groups pushed before it. The bank keeps its
// macro 1. Its effect of object 2 plays the pushed group's macro 2 and sample 9, under the pushed group's name.
void TestMusyxPushedGroups()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup startup = TestSongGroup();
    startup.pool.macros[2] = {Step(MacroOp::kStartSample, {9}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff})};
    startup.samples[0].id = 9;
    musyx::SongGroup bank = TestSongGroup();
    bank.pool.macros[1] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kEnd)};
    const auto pushed = std::make_shared<const musyx::SongGroup>(startup);

    musyx::AddPushedGroups(bank, {pushed});
    musyx::Instruments instruments(bank, TestVolumes(), {{9, "s0_9"}});
    musyx::GameEffect effect;
    effect.name = "pushed";
    effect.row = {.id = 1, .object = 2, .max_voices = 1, .key = 60};
    const Conversion c = musyx::ConvertEffect(effect, instruments);

    Check(bank.pool.macros.at(1).size() == 2 && bank.pool.macros.count(2) == 1,
          "the bank keeps its macro 1 and gains macro 2");
    const musyx::Sample* sample = instruments.GetSample(9);
    Check(sample != nullptr && sample->sfz_name == "s0_9" && sample->pcm.channels.size() == 1 &&
              sample->pcm.channels[0].size() == 3200,
          "sample 9 decodes from the pushed group's data");
    Check(c.samples == 1 && c.sfz.samples.size() == 1 && c.sfz.samples[0].name == "s0_9.wav",
          "the effect plays sample 9 under the pushed group's name");
    Check(instruments.Errors().empty(), "nothing fails");
}

// The SFZ files' shared folder gives each sample of a game one name. Group 1 gives sample 1 other data than group 0.
// Group 2 holds group 1's sample at another offset, and group 3 holds group 0's. Group 1's sample takes the group's
// index in its name. Its song's SFZ files play that file. Its SoundFont keeps the name "s1".
void TestMusyxSfzSampleNames()
{
    const musyx::SongGroup first = TestSongGroup();
    musyx::SongGroup other = TestSongGroup();
    for (uint8_t& byte : other.sample_data)
    {
        byte ^= 0x5a;
    }
    musyx::SongGroup moved = other;
    moved.sample_data.insert(moved.sample_data.begin(), 2, 0);
    moved.samples[0].offset += 2;
    const std::vector<std::shared_ptr<const musyx::SongGroup>> groups = {
        std::make_shared<const musyx::SongGroup>(first), std::make_shared<const musyx::SongGroup>(other),
        std::make_shared<const musyx::SongGroup>(moved), std::make_shared<const musyx::SongGroup>(first)};

    const std::vector<std::map<uint16_t, std::string>> names = musyx::SfzSampleNames(groups);
    musyx::Instruments named(*groups[1], TestVolumes(), names[1]);
    musyx::Instruments plain(*groups[1], TestVolumes());
    const Conversion c = musyx::ConvertSong(TestSong(true, false), {"loop", "G/A/A", 5, 127}, named, {});
    const Conversion unnamed = musyx::ConvertSong(TestSong(true, false), {"loop", "G/A/A", 5, 127}, plain, {});

    const auto name_of = [&names](std::size_t group)
    {
        const auto found = names[group].find(1);
        return found != names[group].end() ? found->second : std::string("none");
    };
    Check(names.size() == 4 && name_of(0) == "s1" && name_of(1) == "s1_1" && name_of(2) == "s1_1" && name_of(3) == "s1",
          "groups 0 and 3 name sample 1 s1, and groups 1 and 2 s1_1: " + name_of(0) + " " + name_of(1) + " " +
              name_of(2) + " " + name_of(3));
    Check(c.sfz.samples.size() == 1 && c.sfz.samples[0].name == "s1_1.wav", "group 1's song writes s1_1.wav");
    Check(c.sfz.files.size() == 1 && c.sfz.files[0].text.find("s1_1.wav") != std::string::npos &&
              c.sfz.files[0].text.find("/s1.wav") == std::string::npos,
          "the song's SFZ file plays s1_1.wav");
    Check(c.sf2 == unnamed.sf2, "the SoundFont stays the same");
}

// The early build of MusyX on the NDDEMO disc moves its clock on by whole 65536ths of a tick. At 120 beats a minute,
// it adds floor(floor(120 × 3072 / 60) × 65 / 8) = 49,920 of them in each run of 1 ms. The song plays at 119.02 beats a
// minute, and 0.089% faster at the output's rate.
void TestMusyxIntegerTickStep()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group, TestVolumes());
    musyx::GameSong listed{"early", "G/A/A", 5, 127};
    listed.tick_step = musyx::TickStep::kInteger;

    const Performance perf = musyx::PlaySong(TestSong(true, false), listed, instruments, {});

    constexpr double kBpm = 49920.0 / 65536.0 * 1000.0 * 60.0 / 384.0 * musyx::kOutputRate / musyx::kNominalRate;
    Check(!perf.tempo.empty() && std::abs(perf.tempo[0].bpm - kBpm) < 0.01,
          "the song plays at 119.02 beats a minute, at the output's rate: " +
              (perf.tempo.empty() ? std::string("no tempo") : std::to_string(perf.tempo[0].bpm)));
    Check(perf.loop && perf.loop->start == 0 && perf.loop->end == 768, "the loop still runs from tick 0 to 768");
}

// The modulators of instrument zone `zone` in `records`, a SoundFont's chunks: each one's source, destination, amount,
// amount source and transform.
std::vector<std::array<int, 5>> ZoneModulators(const seqlib::sf2::Records& records, std::size_t zone)
{
    std::vector<std::array<int, 5>> modulators;
    const uint16_t last = xsflib::LoadLe16(records.Record("ibag", 4, zone + 1) + 2);
    for (uint16_t m = xsflib::LoadLe16(records.Record("ibag", 4, zone) + 2); m < last; m++)
    {
        const uint8_t* modulator = records.Record("imod", 10, m);
        modulators.push_back({xsflib::LoadLe16(modulator), xsflib::LoadLe16(modulator + 2),
                              static_cast<int16_t>(xsflib::LoadLe16(modulator + 4)), xsflib::LoadLe16(modulator + 6),
                              xsflib::LoadLe16(modulator + 8)});
    }

    return modulators;
}

// Convert a song whose second note plays program 1: a copy of the test's macro with a ScaleVolume of `scale`/127.
Conversion ConvertScaledCopy(uint32_t scale, const std::string& name)
{
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = TestSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[2];
    macro.insert(macro.begin() + 1, Step(MacroOp::kScaleVolume, {scale, 0, 0, 0, 1}));
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 240, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 288, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});

    return musyx::ConvertSong(song, {name, "G/A/A", 5, 127}, instruments, {});
}

// The attenuation of the zones that play a sample in a SoundFont: the number of such zones, the values of their
// initialAttenuation generators, and their modulators to initialAttenuation.
struct SampleZoneAttenuation
{
    std::size_t zones = 0;
    std::vector<int> generators;
    std::vector<std::array<int, 5>> modulators;
};

SampleZoneAttenuation AttenuationOf(const std::vector<uint8_t>& sf2)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    SampleZoneAttenuation a;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (!gens.contains(seqlib::sf2::kSampleId))
        {
            continue;
        }

        a.zones++;
        if (gens.contains(seqlib::sf2::kInitialAttenuation))
        {
            a.generators.push_back(gens.at(seqlib::sf2::kInitialAttenuation));
        }
        for (const std::array<int, 5>& m : ZoneModulators(records, zone))
        {
            if (m[1] == seqlib::sf2::kInitialAttenuation)
            {
                a.modulators.push_back(m);
            }
        }
    }

    return a;
}

// A zone's initialAttenuation generator holds 2.5 times the zone's attenuation. FluidSynth, BASSMIDI and the E-mu chips
// apply 0.4 of it. Program 1 plays a copy of the macro with a ScaleVolume of 64/127. On the DLS curve, the zone of the
// copy then plays at (64/127)²: 119.05 cB down. Its generator holds 298 cB. The zone of program 0 plays at full level
// and gets no attenuation.
void TestMusyxZoneAttenuation()
{
    const Conversion c = ConvertScaledCopy(64, "quiet");

    const SampleZoneAttenuation a = AttenuationOf(c.sf2);

    Check(a.zones == 2, "each program has a zone: " + std::to_string(a.zones));
    Check(a.generators == std::vector<int>{298}, "only the zone of the quieter macro gets an attenuation: 298 cB");
    Check(a.modulators.empty(), "no zone has a modulator to initialAttenuation");
    Check(ntdlib::testing::CheckConversion(c, "quiet") == 0, "independent readers accept the files");
}

// The values of `opcode`, such as "volume", in the SFZ files of `c`, in the order of the files and their regions.
std::vector<std::string> OpcodeValues(const Conversion& c, const std::string& opcode)
{
    const std::string key = " " + opcode + "=";
    std::vector<std::string> values;
    for (const seqlib::sfz::File& f : c.sfz.files)
    {
        for (std::size_t at = f.text.find(key); at != std::string::npos; at = f.text.find(key, at + 1))
        {
            const std::size_t start = at + key.size();
            values.push_back(f.text.substr(start, f.text.find_first_of(" \n", start) - start));
        }
    }

    return values;
}

// A voice at a volume of 0 plays at a gain of 0. Program 1 plays a copy of the macro with a ScaleVolume of 0. The zone
// of the copy gets seqlib's form of an infinite attenuation: 1440 cB in its generator and a modulator of 864 cB. Its
// SFZ region gets the lowest volume that sfzformat.com documents: -144 dB.
void TestMusyxSilentZone()
{
    namespace sf2 = seqlib::sf2;
    constexpr std::array<int, 5> kRest = {sf2::kNoController, sf2::kInitialAttenuation, 864, sf2::kNoController,
                                          sf2::kLinear};

    const Conversion c = ConvertScaledCopy(0, "silent");

    const SampleZoneAttenuation a = AttenuationOf(c.sf2);
    Check(a.generators == std::vector<int>{1440}, "only the zone of the silent macro gets an attenuation: 1440 cB");
    Check(a.modulators == std::vector<std::array<int, 5>>{kRest}, "a modulator without a source holds 864 cB more");
    Check(OpcodeValues(c, "volume") == std::vector<std::string>{"-144"},
          "only the region of the silent macro gets a volume: -144 dB");
    Check(ntdlib::testing::CheckConversion(c, "silent") == 0, "independent readers accept the files");
}

// The level that the test's volume table gives a voice with a scale of 80/127 and an addition of 20/127, with the
// volume controllers at full, at MIDI velocity `velocity`. The level is relative to the voice's level at full velocity.
// The player writes a note on the table at the velocity whose square is the table's level of the note's velocity.
double AddedResponse(int velocity)
{
    const musyx::VolumeTable table = TestVolumes();
    const double written = velocity / 127.0;
    const double note = musyx::TableVolume(table, written * written);
    const auto level = [&table](double volume)
    {
        return musyx::TableLevel(table, 80.0 / 127.0 * volume + 20.0 / 127.0);
    };

    return level(note) / level(1.0);
}

// MusyX adds a ScaleVolume step's addition after the scale. Program 1 plays a layer whose entry plays macro 2 at a
// volume of 0. Macro 2 is the test's macro on the volume table, with a scale of 80/127 and an addition of 20/127. The
// voice then plays at the table's level of 20/127 at any velocity. Its zone holds that attenuation, and a velocity
// modulator of 0 that replaces SoundFont's default one. Its SFZ region gives a flat velocity curve. Program 2 plays
// macro 2 at full volume: the table's level of 100/127 at full velocity. Its modulator fits the voice's velocity
// response within 0.5 dB from velocity 40 to 127. Its SFZ region gives the response at every 8th velocity.
void TestMusyxVolumeAddition()
{
    namespace sf2 = seqlib::sf2;
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = TestSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[2];
    macro[0] = Step(MacroOp::kModeSelect, {0, 1});
    macro.insert(macro.begin() + 1, Step(MacroOp::kScaleVolume, {80, 20, 0xff, 0xff, 1}));
    group.pool.layers[0x8000] = {{.object = 2, .volume = 0}};
    group.group.pages.push_back({.object = 0x8000, .priority = 0, .max_voices = 8, .program = 1});
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 2});
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 200, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 240, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});
    events.push_back({.tick = 400, .kind = PatternEventKind::kProgram, .value = 2});
    events.push_back({.tick = 440, .kind = PatternEventKind::kNote, .key = 64, .velocity = 64, .length = 96});

    const Conversion c = musyx::ConvertSong(song, {"added", "G/A/A", 5, 127}, instruments, {});

    const SampleZoneAttenuation a = AttenuationOf(c.sf2);
    const auto generator = [](double volume)
    {
        return static_cast<int>(std::lround(-500.0 * std::log10(musyx::TableLevel(TestVolumes(), volume))));
    };
    std::vector<int> amounts;
    for (const std::array<int, 5>& m : a.modulators)
    {
        if (m[0] == (sf2::kNoteOnVelocity | sf2::kNegative | sf2::kConcave) && m[3] == 0 && m[4] == 0)
        {
            amounts.push_back(m[2]);
        }
    }
    Check(a.zones == 3 && a.generators == std::vector<int>{generator(20.0 / 127.0), generator(100.0 / 127.0)},
          "the zones hold the table's levels of 20/127 and of 100/127");
    Check(amounts.size() == 2 && amounts[0] == 0 && amounts[1] > 0 && amounts[1] < 960,
          "the velocity modulators replace the default one's 960 cB with 0 and with a fitted amount");
    for (int velocity = 40; velocity <= 127 && amounts.size() == 2; velocity += 8)
    {
        const double fitted = amounts[1] / 960.0 * 40.0 * std::log10(velocity / 127.0);
        const double game = 20.0 * std::log10(AddedResponse(velocity));
        Check(std::abs(fitted - game) < 0.5, "velocity " + std::to_string(velocity) + " plays within 0.5 dB: " +
                                                 std::to_string(fitted) + " against " + std::to_string(game));
    }

    const std::vector<std::string> flat = OpcodeValues(c, "amp_velcurve_1");
    const std::vector<std::string> middle = OpcodeValues(c, "amp_velcurve_64");
    Check(flat.size() == 2 && flat[0] == "1" && middle.size() == 2 && middle[0] == "1",
          "the region at a volume of 0 plays at every velocity alike");
    Check(middle.size() == 2 && std::abs(std::stod(middle[1]) - AddedResponse(64)) < 1e-6,
          "the other region gives the response at velocity 64: " + (middle.size() == 2 ? middle[1] : "none"));
    Check(OpcodeValues(c, "amp_velcurve_120").size() == 2 && OpcodeValues(c, "amp_velcurve_127").empty(),
          "the curves run to velocity 120, and reach 1 at 127 by default");
    Check(ntdlib::testing::CheckConversion(c, "added") == 0, "independent readers accept the files");
}

// An SFZ release falls by a factor of e^-8 in its time: 8 × 20 / ln 10 dB, about 69.49 dB. sfzformat.com gives that
// curve on its page on ampeg_attack. The test macro's DLS release falls 96.5 dB in 100 ms of MusyX's clock: 193 steps
// of 0.5 dB. The region's release therefore lasts 100 × 69.49 / 96.5 ms of MusyX's clock.
void TestMusyxSfzRelease()
{
    const double kSeconds = 0.1 * (8.0 * 20.0 / std::numbers::ln10) / 96.5 * musyx::kNominalRate / musyx::kOutputRate;
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group, TestVolumes());

    const Conversion c = musyx::ConvertSong(TestSong(false, false), {"release", "G/A/A", 5, 127}, instruments, {});

    const std::vector<std::string> releases = OpcodeValues(c, "ampeg_release");
    Check(releases.size() == 1 && std::abs(std::stod(releases[0]) - kSeconds) < 1e-6,
          "the release lasts " + std::to_string(kSeconds) + " s: " + (releases.empty() ? "none" : releases[0]));
}

// A zone whose voice fades after its release gets a release that reaches 20 dB down when the fade does. The test's
// macro plays on the DLS curve, where the level is the square of the volume. With FadeSteps, the volume reaches
// sqrt(0.1) at 120 + 800 × (1 - sqrt(0.1) / 0.625) ms of MusyX's clock, about 515.2 ms. A SoundFont release falls
// 100 dB in its time, and an SFZ release 8 × 20 / ln 10 dB.
void TestMusyxReleaseFade()
{
    namespace sf2 = seqlib::sf2;
    musyx::SongGroup group = TestSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.resize(3);
    const std::vector<ntdlib::musyx::MacroStep> fade = FadeSteps();
    macro.insert(macro.end(), fade.begin(), fade.end());
    musyx::Instruments instruments(group, TestVolumes());

    const Conversion c = musyx::ConvertSong(TestSong(false, false), {"fade", "G/A/A", 5, 127}, instruments, {});

    const double fit_ms = (120.0 + 800.0 * (1.0 - std::sqrt(0.1) / 0.625)) * musyx::kNominalRate / musyx::kOutputRate;
    const sf2::Records records = sf2::Read(c.sf2);
    std::vector<int> releases;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (gens.contains(sf2::kSampleId) && gens.contains(sf2::kReleaseVolEnv))
        {
            releases.push_back(static_cast<int16_t>(gens.at(sf2::kReleaseVolEnv)));
        }
    }
    const int expected = static_cast<int>(std::lround(1200.0 * std::log2(5.0 * fit_ms / 1000.0)));
    Check(releases.size() == 1 && std::abs(releases[0] - expected) <= 1,
          "the SoundFont release lasts 5 times the fade's time 20 dB down: " + std::to_string(expected) + " timecents");
    const std::vector<std::string> sfz = OpcodeValues(c, "ampeg_release");
    const double seconds = 8.0 / std::numbers::ln10 * fit_ms / 1000.0;
    Check(sfz.size() == 1 && std::abs(std::stod(sfz[0]) - seconds) < 1e-3,
          "the SFZ release lasts " + std::to_string(seconds) + " s: " + (sfz.empty() ? "none" : sfz[0]));
    Check(c.performance.approximations.empty(), "the conversion reports no approximation");
}

// The values of generator `oper` in the zones of `sf2` that play a sample, in the order of the zones.
std::vector<int> SampleZoneValues(const std::vector<uint8_t>& sf2, uint16_t oper)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<int> values;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (gens.contains(seqlib::sf2::kSampleId) && gens.contains(oper))
        {
            values.push_back(static_cast<int16_t>(gens.at(oper)));
        }
    }

    return values;
}

// A zone whose voice fades while its note is held gets a hold up to the fade's start and a decay that reaches the
// fade's level 20 dB down when the fade does. The voice of program 0 waits 100 ms, then follows FadeSteps' fade and
// stops by itself. On the DLS curve, its fade reaches 20 dB down 515.2 ms after its start, as in TestMusyxReleaseFade.
// Its sustain is silent, and its release goes on at the decay's rate: the game's voice ignores the release. The voice
// of program 1 ignores the release too. It fades to 64/128 of its volume over 400 ms, a level of (64/128)², 12.04 dB
// down. Its decay reaches 6.02 dB down when the fade does, at a volume of sqrt(0.5) after 234.3 ms. Its release stays
// the cut of a stopped voice. The song lasts until the first voice falls silent, 1020 ms after its start.
void TestMusyxHeldFade()
{
    namespace sf2 = seqlib::sf2;
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = TestSongGroup();
    const std::vector<ntdlib::musyx::MacroStep> fade = FadeSteps();
    group.pool.macros[1] = {Step(MacroOp::kModeSelect, {1, 1}), Step(MacroOp::kStartSample, {1}),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0})};
    group.pool.macros[1].insert(group.pool.macros[1].end(), fade.begin() + 1, fade.end());
    group.pool.macros[2] = {Step(MacroOp::kModeSelect, {1, 1}), Step(MacroOp::kStartSample, {1}),
                            Step(MacroOp::kEnvelope, {64, 0, 0xff, 0xff, 1, 0x90, 0x01}),
                            Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}), Step(MacroOp::kEnd)};
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 360, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 400, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});

    const Conversion c = musyx::ConvertSong(song, {"held", "G/A/A", 5, 127}, instruments, {});

    const double to_real = musyx::kNominalRate / musyx::kOutputRate;
    const auto timecents = [](double ms)
    {
        return static_cast<int>(std::lround(1200.0 * std::log2(ms / 1000.0)));
    };
    const double silent_fit_ms = (120.0 + 800.0 * (1.0 - std::sqrt(0.1) / 0.625)) * to_real;
    const double half_fit_ms = 400.0 * (1.0 - std::sqrt(0.5)) / 0.5 * to_real;
    const double half_db = -20.0 * std::log10(0.25);
    const std::vector<int> holds = SampleZoneValues(c.sf2, sf2::kHoldVolEnv);
    const std::vector<int> decays = SampleZoneValues(c.sf2, sf2::kDecayVolEnv);
    const std::vector<int> sustains = SampleZoneValues(c.sf2, sf2::kSustainVolEnv);
    const std::vector<int> releases = SampleZoneValues(c.sf2, sf2::kReleaseVolEnv);
    Check(holds.size() == 2 && holds[0] == timecents(100.0 * to_real) && holds[1] == -12000,
          "the first zone holds until its fade starts, 100 ms in, and the second has no hold");
    Check(decays.size() == 2 && std::abs(decays[0] - timecents(5.0 * silent_fit_ms)) <= 1 &&
              std::abs(decays[1] - timecents(100.0 / half_db * 2.0 * half_fit_ms)) <= 1,
          "each decay reaches its fit's level when the fade does");
    Check(sustains == std::vector<int>{1440, static_cast<int>(std::lround(10.0 * half_db))},
          "the first sustain is silent, and the second 12.04 dB down");
    Check(releases.size() == 2 && releases[0] == decays[0] && releases[1] == timecents(4.5 * 10.0 * to_real),
          "the first release goes on at the decay's rate, and the second is the cut of a stopped voice");

    const std::vector<std::string> sfz_holds = OpcodeValues(c, "ampeg_hold");
    const std::vector<std::string> sfz_decays = OpcodeValues(c, "ampeg_decay");
    const std::vector<std::string> sfz_releases = OpcodeValues(c, "ampeg_release");
    Check(sfz_holds.size() == 1 && std::abs(std::stod(sfz_holds[0]) - 0.1 * to_real) < 1e-6,
          "the first SFZ region holds for 100 ms");
    Check(sfz_decays.size() == 2 && sfz_releases.size() == 2 &&
              std::abs(std::stod(sfz_decays[0]) - 8.0 / std::numbers::ln10 * silent_fit_ms / 1000.0) < 1e-3 &&
              sfz_releases[0] == sfz_decays[0],
          "the first region's decay and release fall at the fit's rate");
    Check(std::abs(c.performance.seconds - 1.02) < 1e-9,
          "the song lasts until the first voice falls silent: " + std::to_string(c.performance.seconds));
    Check(c.performance.approximations ==
              std::map<std::string, uint32_t>{{"voices that loop and ignore the release stop at their note's end", 1}},
          "only the voice that keeps sounding is reported, for stopping at its note's end");
    Check(ntdlib::testing::CheckConversion(c, "held") == 0, "independent readers accept the files");
}

// Each instrument's global zone holds seqlib's kGlobalZoneModulators and a modulator from CC1 to the vibrato's depth.
// The set's CC10 modulator has an amount of 500 and replaces the default one of 1000. The CC1 modulator has an amount
// of 0 and replaces the default one of 50 cents.
void TestMusyxGlobalZone()
{
    namespace sf2 = seqlib::sf2;
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group, TestVolumes());

    const Conversion c = musyx::ConvertSong(TestSong(false, false), {"global", "G/A/A", 5, 127}, instruments, {});
    const sf2::Records records = sf2::Read(c.sf2);

    std::vector<std::array<int, 5>> expected;
    for (const sf2::Modulator& m : sf2::kGlobalZoneModulators)
    {
        expected.push_back({m.source, m.dest, m.amount, m.amount_source, m.transform});
    }
    expected.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, 0, sf2::kNoController, sf2::kLinear});

    std::vector<std::vector<std::array<int, 5>>> global_zones;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        if (!records.ZoneGens(zone).contains(sf2::kSampleId))
        {
            global_zones.push_back(ZoneModulators(records, zone));
        }
    }

    constexpr std::array<int, 5> kCc10 = {sf2::kSrcPan, sf2::kPan, 500, sf2::kNoController, sf2::kLinear};
    Check(global_zones.size() == 1, "the instrument has one global zone: " + std::to_string(global_zones.size()));
    Check(!global_zones.empty() && global_zones[0] == expected,
          "the global zone holds kGlobalZoneModulators and the CC1 modulator");
    Check(!global_zones.empty() && std::ranges::count(global_zones[0], kCc10) == 1,
          "the global zone holds the CC10 modulator with an amount of 500");
}

// A tempo table starts again in each pass. The run goes on past the end to find the loop, but the performance keeps
// no tempo change after the end. The table here slows the song to 60 beats a minute halfway through each pass.
void TestMusyxTempoTable()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(true, false);
    song.tempo_changes = {{.tick = 0, .tempo = 120}, {.tick = 384, .tempo = 60}};

    const Performance perf = musyx::PlaySong(song, {"tempo", "G/A/A", 5, 127}, instruments, {});

    Check(perf.loop && perf.loop->start == 0 && perf.loop->end == 768 && perf.end_tick == 768,
          "the loop runs from tick 0 to 768");
    std::string ticks;
    for (const Performance::Tempo& t : perf.tempo)
    {
        ticks += " " + std::to_string(t.tick);
    }
    Check(ticks == " 0 384 768", "the tempo changes at ticks 0, 384 and 768, the pass's end:" + ticks);
}

// A macro's PitchWheelRange gives its voice the pitch wheel's range. This one sets 12 semitones up and 6 down. The
// channel takes its latest note's range. The pattern moves the wheel halfway up from its centre during the note. It
// then moves the wheel halfway down.
void TestMusyxPitchWheelRange()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = TestSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(), Step(MacroOp::kPitchWheelRange, {12, 6}));
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(true, false);
    song.patterns[0].pitch_wheel = {{.tick = 96, .value = 0x3000}, {.tick = 144, .value = 0x1000}};

    const Performance perf = musyx::PlaySong(song, {"bend", "G/A/A", 5, 127}, instruments, {});

    std::vector<float> bends;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.semitones != 0.0f)
        {
            bends.push_back(e.semitones);
        }
    }
    Check(bends == std::vector<float>{6.0f, -3.0f}, "the wheel bends 6 semitones up and 3 down over the voice's range");
}

// A MIDI channel bends its notes over its latest note's range. The first note has a range of 12. At tick 144, a note
// of program 1 gives the channel a range of 2 while the first note is still held. The wheel already stands at 0x3000.
// The report counts the held note once.
void TestMusyxMixedPitchWheelRanges()
{
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = TestSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(), Step(MacroOp::kPitchWheelRange, {12, 6}));
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 120, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 144, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 192});
    song.patterns[0].pitch_wheel = {{.tick = 96, .value = 0x3000}, {.tick = 168, .value = 0x2800}};

    const Performance perf = musyx::PlaySong(song, {"bends", "G/A/A", 5, 127}, instruments, {});

    std::vector<float> bends;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.semitones != 0.0f)
        {
            bends.push_back(e.semitones);
        }
    }
    Check(bends == std::vector<float>{6.0f, 1.0f, 0.5f}, "the second note's range bends the channel from its note on");
    const auto mixed =
        perf.approximations.find("pitch wheel ranges that differ among a channel's notes take the latest note's");
    Check(mixed != perf.approximations.end() && mixed->second == 1, "the held note of another range counts once");
}

// A note of no length ends on the tick it starts. Its note-off comes after its note-on. The MIDI note then ends, and a
// later note of the same key plays on the same channel.
void TestMusyxNoteOfNoLength()
{
    using ntdlib::musyx::PatternEventKind;
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(false, false);
    song.patterns[0].events[0].length = 0;
    song.patterns[0].events.push_back(
        {.tick = 384, .kind = PatternEventKind::kNote, .key = 60, .velocity = 100, .length = 192});

    const Conversion c = musyx::ConvertSong(song, {"empty", "G/A/A", 5, 127}, instruments, {});

    std::vector<std::pair<EventKind, uint32_t>> notes;
    for (const TrackEvent& e : c.performance.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.kind, e.tick);
        }
    }
    const std::vector<std::pair<EventKind, uint32_t>> expected = {
        {EventKind::kNoteOn, 0}, {EventKind::kNoteOff, 0}, {EventKind::kNoteOn, 384}, {EventKind::kNoteOff, 576}};
    Check(notes == expected, "the note of no length ends after its note-on, at its tick");
    Check(c.channels == 1, "both notes play on one MIDI channel");
    Check(ntdlib::testing::CheckConversion(c, "empty") == 0, "independent readers accept the files");
}

// VolumeSelect picks the sources of a voice's volume. Byte 1 names the controller, and bytes 2 and 3 hold its share in
// percent. Byte 4 picks how the source joins the others: 0 replaces them. A share of the volume controller (CC7) that
// replaces the others scales the voice's volume. MusyX holds the controller's value times the share at full volume.
// Other sources are reported.
void TestVolumeSelect()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kVolumeSelect, {7, 40}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kVolumeSelect, {7, 127}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kVolumeSelect, {1, 50}), Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kVolumeSelect, {7, 60, 0, 1}), Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kEnd)};

    const musyx::MacroVoice part = musyx::FollowMacro(pool, 1, 127);
    Check(part.source_volume == 0.4 && musyx::VoiceVolume(part, 0.5, 1.0) == 0.2 && part.approximations.empty(),
          "a share of 40% of the volume controller scales the voice's volume by 0.4");
    Check(musyx::VoiceVolume(musyx::FollowMacro(pool, 2, 127), 1.0, 1.0) == 1.0,
          "a share above 100% plays at full volume with the controller at full");
    const std::vector<std::string> other = {"volume sources other than a share of the volume controller left out"};
    Check(musyx::FollowMacro(pool, 3, 127).approximations == other, "another controller is reported");
    Check(musyx::FollowMacro(pool, 4, 127).approximations == other, "a source that joins the others is reported");
    Check(musyx::FollowMacro(pool, 5, 127).source_volume == 1.0, "a voice follows the volume controller in full");
}

// Panning sets the voice's pan from byte 1. The voice takes it in place of the pan of the keymap or the layer and of a
// PianoPan before it. MusyX then moves the pan by byte 4, a signed number, over the time in bytes 2 and 3. dolphinsf2
// keeps the first pan. The conversion reports the move.
void TestPanning()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kPianoPan, {100, 60, 54}), Step(MacroOp::kPanning, {0}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kPanning, {127}), Step(MacroOp::kPianoPan, {100, 60, 54}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kPanning, {84, 0xe8, 0x03, 0xd8}), Step(MacroOp::kEnd)};

    const musyx::MacroVoice left = musyx::FollowMacro(pool, 1, 127);
    Check(left.pan == 0 && left.pan_last && left.piano_pan && left.approximations.empty(),
          "the step's pan comes after a PianoPan");
    const musyx::MacroVoice piano = musyx::FollowMacro(pool, 2, 127);
    Check(piano.pan == 127 && !piano.pan_last && piano.piano_pan, "a PianoPan after the step comes last");
    const musyx::MacroVoice moving = musyx::FollowMacro(pool, 3, 127);
    Check(moving.pan == 84 && moving.pan_moves && moving.approximations.empty(),
          "a pan that moves keeps its first pan");
}

// A song's voice takes a macro's Panning: its region pans hard left. A song's channel holds the volume controller that
// a macro sets. A song's voice leaves that out and reports it. The rest of the song's files stays as it is without the
// two steps. An effect's voice takes both.
void TestMusyxSongPanning()
{
    using ntdlib::musyx::MacroOp;
    const musyx::SongGroup plain = TestSongGroup();
    musyx::SongGroup group = TestSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin() + 1, Step(MacroOp::kPanning, {0}));
    macro.insert(macro.begin() + 1, Step(MacroOp::kSetVariable, {1, 7, 0, 0x00, 0x20}));
    musyx::Instruments plain_instruments(plain, TestVolumes());
    musyx::Instruments instruments(group, TestVolumes());
    const ntdlib::musyx::Song song = TestSong(false, false);

    const Conversion before = musyx::ConvertSong(song, {"pans", "G/A/A", 5, 127}, plain_instruments, {});
    const Conversion after = musyx::ConvertSong(song, {"pans", "G/A/A", 5, 127}, instruments, {});
    Check(OpcodeValues(before, "pan").empty() && OpcodeValues(after, "pan") == std::vector<std::string>{"-100"},
          "the song's region pans hard left");
    bool same = after.midi == before.midi && after.sfz.files.size() == before.sfz.files.size();
    for (std::size_t i = 0; same && i < after.sfz.files.size(); i++)
    {
        std::string text = after.sfz.files[i].text;
        if (const std::size_t at = text.find(" pan=-100"); at != std::string::npos)
        {
            text.erase(at, std::string(" pan=-100").size());
        }
        same = text == before.sfz.files[i].text;
    }
    Check(same, "the rest of the song's files stays the same");
    const auto& reported = after.performance.approximations;
    Check(reported.count("SoundMacro commands that dolphinsf2 doesn't follow left out") == 0 &&
              reported.count("volumes and sends that a macro sets left out") == 1,
          "the song reports the controller alone");

    musyx::GameEffect effect;
    effect.name = "left";
    effect.row = {.id = 1, .object = 1, .max_voices = 1, .key = 60};
    const Conversion c = musyx::ConvertEffect(effect, instruments);
    Check(OpcodeValues(c, "pan") == std::vector<std::string>{"-100"}, "the effect's region pans hard left");
    Check(ntdlib::testing::CheckConversion(c, "left") == 0, "independent readers accept the effect's files");
}

// SetPitch sets the rate of the voice's sample in Hz, from bytes 1 to 3, whatever the voice's key. A later SetKey takes
// its place. A step with more in bytes 4 to 7 is reported. A song's macro at 16,000 Hz plays the sample of 32,000 Hz an
// octave down at every key: 1,200 cents down, and the output's 1.5 cents up.
void TestSetPitch()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kSetPitch, {0x00, 0x7d, 0x00}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kSetPitch, {0x00, 0x7d, 0x00}), Step(MacroOp::kSetKey, {72}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kSetPitch, {0x00, 0x7d, 0x00, 1}), Step(MacroOp::kEnd)};
    const musyx::MacroVoice rate = musyx::FollowMacro(pool, 1, 127);
    Check(rate.rate_hz == 32000.0 && rate.approximations.empty(), "the step sets a rate of 32,000 Hz");
    const musyx::MacroVoice key = musyx::FollowMacro(pool, 2, 127);
    Check(!key.rate_hz && key.fixed_key == 72, "a later SetKey takes the rate's place");
    const std::vector<std::string> left_out = {"SoundMacro commands that dolphinsf2 doesn't follow left out"};
    Check(musyx::FollowMacro(pool, 3, 127).approximations == left_out, "a step with more bytes is reported");

    musyx::SongGroup group = TestSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin() + 1, Step(MacroOp::kSetPitch, {0x00, 0x3e, 0x80}));
    musyx::Instruments instruments(group, TestVolumes());
    const Conversion c = musyx::ConvertSong(TestSong(false, false), {"rate", "G/A/A", 5, 127}, instruments, {});
    Check(OpcodeValues(c, "pitch_keytrack") == std::vector<std::string>{"0"} &&
              OpcodeValues(c, "transpose") == std::vector<std::string>{"-11"} &&
              OpcodeValues(c, "tune") == std::vector<std::string>{"-98"},
          "the region plays an octave down at every key");
    Check(ntdlib::testing::CheckConversion(c, "rate") == 0, "independent readers accept the files");
}

// SendMessage sends a message to the voices of the macro that bytes 2 and 3 name. No voice plays macro 0xffff.
void TestSendMessage()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kSendMessage, {0, 0xff, 0xff}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kSendMessage, {0, 5, 0}), Step(MacroOp::kEnd)};

    Check(musyx::FollowMacro(pool, 1, 127).approximations.empty(), "a message that reaches no voice changes nothing");
    Check(musyx::FollowMacro(pool, 2, 127).approximations == std::vector<std::string>{musyx::kCommandsLeftOut},
          "a message to a macro is reported");
}

// A voice that waits before it starts its sample plays its note from the sample's start (VoiceFrom). The times of the
// voice's stop, ramps and vibrato then count from there. A voice that stops before the note's start plays no sample.
void TestVoiceFrom()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 200, 0}),
        Step(MacroOp::kVibrato, {0, 20, 0, 0, 1, 100, 0}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 50, 0}),
        Step(MacroOp::kEnvelope, {0, 0, 0xff, 0xff, 1, 100, 0}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0x2c, 0x01}),
        Step(MacroOp::kStopSample),
        Step(MacroOp::kEnd),
    };

    const musyx::MacroVoice voice = musyx::FollowMacro(pool, 1, 127);
    Check(voice.sample_ms == 200.0 && voice.stop_ms == 550.0, "the voice starts its sample at 200 ms and stops at 550");
    const musyx::MacroVoice later = musyx::VoiceFrom(voice, 200.0);
    Check(later.sample && later.sample_ms == 0.0 && later.stop_ms == 350.0, "from the sample's start, it stops at 350");
    Check(later.held_ramps.size() == 1 && later.held_ramps[0].start_ms == 50.0, "its ramp starts 50 ms in");
    Check(later.vibrato && later.vibrato->delay_ms == 0.0, "its vibrato starts with the sample");
    Check(!musyx::VoiceFrom(voice, 600.0).sample, "a voice that stops before the note's start plays no sample");
}

// PlayMacro starts another voice on the macro that bytes 2 and 3 name, from the step that bytes 4 and 5 name, with the
// key of the voice that starts it plus byte 1. An effect plays the voices that its object's macros start. Each voice's
// note starts where the voice starts its sample. The effect's voices that start their samples at one time share a
// note. Each note plays on a MIDI channel of its own, with a preset of its own.
void TestPlayMacro()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = TestSongGroup();

    // Macro 3 starts macro 4 at once, two keys up. It waits 200 ms, starts macro 5, and starts its sample. Macro 4
    // starts its sample at once. Macro 5 waits 100 ms first. Each waits for ever once its sample plays.
    group.pool.macros[3] = {
        Step(MacroOp::kPlayMacro, {2, 4, 0, 0, 0, 50, 0xff}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 200, 0}),
        Step(MacroOp::kPlayMacro, {0, 5, 0, 0, 0, 50, 0xff}), Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff}),  Step(MacroOp::kEnd),
    };
    group.pool.macros[4] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff})};
    group.pool.macros[5] = {
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff}),
    };
    musyx::Instruments instruments(group, TestVolumes());

    const std::vector<musyx::StartedVoice>& started = instruments.Macro(3).started;
    Check(started.size() == 2 && started[0].macro == 4 && started[0].key_add == 2 && started[0].at_ms == 0.0 &&
              started[1].macro == 5 && started[1].key_add == 0 && started[1].at_ms == 200.0,
          "the macro starts macro 4 at once, two keys up, and macro 5 at 200 ms");

    std::set<std::string> left_out;
    const std::vector<musyx::EffectVoice> voices = musyx::EffectVoicesOf(instruments, 3, 60, left_out);
    Check(voices.size() == 3 && voices[0].voice.macro == 3 && voices[1].voice.macro == 4 && voices[1].voice.key == 62 &&
              voices[2].voice.macro == 5 && voices[2].start_ms == 200.0 && left_out.empty(),
          "the effect plays the object's voice and the two that it starts");

    musyx::GameEffect effect;
    effect.name = "three voices";
    effect.row = {.id = 1, .object = 3, .max_voices = 4, .key = 60};
    const std::vector<musyx::EffectNote> notes = musyx::EffectNotes(effect, instruments, left_out);
    const auto note_is = [&notes](std::size_t i, double start, uint8_t key, uint16_t macro)
    {
        return notes[i].start_ms == start && notes[i].key == key && notes[i].voices.size() == 1 &&
               notes[i].voices[0].macro == macro;
    };
    Check(notes.size() == 3 && note_is(0, 0.0, 62, 4) && note_is(1, 200.0, 60, 3) && note_is(2, 300.0, 60, 5),
          "a note for each start of a sample, at 0, 200 and 300 ms");

    const Conversion c = musyx::ConvertEffect(effect, instruments);
    std::vector<int64_t> starts;
    std::vector<int64_t> ends;
    for (std::size_t slot = 0; slot < c.performance.tracks.size(); slot++)
    {
        for (const TrackEvent& e : c.performance.tracks[slot])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                starts.push_back(e.tick);
            }
            else if (e.kind == EventKind::kNoteOff)
            {
                ends.push_back(e.tick);
            }
        }
    }
    Check(starts.size() == 3 && starts[0] == 0 && starts[1] > 0 && std::abs(starts[2] * 2 - starts[1] * 3) <= 5,
          "the notes start on three channels, the third at 1.5 times the second's time");

    // The samples loop. Each note stops 10 s after the last note's start: 50 times the second note's time later.
    const auto stops_after_last_start = [&](int64_t end)
    {
        return starts.size() == 3 && std::abs(end - (starts[2] + 50 * starts[1])) <= 50;
    };
    Check(ends.size() == 3 && std::all_of(ends.begin(), ends.end(), stops_after_last_start),
          "the held notes stop 10 s after the last note's start");
    Check(c.performance.presets.size() == 3, "each note has a preset");
    Check(ntdlib::testing::CheckConversion(c, "three voices") == 0, "independent readers accept the files");
}

// SetVariable with byte 1 sets the MIDI controller that byte 2 names to the value in bytes 4 and 5, of 14 bits. The
// voice's volume follows the volume controller. A voice that a macro starts shares the controllers of the voice that
// starts it. An effect's note takes the sends that its voices set. Without byte 1, the step sets a variable. A
// variable changes nothing by itself.
void TestSetController()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = TestSongGroup();
    group.pool.macros[3] = {
        Step(MacroOp::kSetVariable, {1, 7, 0, 0x00, 0x20}),
        Step(MacroOp::kPlayMacro, {0, 4, 0, 0, 0, 50, 0xff}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff}),
    };
    group.pool.macros[4] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff})};
    group.pool.macros[5] = {
        Step(MacroOp::kSetVariable, {1, 91, 0, 0x00, 0x20}),
        Step(MacroOp::kSetVariable, {1, 93, 0, 0x00, 0x10}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff}),
    };
    group.pool.macros[6] = {Step(MacroOp::kSetVariable, {0, 7, 0, 0x10, 0x27}), Step(MacroOp::kEnd)};
    group.pool.macros[7] = {Step(MacroOp::kSetVariable, {1, 1, 0, 0x10, 0x27}), Step(MacroOp::kEnd)};
    musyx::Instruments instruments(group, TestVolumes());

    const double share = 0x2000 / 16383.0;
    const musyx::MacroVoice& voice = instruments.Macro(3);
    Check(voice.volume_controller == share && musyx::VoiceVolume(voice, 1.0, share) == share &&
              voice.approximations.empty(),
          "the macro sets its volume controller to 0x2000 of 0x3fff, and its volume follows");
    std::set<std::string> left_out;
    const std::vector<musyx::EffectVoice> voices = musyx::EffectVoicesOf(instruments, 3, 60, left_out);
    Check(voices.size() == 2 && voices[1].voice.macro == 4 && voices[1].voice.volume == share,
          "the voice that the macro starts takes the controller");
    Check(instruments.Macro(5).reverb_controller == 0x2000 / 16383.0 &&
              instruments.Macro(5).chorus_controller == 0x1000 / 16383.0 && instruments.Macro(5).approximations.empty(),
          "the macro sets the sends to aux A and aux B");
    Check(!instruments.Macro(6).volume_controller && instruments.Macro(6).approximations.empty(),
          "a variable changes nothing by itself");
    Check(instruments.Macro(7).approximations ==
              std::vector<std::string>{"controllers other than the volume and the sends that a macro sets left out"},
          "another controller is reported");

    musyx::GameEffect effect;
    effect.name = "sends";
    effect.row = {.id = 1, .object = 5, .max_voices = 1, .key = 60};
    const Conversion c = musyx::ConvertEffect(effect, instruments);
    std::map<EventKind, int> sends;
    for (const std::vector<TrackEvent>& track : c.performance.tracks)
    {
        for (const TrackEvent& e : track)
        {
            if (e.kind == EventKind::kReverb || e.kind == EventKind::kChorus)
            {
                sends[e.kind] = e.value;
            }
        }
    }
    Check(sends[EventKind::kReverb] == 64 && sends[EventKind::kChorus] == 32,
          "the effect's note sends at the levels that its voice sets");
}

// A macro that starts one sample after another plays them in parts. Each part takes the settings that the macro has
// made by the time its sample has played for a moment, and stops where the next part starts. A sample that another
// replaces at once never sounds. A song's note plays the first sample. An effect plays each part as a note.
void TestVoiceParts()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = TestSongGroup();

    // Sample 1 at 80% of the volume controller for 200 ms. Then sample 2 two keys up at 40%. Sample 1 replaces it at
    // once. The voice then waits for ever.
    group.pool.macros[3] = {
        Step(MacroOp::kVolumeSelect, {7, 80}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 200, 0}),
        Step(MacroOp::kAddKey, {2}),
        Step(MacroOp::kVolumeSelect, {7, 40}),
        Step(MacroOp::kStartSample, {2}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff}),
        Step(MacroOp::kEnd),
    };
    musyx::Instruments instruments(group, TestVolumes());

    const musyx::MacroVoice& voice = instruments.Macro(3);
    Check(voice.sample == 1 && voice.sample_ms == 0.0 &&
              voice.approximations == std::vector<std::string>{"a second sample of a voice left out"},
          "a song's note plays the first sample and reports the second");
    if (!Check(voice.parts.size() == 2, "the voice plays in two parts"))
    {
        return;
    }
    const musyx::MacroVoice& first = voice.parts[0];
    Check(first.sample == 1 && first.sample_ms == 0.0 && first.stop_ms == 200.0 && first.source_volume == 0.8 &&
              first.key_shift == 0,
          "the first part plays sample 1 for 200 ms at 80%");
    const musyx::MacroVoice& second = voice.parts[1];
    Check(second.sample == 1 && second.sample_ms == 200.0 && !second.stop_ms && second.source_volume == 0.4 &&
              second.key_shift == 2 && second.approximations.empty(),
          "the second part plays the sample that replaces sample 2, two keys up at 40%, from 200 ms on");

    musyx::GameEffect effect;
    effect.name = "two parts";
    effect.row = {.id = 1, .object = 3, .max_voices = 1, .key = 60};
    std::set<std::string> left_out;
    const std::vector<musyx::EffectNote> notes = musyx::EffectNotes(effect, instruments, left_out);
    Check(notes.size() == 2 && notes[0].start_ms == 0.0 && notes[0].voices.size() == 1 &&
              notes[0].voices[0].part == 1 && notes[1].start_ms == 200.0 && notes[1].voices.size() == 1 &&
              notes[1].voices[0].part == 2 && left_out.empty(),
          "the effect plays a note for each part");
    Check(instruments.Voice(notes[1].voices[0]).sample_ms == 0.0 && !instruments.Voice(notes[1].voices[0]).stop_ms,
          "the second part's note starts with its sample");

    const Conversion c = musyx::ConvertEffect(effect, instruments);
    Check(c.performance.presets.size() == 2, "each part has a preset");
    Check(ntdlib::testing::CheckConversion(c, "two parts") == 0, "independent readers accept the files");
}

// A table of two songs that play as effects, and a table of two effects that the play function starts.
void TestPlayedEffects()
{
    constexpr uint32_t kBase = 0x80200000;
    BigEndianBytes b;
    b.U8(3).U8(0).U16(13).U8(90).U8(10).U8(1).U8(0);
    b.U8(1).U8(0).U16(1606).U8(100).U8(20).U8(1).U8(0);
    b.U16(1616).U8(100).U8(1).U8(50).U8(10);
    b.U16(1607).U8(90).U8(0).U8(75).U8(12);
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<musyx::SoundRoomSong> songs = musyx::ReadEffectSongs(*dol, kBase, 2);
    if (Check(songs.size() == 2, "the table lists two songs"))
    {
        Check(songs[0].name == "se-0013" && songs[0].number == 13 && songs[0].group_id == 3 && songs[0].volume == 90,
              "the first song: its name from its number, its number, its group and its volume");
        Check(songs[1].number == 1606 && songs[1].group_id == 1, "the second song");
    }

    const std::vector<musyx::PlayedEffect> effects = musyx::ReadPlayedEffects(*dol, kBase + 16, 2);
    if (Check(effects.size() == 2, "the table lists two effects"))
    {
        Check(
            effects[0].number == 1616 && effects[0].volume == 100 && effects[0].reverb == 50 && effects[0].chorus == 10,
            "the first effect: its number, its volume and its sends");
        Check(
            effects[1].number == 1607 && effects[1].volume == 90 && effects[1].reverb == 75 && effects[1].chorus == 12,
            "the second effect");
    }
    const std::string past_the_data = ErrorOf([&] { musyx::ReadPlayedEffects(*dol, kBase + 24, 2); });
    Check(past_the_data == "main.dol has no data at the song list's address",
          "a table past the DOL's data fails: " + past_the_data);
}

// Open the game at `path` as dolphinsf2 opens its inputs. Convert every sequence, and validate each file.
void TestGameConvertsEverySequence(const char* path)
{
    std::optional<Input> input;
    try
    {
        input = Input::Open(std::filesystem::path(path));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: %s\n", path, e.what());
        failures++;
        return;
    }

    const std::vector<ntdlib::sequence_export::Sequence> sequences = input->Sequences();
    int looping = 0;
    for (const ntdlib::sequence_export::Sequence& sequence : sequences)
    {
        Conversion c;
        try
        {
            c = input->Convert(sequence.index, {});
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", sequence.name.c_str(), e.what());
            failures++;
            continue;
        }

        failures += ntdlib::testing::CheckConversion(c, sequence.name);
        looping += c.performance.loop ? 1 : 0;
    }

    std::printf("%s: %zu sequences converted and checked, %d loop\n", path, sequences.size(), looping);
}

} // namespace

int main(int argc, char** argv)
{
    TestMusyxPaths();
    TestMusyxSongLists();
    TestSoundManagerFile();
    TestSoundTest();
    TestMusyxSoundTable();
    TestSoundRoom();
    TestSongsInOneFile();
    TestSongPaths();
    TestYay0();
    TestRelModule();
    TestDlsMilliseconds();
    TestPianoPan();
    TestScaleVolume();
    TestReleaseFade();
    TestHeldFade();
    TestMusyxSongVolume();
    TestMusyxVolumeTable();
    TestMusyxTableLevels();
    TestMusyxSong();
    TestMusyxSfzSampleNames();
    TestMusyxPushedGroups();
    TestMusyxIntegerTickStep();
    TestMusyxZoneAttenuation();
    TestMusyxSilentZone();
    TestMusyxVolumeAddition();
    TestMusyxSfzRelease();
    TestMusyxReleaseFade();
    TestMusyxHeldFade();
    TestMusyxGlobalZone();
    TestMusyxTempoTable();
    TestMusyxPitchWheelRange();
    TestMusyxMixedPitchWheelRanges();
    TestMusyxNoteOfNoLength();
    TestVolumeSelect();
    TestPanning();
    TestMusyxSongPanning();
    TestSetPitch();
    TestSendMessage();
    TestVoiceFrom();
    TestPlayMacro();
    TestVoiceParts();
    TestSetController();
    TestPlayedEffects();

    if (argc > 1)
    {
        TestGameConvertsEverySequence(argv[1]);
    }

    return xsflib::testing::Summary("dolphinsf2");
}
