// SPDX-License-Identifier: MIT

// A MusyX game's songs and song groups (see musyx_game.h).

#include "musyx_game.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include "ntdlib/disc/error.h"
#include "ntdlib/disc/fsys.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "musyx_msm.h"
#include "musyx_puzzle.h"

namespace dolphinsf2::musyx
{

// What the code of a game with Hudson's sound manager says about its music: the sound manager's file, and the sound
// test that lists the songs. `sound_test_entries` entries start at offset `sound_test` of the code module's file
// `module`. The message file `messages` holds the songs' names. Every kind of rules gives the address of MusyX's volume
// table in main.dol and the game's master volume for music.
struct SoundManagerRules
{
    std::string_view dol_sha1;
    const char* sound_file = nullptr;
    const char* module = nullptr;
    uint32_t sound_test = 0;
    uint32_t sound_test_entries = 0;
    const char* messages = nullptr;
    uint32_t volume_table = 0;
    uint8_t master_volume = 127;
};

// What the code of a game that keeps its music in FSYS archives says about it. The module `module` in the archive
// `common_archive` holds the game's table of `sound_count` sounds at offset `sounds` of the module's file, and the
// table of its songs' `file_count` files at `files` (see ReadSoundTable). The archive `songs_archive` holds the songs.
// Each set in `sets` holds a song group. `common_archive` holds the set's project, pool and sample directory under the
// set's name with _proj, _pool and _sdir. The disc's folder `sample_folder` holds its samples, under the set's name
// with .samp. The game plays every song at `volume`.
struct FsysRules
{
    std::string_view dol_sha1;
    const char* common_archive = nullptr;
    const char* module = nullptr;
    uint32_t sounds = 0;
    uint32_t sound_count = 0;
    uint32_t files = 0;
    uint32_t file_count = 0;
    const char* songs_archive = nullptr;
    std::array<const char*, 2> sets{};
    const char* sample_folder = nullptr;
    uint8_t volume = 127;
    uint32_t volume_table = 0;
    uint8_t master_volume = 127;
};

// What the code of a game with Intelligent Systems' sound layer says about its music. `project` names the four files of
// its MusyX project without their extensions: the project (.proj), the pool (.pool), the sample directory (.sdir) and
// the samples (.samp). The file `songs` holds `song_count` songs back to back. The table at `song_sizes` gives their
// sizes. The sound room's table at `sound_room` lists `sound_room_entries` of the songs (ReadSoundRoom).
struct SoundRoomRules
{
    std::string_view dol_sha1;
    const char* project = nullptr;
    const char* songs = nullptr;
    uint32_t song_sizes = 0;
    uint32_t song_count = 0;
    uint32_t sound_room = 0;
    uint32_t sound_room_entries = 0;
    uint32_t volume_table = 0;
    uint8_t master_volume = 127;
};

// What the code of a game that lists the paths of its songs' files says about its music. The table at `song_paths` in
// main.dol holds the addresses of `song_count` paths, in the order of the songs' IDs (ReadSongPaths). A song takes the
// name of its file. `project` names the four files of the game's MusyX project without their extensions: the project
// (.prj), the pool (.pol), the sample directory (.sdr) and the samples (.smp). The project's group `group` plays every
// song at `volume`.
struct PathTableRules
{
    std::string_view dol_sha1;
    uint32_t song_paths = 0;
    uint32_t song_count = 0;
    const char* project = nullptr;
    uint16_t group = 0;
    uint8_t volume = 127;
    uint32_t volume_table = 0;
    uint8_t master_volume = 127;
    TickStep tick_step = TickStep::kFloat;
};

// What the code of Nintendo Puzzle Collection says about its music (musyx_puzzle.h).
struct PuzzleRules
{
    std::string_view dol_sha1;
    uint32_t volume_table = 0;
    uint8_t master_volume = 127;
};

namespace
{

using ntdlib::disc::FsysFile;
using xsflib::FormatError;

// What a game's code says about its music: the number of its song group's first file, and its lists of songs. The
// group's four files come one after another: the pool, the project, the sample directory and the samples. A list
// with no entries stands for none.
struct Rules
{
    std::string_view dol_sha1;
    uint32_t group_file = 0;
    std::array<SongList, 2> lists;
    uint32_t volume_table = 0;
    uint8_t master_volume = 127;
};

// The games that dolphinsf2 converts.
//
// Mario Golf: Toadstool Tour (USA). Its 90 groups are in 360 files: each group's pool, project, sample directory and
// samples, one after another. The song group, 0x59, plays every song. The code lists 47 songs in two lists of 16-byte
// entries: the address of the song's name, then the three words. The sound set-up (0x80014088) calls 0x8001307c with a
// master volume of 127 (at 0x800140ac). That function passes it to sndMasterVolume (0x8007394c) for the music.
//
// Mario Power Tennis (USA). Its 66 groups are in files of the same kind. The song group is 0x40. The code lists 111
// songs in entries of 0x24 bytes: the name in the first 0x18 bytes, then the three words. The sound set-up calls
// 0x8001a8d4 with a volume of 127 (at 0x80019f6c). That function multiplies the volume by 0.8 and truncates the
// product. It passes 101 to sndMasterVolume (0x800a4ee4) for the music.
constexpr std::array<Rules, 2> kGames{{
    {"7fa79da5a7cc3159d4af8c99cb38a971080bdbb5",
     4650,
     {{{0x80137784, 38, 0x10, false}, {0x801379e4, 9, 0x10, false}}},
     0x8015c608,
     127},
    {"beae7cba9bc987de9c9962bf0d273143aa4c8be6", 746, {{{0x802165e8, 111, 0x24, true}, {}}}, 0x802220b8, 101},
}};

// The games with Hudson's sound manager that dolphinsf2 converts.
//
// Mario Party 4 (USA, disc version 1). The sound manager's file holds 127 groups and 78 songs. The options module's
// sound test lists 76 of the songs on 14 pages of 10 entries. Bank 47 of the board messages holds their names. The
// sound manager calls sndMasterVolume (0x800fa09c) only with 0, to silence the music at the game's resets (0x8005adb8
// and 0x8005b0d8). dolphinsf2 plays the music at a master volume of 127.
constexpr std::array<SoundManagerRules, 1> kSoundManagerGames{{
    {"64b42eac733a57a53d2ce25e7e47f54fc1e31915", "sound/mpgcsnd.msm", "dll/option.rel", 0xac58, 140, "mess/board_e.dat",
     0x8013fb20, 127},
}};

// The games that keep their music in FSYS archives that dolphinsf2 converts.
//
// Pokémon Colosseum (USA). The module common_rel in common.fsys holds the game's table of 1,236 sounds at 0x141654 and
// the table of its songs' 79 files at 0x1133d8. bgm_archive.fsys holds 78 songs. The music has two song groups: group
// 0 in the files of snd_music, and group 1 in those of snd_music_atmos. The table gives no song a volume. Every song
// plays at full volume. The game calls sndMasterVolume (0x8014d928) through 0x80166d18 with 127, or with 0 to silence
// the music (at 0x8000c648, 0x8000c660, 0x8000c6ac and 0x8000c6c4).
constexpr std::array<FsysRules, 1> kFsysGames{{
    {"870e8b9693ca780782d80f22a6a4572d8ba9458f",
     "common.fsys",
     "common_rel",
     0x141654,
     1236,
     0x1133d8,
     79,
     "bgm_archive.fsys",
     {"snd_music", "snd_music_atmos"},
     "sound/",
     0x7f,
     0x80369a68,
     127},
}};

// A table of sounds' entries, the table of files' entries, and an entry's bit for a sound effect.
constexpr std::size_t kSoundEntry = 12;
constexpr std::size_t kFileEntry = 8;
constexpr uint8_t kEffect = 0x80;

// The games with Intelligent Systems' sound layer that dolphinsf2 converts.
//
// WarioWare, Inc.: Mega Party Game$! (USA). Its MusyX project holds 12 groups: 9 of songs and 3 of effects. The game
// loads all of them at once. sound_data.bin holds the game's 1,606 songs. The songs of the sound effects are among
// them. The sound room lists the 231 songs of the game's music. The sound set-up calls sndMasterVolume (0x800cdd48)
// with 100 for the music (at 0x800332a8).
constexpr std::array<SoundRoomRules, 1> kSoundRoomGames{{
    {"422e40ca1283a91e9f3c6a8e5da6ae82639b1e02", "gcwario_sound", "sound_data.bin", 0x80264a94, 1606, 0x801b2c38, 231,
     0x8026f6f8, 100},
}};

// The games whose code lists the paths of their songs' files that dolphinsf2 converts.
//
// The NDDEMO disc. The disc never went on sale. Its code lists the paths of its nine songs at 0x80065394. The disc's
// MusyX project, audio/nddemo, holds two groups: group 0 with the songs' instruments and group 1 with the effects. The
// set-up (0x80026b00) pushes both. The song start (0x80027500) plays a song through sndSeqPlayEx (0x800505d8) with
// group 0. It then sets the song's volume to 127 at once with sndSeqVolume (0x80047cec). The disc's code keeps the
// master volume for music at 127 (0x80026a90). Its menu of scenes (0x80007b20) fades the music to 64 through
// sndMasterVolume (0x80047ef8). Each scene starts its song and fades the master volume back to 127 over a second. The
// disc links an early build of MusyX (TickStep::kInteger). That build's salCalcVolume (0x800538e8) takes the volume
// table at 0x8006a0f0, or the DLS table at 0x80069eb4.
constexpr std::array<PathTableRules, 1> kPathTableGames{{
    {"d46883ce81a884b65c6e40d483b196add1dd1bf4", 0x80065394, 9, "audio/nddemo", 0, 127, 0x8006a0f0, 127,
     TickStep::kInteger},
}};

// The games that keep their music in modules of their code that dolphinsf2 converts.
//
// Nintendo Puzzle Collection (Japan). main.dol holds the OS, the SDK's libraries and MusyX 2.0. Its module loader
// (0x80006a10) loads the menu and the three games as modules of code (musyx_puzzle.h). The sound set-up (0x80006d5c)
// calls sndInit for 64 voices. dolphinsf2 plays the music at MusyX's default master volume of 127. MusyX's
// salCalcVolume (0x8002fb78) takes the volume table at 0x800bf7e8, or the DLS table at 0x800bf5cc.
constexpr std::array<PuzzleRules, 1> kPuzzleGames{{
    {"00ab0f4fd4cff96b9087ed1176f9a6e839b57dde", 0x800bf7e8, 127},
}};

// The longest name that a list's entry gives.
constexpr std::size_t kLongestName = 64;

// The size of an entry of a sound room's table, and the offset of the entry's English name.
constexpr uint32_t kSoundRoomEntrySize = 0x20;
constexpr uint32_t kEnglishName = 0xc;

// The bytes with which a sound room's names mark each space.
constexpr std::string_view kSoundRoomSpace = "\x94\xbc";

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

// The rules in `games` of the game whose main.dol has the SHA-1 `sha1`. Return nullptr for a game that `games` leaves
// out.
template <typename GameRules, std::size_t N>
const GameRules* RulesOf(const std::array<GameRules, N>& games, std::string_view sha1)
{
    const auto matches = [sha1](const GameRules& r)
    {
        return r.dol_sha1 == sha1;
    };
    const auto rules = std::find_if(games.begin(), games.end(), matches);

    return rules != games.end() ? &*rules : nullptr;
}

// The samples that the macros of `pool` start.
std::set<uint16_t> StartedSamples(const ntdlib::musyx::Pool& pool)
{
    std::set<uint16_t> samples;
    for (const auto& [id, steps] : pool.macros)
    {
        for (const ntdlib::musyx::MacroStep& step : steps)
        {
            if (step.Op() == ntdlib::musyx::MacroOp::kStartSample)
            {
                samples.insert(step.Half(1));
            }
        }
    }

    return samples;
}

// The song group whose MusyX ID is `id` in the sound manager's file `file`: the group of its project, with its pool,
// its sample directory and its samples. MusyX finds a sample among the samples of every group that it has loaded. A
// group's macros can therefore start samples of the base groups, which stay loaded. `path` is the file's path on the
// disc, for the errors. Throw FormatError if the parts can't be read or the project has no group of songs with that
// ID.
std::shared_ptr<const SongGroup> ReadSoundManagerGroup(const MsmFile& file, uint16_t id, const std::string& path)
{
    const MsmGroup parts = file.Group(id);
    std::vector<ntdlib::musyx::Group> groups = ntdlib::musyx::ReadProject(parts.project);
    const auto is_songs = [id](const ntdlib::musyx::Group& g)
    {
        return g.id == id && g.type == ntdlib::musyx::GroupType::kSongs;
    };
    const auto songs = std::find_if(groups.begin(), groups.end(), is_songs);
    if (songs == groups.end())
    {
        throw FormatError(path + "'s group " + std::to_string(id) + " has no song group");
    }

    auto group = std::make_shared<SongGroup>();
    group->group = std::move(*songs);
    group->pool = ntdlib::musyx::ReadPool(parts.pool);
    group->samples = ntdlib::musyx::ReadSampleDirectory(parts.directory);
    group->sample_data.assign(parts.samples.begin(), parts.samples.end());

    // The samples that the group's macros start and its directory lacks.
    std::set<uint16_t> missing = StartedSamples(group->pool);
    for (const ntdlib::musyx::SampleInfo& sample : group->samples)
    {
        missing.erase(sample.id);
    }

    // The samples of each base group that holds one of them go after the group's samples. The base group's directory
    // entries that the group lacks go into the group's directory.
    for (const MsmGroup& base : file.BaseGroups())
    {
        std::vector<ntdlib::musyx::SampleInfo> samples = ntdlib::musyx::ReadSampleDirectory(base.directory);
        const auto lacking = [&missing](const ntdlib::musyx::SampleInfo& s)
        {
            return missing.contains(s.id);
        };
        if (std::none_of(samples.begin(), samples.end(), lacking))
        {
            continue;
        }

        const auto offset = static_cast<uint32_t>(group->sample_data.size());
        group->sample_data.insert(group->sample_data.end(), base.samples.begin(), base.samples.end());
        for (ntdlib::musyx::SampleInfo& sample : samples)
        {
            const auto held = [&sample](const ntdlib::musyx::SampleInfo& s)
            {
                return s.id == sample.id;
            };
            if (std::none_of(group->samples.begin(), group->samples.end(), held))
            {
                sample.offset += offset;
                missing.erase(sample.id);
                group->samples.push_back(sample);
            }
        }
    }

    return group;
}

// The files of the FSYS archive `archive`, the disc's file `path`. Throw FormatError if the archive is damaged.
std::vector<FsysFile> FsysFiles(std::span<const uint8_t> archive, const std::string& path)
{
    try
    {
        return ntdlib::disc::ReadFsys(archive);
    }
    catch (const ntdlib::disc::Error& e)
    {
        throw FormatError(path + ": " + e.what());
    }
}

// The file named `name` among `files`, the files of the archive `archive`. Throw FormatError if the archive has none.
const FsysFile& FsysFileNamed(const std::vector<FsysFile>& files, const std::string& name, const std::string& archive)
{
    const auto named = [&name](const FsysFile& f)
    {
        return f.name == name;
    };
    const auto file = std::find_if(files.begin(), files.end(), named);
    if (file == files.end())
    {
        throw FormatError(archive + " has no file " + name);
    }

    return *file;
}

// The song group of the set `set` in a game that keeps its music in FSYS archives: the first group of songs in the
// set's project, with the set's pool and sample directory, from `common`, the files of the archive `archive`, and the
// set's samples `samples`. Throw FormatError if the files can't be read or the project has no group of songs.
std::shared_ptr<const SongGroup> ReadSetGroup(const std::vector<FsysFile>& common, const std::string& archive,
                                              const std::string& set, std::vector<uint8_t> samples)
{
    const std::string project = set + "_proj";
    std::vector<ntdlib::musyx::Group> groups = ntdlib::musyx::ReadProject(FsysFileNamed(common, project, archive).data);
    const auto is_songs = [](const ntdlib::musyx::Group& g)
    {
        return g.type == ntdlib::musyx::GroupType::kSongs;
    };
    const auto songs = std::find_if(groups.begin(), groups.end(), is_songs);
    if (songs == groups.end())
    {
        throw FormatError(archive + "'s " + project + " has no song group");
    }

    auto group = std::make_shared<SongGroup>();
    group->group = std::move(*songs);
    group->pool = ntdlib::musyx::ReadPool(FsysFileNamed(common, set + "_pool", archive).data);
    group->samples = ntdlib::musyx::ReadSampleDirectory(FsysFileNamed(common, set + "_sdir", archive).data);
    group->sample_data = std::move(samples);

    return group;
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

std::vector<TableSong> ReadSoundTable(std::span<const uint8_t> module, const std::string& name, uint32_t sounds,
                                      uint32_t sound_count, uint32_t files, uint32_t file_count)
{
    // Both tables lie inside the module's file.
    const auto inside = [&module](uint32_t offset, std::size_t size)
    {
        return offset <= module.size() && size <= module.size() - offset;
    };
    if (!inside(sounds, std::size_t{sound_count} * kSoundEntry))
    {
        throw FormatError(name + "'s table of sounds runs past the file's end");
    }
    if (!inside(files, std::size_t{file_count} * kFileEntry))
    {
        throw FormatError(name + "'s table of files runs past the file's end");
    }

    std::vector<TableSong> songs;
    for (uint32_t i = 0; i < sound_count; i++)
    {
        const uint8_t* entry = module.data() + sounds + std::size_t{i} * kSoundEntry;
        if ((entry[0] & kEffect) != 0)
        {
            continue;
        }

        const uint8_t file = entry[3];
        if (file >= file_count)
        {
            throw FormatError(name + "'s sound " + std::to_string(i) + " names no file");
        }

        const uint32_t id = xsflib::LoadBe32(module.data() + files + std::size_t{file} * kFileEntry);
        if (id != 0)
        {
            songs.push_back({.sound = i, .group = entry[1], .id = xsflib::LoadBe16(entry + 4), .file = id});
        }
    }

    return songs;
}

std::vector<SoundRoomSong> ReadSoundRoom(const ntdlib::Dol& dol, uint32_t address, uint32_t count)
{
    std::vector<SoundRoomSong> songs;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = address + i * kSoundRoomEntrySize;
        const uint32_t id = DolWord(dol, entry);
        SoundRoomSong song;
        song.number = static_cast<uint16_t>(id);
        song.group_id = static_cast<uint16_t>(id >> 24);
        song.volume = DolBytes(dol, entry + 4, 1)[0];
        song.name = DolText(dol, DolWord(dol, entry + kEnglishName), kLongestName);
        for (std::size_t at = song.name.find(kSoundRoomSpace); at != std::string::npos;
             at = song.name.find(kSoundRoomSpace, at + 1))
        {
            song.name.replace(at, kSoundRoomSpace.size(), " ");
        }
        songs.push_back(std::move(song));
    }

    return songs;
}

std::vector<std::string> ReadSongPaths(const ntdlib::Dol& dol, uint32_t address, uint32_t count)
{
    std::vector<std::string> paths;
    for (uint32_t i = 0; i < count; i++)
    {
        paths.push_back(DolText(dol, DolWord(dol, address + i * 4), kLongestName));
    }

    return paths;
}

std::span<const uint8_t> SongIn(const ntdlib::Dol& dol, uint32_t sizes, uint32_t count, std::span<const uint8_t> songs,
                                uint16_t number)
{
    if (number >= count)
    {
        throw FormatError("the table of song sizes has no song " + std::to_string(number));
    }

    // A song starts where the songs before it end.
    uint64_t offset = 0;
    for (uint32_t i = 0; i < number; i++)
    {
        offset += DolWord(dol, sizes + i * 4);
    }
    if (offset >= songs.size())
    {
        throw FormatError("song " + std::to_string(number) + " starts past the end of the file of songs");
    }

    const uint64_t size = std::min<uint64_t>(DolWord(dol, sizes + number * 4u), songs.size() - offset);

    return songs.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}

std::optional<Game> Game::Open(std::shared_ptr<ntdlib::disc::GameFiles> disc)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(disc->dol));
    Game game;
    game.disc_ = std::move(disc);
    if (const SoundManagerRules* rules = RulesOf(kSoundManagerGames, sha1))
    {
        game.ReadVolumes(rules->volume_table);
        game.ReadSoundManager(*rules);
        return game;
    }

