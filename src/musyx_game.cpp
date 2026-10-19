// SPDX-License-Identifier: MIT

// A MusyX game's songs and song group (see musyx_game.h).

#include "musyx_game.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

namespace dolphinsf2::musyx
{
namespace
{

using xsflib::FormatError;

// What a game's code says about its music: the number of its song group's first file, and its lists of songs. The
// group's four files come one after another: the pool, the project, the sample directory and the samples. A list
// with no entries stands for none. `level` multiplies every channel volume (CC7) of the game's songs.
struct Rules
{
    std::string_view dol_sha1;
    uint32_t group_file = 0;
    std::array<SongList, 2> lists;
    double level = 1.0;
};

// Mario Power Tennis plays its songs more quietly than MusyX's volume model gives, by a constant factor. The game links
// a later build of MusyX than Mario Golf, with another DSP program. The cause of the difference hasn't been found. This
// factor on each channel volume brings the songs to the game's level. Inside the squared volume curve, it lowers them
// by 40 log10(0.8064), about 3.7 dB.
constexpr double kMarioPowerTennisLevel = 0.8064;

// The games that dolphinsf2 converts.
//
// Mario Golf: Toadstool Tour (USA). Its 90 groups are in 360 files: each group's pool, project, sample directory and
// samples, one after another. The song group, 0x59, plays every song. The code lists 47 songs in two lists of 16-byte
// entries: the address of the song's name, then the three words.
//
// Mario Power Tennis (USA). Its 66 groups are in files of the same kind. The song group is 0x40. The code lists 111
// songs in entries of 0x24 bytes: the name in the first 0x18 bytes, then the three words.
constexpr std::array<Rules, 2> kGames{{
    {"7fa79da5a7cc3159d4af8c99cb38a971080bdbb5", 4650, {{{0x80137784, 38, 0x10, false}, {0x801379e4, 9, 0x10, false}}}},
    {"beae7cba9bc987de9c9962bf0d273143aa4c8be6", 746, {{{0x802165e8, 111, 0x24, true}, {}}}, kMarioPowerTennisLevel},
}};

// The longest name that a list's entry gives.
constexpr std::size_t kLongestName = 64;

// The `size` bytes at load address `address` of `dol`, main.dol as the game loads it. Throw FormatError if no section
// holds them.
std::span<const uint8_t> DolBytes(const ntdlib::Dol& dol, uint32_t address, uint32_t size)
{
    const std::span<const uint8_t> bytes = dol.At(address);
    if (bytes.size() < size)
    {
        throw FormatError("main.dol has no data at the song list's address");
    }

    return bytes.first(size);
}

uint32_t DolWord(const ntdlib::Dol& dol, uint32_t address)
{
    return xsflib::LoadBe32(DolBytes(dol, address, 4).data());
}

// The text that starts at load address `address` of `dol`, up to its zero byte, at most `longest` bytes.
std::string DolText(const ntdlib::Dol& dol, uint32_t address, std::size_t longest)
{
    std::string text;
    for (std::size_t i = 0; i < longest; i++)
    {
        const char c = static_cast<char>(DolBytes(dol, static_cast<uint32_t>(address + i), 1)[0]);
        if (c == '\0')
        {
            break;
        }
        text += c;
    }

    return text;
}

// The rules of the game whose main.dol is `dol`. Return nullptr for a game that dolphinsf2 doesn't convert.
const Rules* RulesOf(std::span<const uint8_t> dol)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(dol));
    const auto matches = [&sha1](const Rules& r)
    {
        return r.dol_sha1 == sha1;
    };
    const auto rules = std::find_if(kGames.begin(), kGames.end(), matches);

    return rules != kGames.end() ? &*rules : nullptr;
}

} // namespace

std::string PathOf(uint32_t number)
{
    const uint32_t n = number >> 1;
    std::string path;
    path += static_cast<char>('A' + n / (26 * 26) % 26);
    path += '/';
    path += static_cast<char>('A' + n / 26 % 26);
    path += '/';
    path += static_cast<char>('A' + n % 26);

    return path;
}

std::vector<GameSong> ReadSongList(const ntdlib::Dol& dol, const SongList& list)
{
    std::vector<GameSong> songs;
    for (uint32_t i = 0; i < list.count; i++)
    {
        const uint32_t entry = list.address + i * list.entry_size;
        const uint32_t words = entry + list.entry_size - 12;
        GameSong song;
        song.name =
            list.name_inline ? DolText(dol, entry, words - entry) : DolText(dol, DolWord(dol, entry), kLongestName);
        if (song.name.ends_with(".sng"))
        {
            song.name.resize(song.name.size() - 4);
        }
        song.path = PathOf(DolWord(dol, words));
        song.id = static_cast<uint16_t>(DolWord(dol, words + 4));
        song.volume = static_cast<uint8_t>(DolWord(dol, words + 8) >> 24);
        songs.push_back(std::move(song));
    }

    return songs;
}

std::optional<Game> Game::Open(std::shared_ptr<ntdlib::disc::GameFiles> disc)
{
    const Rules* rules = RulesOf(disc->dol);
    if (rules == nullptr)
    {
        return std::nullopt;
    }

    Game game;
    game.disc_ = std::move(disc);
    game.group_ = game.ReadSongGroup(rules->group_file);

    // The code's lists of songs.
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(game.disc_->dol);
    for (const SongList& list : rules->lists)
    {
        std::vector<GameSong> songs = ReadSongList(dol, list);
        for (GameSong& song : songs)
        {
            song.level = rules->level;
        }

        game.songs_.insert(game.songs_.end(), std::make_move_iterator(songs.begin()),
                           std::make_move_iterator(songs.end()));
    }

    return game;
}

std::vector<uint8_t> Game::ReadSong(std::size_t index) const
{
    return ReadFile(songs_.at(index).path);
}

std::shared_ptr<const SongGroup> Game::ReadSongGroup(uint32_t first_file) const
{
    // The group's four files. A file's number goes up by 2 from one file to the next (PathOf).
    auto group = std::make_shared<SongGroup>();
    const std::vector<uint8_t> pool = ReadFile(PathOf(first_file));
    const std::vector<uint8_t> project = ReadFile(PathOf(first_file + 2));
    const std::vector<uint8_t> directory = ReadFile(PathOf(first_file + 4));
    group->sample_data = ReadFile(PathOf(first_file + 6));

    // The project's first group of songs, with the pool and the samples.
    std::vector<ntdlib::musyx::Group> groups = ntdlib::musyx::ReadProject(project);
    const auto is_songs = [](const ntdlib::musyx::Group& g)
    {
        return g.type == ntdlib::musyx::GroupType::kSongs;
    };
    const auto songs = std::find_if(groups.begin(), groups.end(), is_songs);
    if (songs == groups.end())
    {
        throw FormatError(PathOf(first_file + 2) + " has no song group");
    }

    group->group = std::move(*songs);
    group->pool = ntdlib::musyx::ReadPool(pool);
    group->samples = ntdlib::musyx::ReadSampleDirectory(directory);

    return group;
}

std::vector<uint8_t> Game::ReadFile(const std::string& path) const
{
    std::vector<uint8_t> data;
    try
    {
        if (!disc_->read_file(path, data))
        {
            throw FormatError("can't read " + path);
        }
    }
    catch (const ntdlib::disc::Error& e)
    {
        throw FormatError(e.what());
    }

    return data;
}

} // namespace dolphinsf2::musyx
