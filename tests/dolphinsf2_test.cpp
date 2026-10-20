// SPDX-License-Identifier: MIT

// Unit tests for dolphinsf2, without game data. The tests build small MusyX song groups, Yay0 data and modules of code
// in memory. ntdlib's tests cover the conversion of JAudio's sound data.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/dol.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "ntdlib/testing/dol_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
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

// A sound manager's file with one group and two songs. The header gives the offsets and sizes of the tables of groups
// and songs. It also gives the offsets of the groups' data, the groups' samples and the songs that no group holds.
// Group 7's data starts with the offsets of its pool, its project, its sample directory and its songs. Song 0's SNG
// file lies at offset 2 of the group's songs and holds the bytes 1 to 4. Song 1's file lies at offset 1 of the songs
// that no group holds and holds the bytes 5 to 7. The group's samples lie at offset 1 of the groups' samples. They're
// the bytes 0xaa and 0xbb. The settings list one base group: the table's group 0.
void TestSoundManagerFile()
{
    BigEndianBytes b;
    b.PadTo(0x60);
    b.Put32(0x20, 0x60);
    b.Put32(0x24, 0x20);
    b.U16(7).U16(0).U32(0).U32(0x20).U32(1).U32(2).PadTo(0x80);
    b.Put32(0x28, 0x80);
    b.Put32(0x2c, 0x20);
    b.U16(7).U16(3).U32(2).U32(4).U8(0).U8(90).U16(0);
    b.U16(7).U16(4).U32(1).U32(3).U8(0xff).U8(127).U16(0);
    b.Put32(0x38, 0xa0);
    b.U32(0x10).U32(0x14).U32(0x18).U32(0x1a);
    b.U32(0x11111111).U32(0x22222222).U16(0x3333).U16(0).U32(0x01020304);
    b.Put32(0x40, 0xc0);
    b.U8(0).U8(0xaa).U8(0xbb);
    b.Put32(0x48, 0xc3);
    b.U8(0).U8(5).U8(6).U8(7);
    b.Put32(0x10, 0xc7);
    b.PadTo(0xc7 + 0x28).U8(1).U8(0);
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
    std::vector<uint8_t> wrong_base = file;
    wrong_base.back() = 5;
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
    Check(ErrorOf([&]
                  { musyx::ReadSoundTest(missing.Data(), "dll/test.rel", 0, 1, messages.Data(), "mess/test.dat"); }) ==
              "mess/test.dat's message 47-2 is missing",
          "a name that the bank lacks fails");
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

    const musyx::MacroVoice shifted = musyx::FollowMacro(pool, 1);
    Check(shifted.piano_pan && shifted.piano_pan->scale == 100 && shifted.piano_pan->centre_key == 60 &&
              shifted.piano_pan->centre_pan == 54,
          "PianoPan takes its scale, centre key and centre pan from bytes 1 to 3");
    Check(musyx::PianoPanAt(*shifted.piano_pan, 60) == 63.375 && musyx::PianoPanAt(*shifted.piano_pan, 0) == 16.5,
          "an AddKey before the step moves the key that the pan follows");

    const musyx::MacroVoice wide = musyx::FollowMacro(pool, 2);
    Check(wide.piano_pan && !wide.piano_pan->fixed_key, "a SetKey after the step leaves the key of the pan alone");
    Check(wide.piano_pan && musyx::PianoPanAt(*wide.piano_pan, 0) == 124.0,
          "a scale of -128 moves the pan one step left for each key");
    Check(wide.piano_pan && musyx::PianoPanAt(*wide.piano_pan, 127) == 0.0, "the pan stops at 0");

    const musyx::MacroVoice fixed = musyx::FollowMacro(pool, 3);
    Check(fixed.piano_pan && musyx::PianoPanAt(*fixed.piano_pan, 0) == 63.375,
          "a SetKey before the step fixes the key that the pan follows");
    Check(!musyx::FollowMacro(pool, 4).piano_pan, "a macro without the step keeps the pan of its keymap or layer");
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

    const double channel = 100.0 / 127.0 * 100.0 / 127.0;
    const auto written = [](double volume)
    {
        return static_cast<int>(std::lround(127.0 * std::sqrt(musyx::TableLevel(TestVolumes(), volume))));
    };
    const std::vector<int> volumes = VolumesOf(table);
    Check(volumes == std::vector<int>{written(channel), written(64.0 / 127.0 * 100.0 / 127.0)},
          "CC7 takes the square root of the table's level of the channel's volume times the master volume");
    const std::vector<int> velocities = VelocitiesOf(table);
    Check(velocities.size() == 1 && !volumes.empty() &&
              std::abs(velocities[0] - TableVelocity(100, volumes[0], true)) <= 0.5,
          "the velocity gives the note the table's level of the velocity times the volume that CC7 stands for");
    Check(velocities.size() == 1 && !volumes.empty() &&
              std::abs(TableMiss(velocities[0], volumes[0], 100.0 / 127.0 * channel)) < 0.15,
          "the note plays within 0.15 dB of the table's level of the whole product");
    Check(table.approximations.empty(), "a channel on the table reports no approximation");

    const std::vector<int> mixed_volumes = VolumesOf(mixed);
    const std::vector<int> mixed_velocities = VelocitiesOf(mixed);
    Check(mixed_volumes == std::vector<int>{static_cast<int>(std::lround(127.0 * channel))},
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
    const double bpm = 120.0 * musyx::kOutputRate / musyx::kNominalRate;
    Check(!perf.tempo.empty() && std::abs(perf.tempo[0].bpm - bpm) < 0.01,
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

    const double bpm = 49920.0 / 65536.0 * 1000.0 * 60.0 / 384.0 * musyx::kOutputRate / musyx::kNominalRate;
    Check(!perf.tempo.empty() && std::abs(perf.tempo[0].bpm - bpm) < 0.01,
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

// A zone's attenuation goes into a modulator without a source, not into the initialAttenuation generator. FluidSynth
// applies only 0.4 of the generator. Program 1 plays a copy of the macro with a ScaleVolume of 64/127. On the DLS
// curve, the zone of the copy then plays at (64/127)², 119 centibels down. The zone of program 0 plays at full level
// and gets no such modulator.
void TestMusyxZoneAttenuation()
{
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = TestSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[2];
    macro.insert(macro.begin() + 1, Step(MacroOp::kScaleVolume, {64, 0, 0, 0, 1}));
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, TestVolumes());
    ntdlib::musyx::Song song = TestSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 240, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 288, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});

    const Conversion c = musyx::ConvertSong(song, {"quiet", "G/A/A", 5, 127}, instruments, {});
    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);

    std::size_t sample_zones = 0;
    std::size_t generators = 0;
    std::vector<int> amounts;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (!gens.contains(seqlib::sf2::kSampleId))
        {
            continue;
        }

        sample_zones++;
        generators += gens.count(seqlib::sf2::kInitialAttenuation);
        for (const std::array<int, 5>& m : ZoneModulators(records, zone))
        {
            if (m[0] == seqlib::sf2::kNoController && m[1] == seqlib::sf2::kInitialAttenuation)
            {
                Check(m[3] == seqlib::sf2::kNoController && m[4] == seqlib::sf2::kLinear,
                      "the modulator has no amount source and a linear transform");
                amounts.push_back(m[2]);
            }
        }
    }

    Check(sample_zones == 2, "each program has a zone: " + std::to_string(sample_zones));
    Check(generators == 0, "no zone has an initialAttenuation generator");
    Check(amounts == std::vector<int>{119}, "only the zone of the quieter macro gets a modulator, of 119 centibels");
    Check(ntdlib::testing::CheckConversion(c, "quiet") == 0, "independent readers accept the files");
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
    TestMusyxSongVolume();
    TestMusyxVolumeTable();
    TestMusyxTableLevels();
    TestMusyxSong();
    TestMusyxIntegerTickStep();
    TestMusyxZoneAttenuation();
    TestMusyxTempoTable();
    TestMusyxPitchWheelRange();
    TestMusyxMixedPitchWheelRanges();

    if (argc > 1)
    {
        TestGameConvertsEverySequence(argv[1]);
    }

    return xsflib::testing::Summary("dolphinsf2");
}