    if (const FsysRules* rules = RulesOf(kFsysGames, sha1))
    {
        game.ReadVolumes(rules->volume_table);
        game.ReadFsysGame(*rules);
        return game;
    }

    if (const SoundRoomRules* rules = RulesOf(kSoundRoomGames, sha1))
    {
        game.ReadVolumes(rules->volume_table);
        game.ReadSoundRoomGame(*rules);
        return game;
    }

    if (const PathTableRules* rules = RulesOf(kPathTableGames, sha1))
    {
        game.ReadVolumes(rules->volume_table);
        game.ReadPathTableGame(*rules);
        return game;
    }

    if (const PuzzleRules* rules = RulesOf(kPuzzleGames, sha1))
    {
        game.ReadVolumes(rules->volume_table);
        game.ReadPuzzleGame(*rules);
        return game;
    }

    const Rules* rules = RulesOf(kGames, sha1);
    if (rules == nullptr)
    {
        return std::nullopt;
    }

    game.ReadVolumes(rules->volume_table);
    game.groups_.push_back(game.ReadSongGroup(rules->group_file));

    // The code's lists of songs.
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(game.disc_->dol);
    for (const SongList& list : rules->lists)
    {
        std::vector<GameSong> songs = ReadSongList(dol, list);
        for (GameSong& song : songs)
        {
            song.master_volume = rules->master_volume;
        }

        game.songs_.insert(game.songs_.end(), std::make_move_iterator(songs.begin()),
                           std::make_move_iterator(songs.end()));
    }

