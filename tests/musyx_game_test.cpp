// SPDX-License-Identifier: MIT

// Unit tests of the tables of the MusyX games: the song lists and the songs in one file. ntdlib's
// musyx_game_tables_test checks the paths of Camelot's files, the sound room and the played effects.

#include "musyx_game.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dol.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "xsflib/testing/check.h"

#include "musyx_convert.h"
#include "musyx_test_helpers.h"
#include "test_helpers.h"
#include "unit_tests.h"

namespace dolphinsf2::testing
{
namespace
{

using ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// The bytes of `text`, without a terminating zero.
std::vector<uint8_t> TextBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// Song lists in both of the games' forms. The first list's entries of 16 bytes give the address of each song's name.
// The second list's entry of 0x24 bytes contains its name in its first 0x18 bytes. Each entry ends with the song's file
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
    Check(past_the_data == "main.dol has no data at the song list",
          "a list past the DOL's data fails: " + past_the_data);
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

} // namespace

void RunMusyxGameTests()
{
    TestMusyxSongLists();
    TestSongsInOneFile();
}

} // namespace dolphinsf2::testing