    return game;
}

std::vector<uint8_t> Game::ReadSong(std::size_t index) const
{
    if (!song_data_.empty())
    {
        return song_data_.at(index);
    }

    return ReadFile(songs_.at(index).path);
}

void Game::ReadVolumes(uint32_t address)
{
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc_->dol);
    const std::span<const uint8_t> bytes = dol.At(address);
    if (bytes.size() < volumes_.size() * 4)
    {
        throw FormatError("main.dol has no data at the volume table's address");
    }

    for (std::size_t i = 0; i < volumes_.size(); i++)
    {
        volumes_[i] = std::bit_cast<float>(xsflib::LoadBe32(bytes.data() + 4 * i));
    }
    if (volumes_.front() != 0.0f || volumes_[127] != 1.0f || volumes_.back() != 1.0f ||
        !std::is_sorted(volumes_.begin(), volumes_.end()))
    {
        throw FormatError("main.dol's volume table doesn't rise from 0 to 1");
    }
}

void Game::ReadSoundManager(const SoundManagerRules& rules)
{
    const std::vector<uint8_t> file = ReadFile(rules.sound_file);
    const std::vector<uint8_t> module = ReadFile(rules.module);
    const std::vector<uint8_t> messages = ReadFile(rules.messages);
    const MsmFile sound_file(file, rules.sound_file);

    // The index in groups_ of each song group, by the group's MusyX ID. Each group is read once.
    std::map<uint16_t, std::size_t> indexes;
    for (const SoundTestSong& listed :
         ReadSoundTest(module, rules.module, rules.sound_test, rules.sound_test_entries, messages, rules.messages))
    {
        const MsmSong song = sound_file.Song(listed.number);
        const auto [found, added] = indexes.try_emplace(song.group_id, groups_.size());
        if (added)
        {
            groups_.push_back(ReadSoundManagerGroup(sound_file, song.group_id, rules.sound_file));
        }

        songs_.push_back(
            {listed.name, rules.sound_file, song.song_id, song.volume, rules.master_volume, found->second});
        song_data_.emplace_back(song.data.begin(), song.data.end());
    }
}

void Game::ReadFsysGame(const FsysRules& rules)
{
    const std::vector<FsysFile> common = FsysFiles(ReadFile(rules.common_archive), rules.common_archive);
    const std::vector<FsysFile> archived = FsysFiles(ReadFile(rules.songs_archive), rules.songs_archive);
    const std::vector<uint8_t>& module = FsysFileNamed(common, rules.module, rules.common_archive).data;

    // The index in groups_ of each set's song group, by the group's MusyX ID.
    std::map<uint16_t, std::size_t> indexes;
    for (const char* set : rules.sets)
    {
        std::vector<uint8_t> samples = ReadFile(std::string(rules.sample_folder) + set + ".samp");
        std::shared_ptr<const SongGroup> group = ReadSetGroup(common, rules.common_archive, set, std::move(samples));
        indexes[group->group.id] = groups_.size();
        groups_.push_back(std::move(group));
    }

    // The table's songs, with their names and files from the songs' archive.
    for (const TableSong& listed :
         ReadSoundTable(module, rules.module, rules.sounds, rules.sound_count, rules.files, rules.file_count))
    {
        const std::string sound = std::string(rules.module) + "'s sound " + std::to_string(listed.sound);
        const auto is_file = [&listed](const FsysFile& f)
        {
            return f.id == listed.file;
        };
        const auto file = std::find_if(archived.begin(), archived.end(), is_file);
        if (file == archived.end())
        {
            throw FormatError(std::string(rules.songs_archive) + " has no file for " + sound);
        }

        const auto group = indexes.find(listed.group);
        if (group == indexes.end())
        {
            throw FormatError(sound + " names no song group");
        }

        songs_.push_back(
            {file->name, rules.songs_archive, listed.id, rules.volume, rules.master_volume, group->second});
        song_data_.push_back(file->data);
    }
}

void Game::ReadSoundRoomGame(const SoundRoomRules& rules)
{
    // The project's groups, and the pool, the sample directory and the samples that they share.
    const std::string project = rules.project;
    const std::vector<ntdlib::musyx::Group> groups = ntdlib::musyx::ReadProject(ReadFile(project + ".proj"));
    const ntdlib::musyx::Pool pool = ntdlib::musyx::ReadPool(ReadFile(project + ".pool"));
    const std::vector<ntdlib::musyx::SampleInfo> samples =
        ntdlib::musyx::ReadSampleDirectory(ReadFile(project + ".sdir"));
    const std::vector<uint8_t> sample_data = ReadFile(project + ".samp");
    const std::vector<uint8_t> song_file = ReadFile(rules.songs);
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc_->dol);

    // The index in groups_ of each song group, by the group's MusyX ID. Each group is read once. The game loads every
    // group. A group's macros can therefore start any sample of the project.
    std::map<uint16_t, std::size_t> indexes;
    for (SoundRoomSong& listed : ReadSoundRoom(dol, rules.sound_room, rules.sound_room_entries))
    {
        const auto [found, added] = indexes.try_emplace(listed.group_id, groups_.size());
        if (added)
        {
            const auto is_songs = [&listed](const ntdlib::musyx::Group& g)
            {
                return g.id == listed.group_id && g.type == ntdlib::musyx::GroupType::kSongs;
            };
            const auto songs = std::find_if(groups.begin(), groups.end(), is_songs);
            if (songs == groups.end())
            {
                throw FormatError(project + ".proj has no song group " + std::to_string(listed.group_id));
            }

            auto group = std::make_shared<SongGroup>();
            group->group = *songs;
            group->pool = pool;
            group->samples = samples;
            group->sample_data = sample_data;
            groups_.push_back(std::move(group));
        }

        const std::span<const uint8_t> song = SongIn(dol, rules.song_sizes, rules.song_count, song_file, listed.number);
        songs_.push_back(
            {std::move(listed.name), rules.songs, listed.number, listed.volume, rules.master_volume, found->second});
        song_data_.emplace_back(song.begin(), song.end());
    }
}

void Game::ReadPathTableGame(const PathTableRules& rules)
{
    // The project's song group, with the pool and the samples.
    const std::string project = rules.project;
    std::vector<ntdlib::musyx::Group> groups = ntdlib::musyx::ReadProject(ReadFile(project + ".prj"));
    const auto is_songs = [&rules](const ntdlib::musyx::Group& g)
    {
        return g.id == rules.group && g.type == ntdlib::musyx::GroupType::kSongs;
    };
    const auto songs = std::find_if(groups.begin(), groups.end(), is_songs);
    if (songs == groups.end())
    {
        throw FormatError(project + ".prj has no song group " + std::to_string(rules.group));
    }

    auto group = std::make_shared<SongGroup>();
    group->group = std::move(*songs);
    group->pool = ntdlib::musyx::ReadPool(ReadFile(project + ".pol"));
    group->samples = ntdlib::musyx::ReadSampleDirectory(ReadFile(project + ".sdr"));
    group->sample_data = ReadFile(project + ".smp");
    groups_.push_back(std::move(group));

    // The code's table of the songs' paths. A song's place in the table is its ID.
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc_->dol);
    const std::vector<std::string> paths = ReadSongPaths(dol, rules.song_paths, rules.song_count);
    for (std::size_t id = 0; id < paths.size(); id++)
    {
        const std::string& path = paths[id];
        const std::string file = path.substr(path.find_last_of('/') + 1);
        songs_.push_back({file.substr(0, file.find_last_of('.')), path, static_cast<uint16_t>(id), rules.volume,
                          rules.master_volume, 0, rules.tick_step});
    }
}

void Game::ReadPuzzleGame(const PuzzleRules& rules)
{
    PuzzleMusic music =
        ReadPuzzleCollection([this](const std::string& path) { return ReadFile(path); }, rules.master_volume);
    groups_ = std::move(music.groups);
    songs_ = std::move(music.songs);
    song_data_ = std::move(music.song_data);
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
