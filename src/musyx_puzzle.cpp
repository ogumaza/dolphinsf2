// SPDX-License-Identifier: MIT

// Nintendo Puzzle Collection's music (see musyx_puzzle.h).

#include "musyx_puzzle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "xsflib/binary.h"

#include "musyx_game.h"
#include "rel_module.h"
#include "yay0.h"

namespace dolphinsf2::musyx
{
namespace
{

using xsflib::FormatError;
using ReadFile = std::function<std::vector<uint8_t>(const std::string& path)>;

// Where a module keeps a MusyX group's files: the group's ID, then the offsets of its project, its pool and its sample
// directory in one section of the module.
struct ModuleGroup
{
    uint16_t id = 0;
    uint32_t project = 0;
    uint32_t pool = 0;
    uint32_t directory = 0;
};

// The song group `id` of `project`, with its pool, its sample directory and its samples. `name` names the files in the
// errors. Throw FormatError if the project has no such group of songs.
SongGroup MakeSongGroup(std::span<const uint8_t> project, std::span<const uint8_t> pool,
                        std::span<const uint8_t> directory, std::vector<uint8_t> samples, uint16_t id,
                        const std::string& name)
{
    const std::vector<ntdlib::musyx::Group> groups = ntdlib::musyx::ReadProject(project);
    const ntdlib::musyx::Group* found = FindSongGroup(groups, id);
    if (found == nullptr)
    {
        throw FormatError(name + " has no song group " + std::to_string(id));
    }

    SongGroup group;
    group.group = *found;
    group.pool = ntdlib::musyx::ReadPool(pool);
    group.samples = ntdlib::musyx::ReadSampleDirectory(directory);
    group.sample_data = std::move(samples);

    return group;
}

// The song group of `group`'s files in section `section` of `module`, with the samples `samples`.
SongGroup ModuleSongGroup(const RelModule& module, uint32_t section, const ModuleGroup& group,
                          std::vector<uint8_t> samples, const std::string& name)
{
    return MakeSongGroup(module.Object(module.At(section, group.project)),
                         module.Object(module.At(section, group.pool)),
                         module.Object(module.At(section, group.directory)), std::move(samples), group.id, name);
}

// The objects and the samples of a group that a module keeps loaded while its songs play: the group's pool, its sample
// directory and its samples.
struct LoadedGroup
{
    ntdlib::musyx::Pool pool;
    std::vector<ntdlib::musyx::SampleInfo> samples;
    std::vector<uint8_t> sample_data;
};

// The loaded group of the pool `pool`, the sample directory `directory` and the samples `samples`.
LoadedGroup ReadLoadedGroup(std::span<const uint8_t> pool, std::span<const uint8_t> directory,
                            std::span<const uint8_t> samples)
{
    return {ntdlib::musyx::ReadPool(pool), ntdlib::musyx::ReadSampleDirectory(directory),
            std::vector<uint8_t>(samples.begin(), samples.end())};
}

// Give `group` the objects and the samples of `loaded` that it lacks. MusyX finds a voice's objects and samples in
// every group that the game has pushed. The samples of `loaded` go after those of `group`.
void AddLoadedGroup(SongGroup& group, const LoadedGroup& loaded)
{
    group.pool.macros.insert(loaded.pool.macros.begin(), loaded.pool.macros.end());
    group.pool.tables.insert(loaded.pool.tables.begin(), loaded.pool.tables.end());
    group.pool.keymaps.insert(loaded.pool.keymaps.begin(), loaded.pool.keymaps.end());
    group.pool.layers.insert(loaded.pool.layers.begin(), loaded.pool.layers.end());
    AddMissingSamples(group, loaded.samples, loaded.sample_data);
}

// Add a song to `music`.
void AddSong(PuzzleMusic& music, std::string name, std::string path, uint16_t id, uint8_t volume, uint8_t master_volume,
             std::size_t group, std::span<const uint8_t> data)
{
    music.songs.push_back({std::move(name), std::move(path), id, volume, master_volume, group, TickStep::kFloat});
    music.song_data.emplace_back(data.begin(), data.end());
}

// The menu's table of volumes: two rows of four words. The low byte of a word gives a volume.
constexpr std::size_t kMenuVolumeColumns = 4;
constexpr std::size_t kMenuVolumeRows = 2;

// The volume of the menu's song `id` of group `group`, from the menu's table of volumes `table`. The menu's code at
// 0x3a44 in section 1 picks the word by the song's group and ID.
uint8_t MenuVolume(std::span<const uint8_t> table, uint16_t group, uint16_t id)
{
    std::size_t row = 0;
    std::size_t column = kMenuVolumeColumns - 1;
    if (group == 1)
    {
        column = 2;
    }
    else if (group == 2 && id == 1)
    {
        row = 1;
        column = 1;
    }
    else if (group == 2 && (id == 2 || id == 3))
    {
        column = id == 2 ? 0 : 1;
    }
    else if (group == 3)
    {
        row = 1;
        column = id == 4 ? 0 : id == 5 ? 2 : kMenuVolumeColumns - 1;
    }

    return table[(row * kMenuVolumeColumns + column) * 4 + 3];
}

// The menu's songs. menu_relsamp.szp unpacks to four parts. Its first four words give their offsets: the menu's module,
// then the samples of groups 2, 0 and 4. A part runs up to the next part, and the last one up to the file's end. The
// module's section 4 holds a table of six pointers at 0x448. Pointer g points at the table of group g's songs, for g
// from 1 to 5. A group's table holds a pointer to each of its songs, at the song's ID. The menu's code gives the
// offsets of each group's files in section 4: at 0x56ec in section 1 for groups 0, 2 and 4, at 0x58b8 for group 1, at
// 0x5988 for group 3 and at 0x5a58 for group 5. Files in title/ hold the samples of groups 1, 3 and 5. Group 0 holds
// the menu's effects. The menu pushes group 0 at start-up and a song's group before the song. The song change at 0x3b68
// in section 1 starts the song. The song change then sets the volume of the songs' groups through sndVolume. The volume
// comes from the table at 0 in section 4 (MenuVolume). Group 0 stays loaded while the songs play. Its objects and
// samples serve every song. Nothing names the menu's songs. A song's name gives its ID in two digits, such as
// "menu-01". The songs come in the order of their IDs.
void AddMenu(PuzzleMusic& music, const ReadFile& read_file, uint8_t master_volume)
{
    constexpr char kFile[] = "menu_relsamp.szp";
    constexpr uint32_t kData = 4;
    constexpr uint32_t kVolumes = 0;
    constexpr uint32_t kSongTables = 0x448;
    constexpr std::size_t kParts = 4;

    // The files of the groups of songs, and the part of the file or the file that holds each group's samples.
    struct Group
    {
        ModuleGroup files;
        std::size_t part = 0;
        const char* samples = nullptr;
    };

    constexpr ModuleGroup kEffects{0, 0x111884, 0x1116c0, 0x111938};
    constexpr std::size_t kEffectsPart = 2;
    constexpr std::array<Group, 5> kGroups{{
        {{1, 0x135184, 0x133fd8, 0x1352b8}, 0, "title/Yoshi_title.samp"},
        {{2, 0x113d84, 0x111bc4, 0x113fec}, 1, nullptr},
        {{3, 0x133a50, 0x1339ac, 0x133b60}, 0, "title/agb_title.samp"},
        {{4, 0x11e284, 0x11d0d4, 0x11e42c}, 3, nullptr},
        {{5, 0x12842c, 0x125e4c, 0x128600}, 0, "title/grand_staff.samp"},
    }};

    const std::vector<uint8_t> file = UnpackYay0(read_file(kFile));
    const xsflib::Reader r(file, true);
    std::array<std::span<const uint8_t>, kParts> parts;
    for (std::size_t i = 0; i < kParts; i++)
    {
        const std::size_t start = r.U32(i * 4);
        const std::size_t end = i + 1 < kParts ? r.U32(i * 4 + 4) : file.size();
        if (end < start)
        {
            throw FormatError(std::string(kFile) + "'s parts overlap");
        }

        parts[i] = r.Sub(start, end - start);
    }

    const RelModule module(parts[0], kFile);
    const LoadedGroup effects =
        ReadLoadedGroup(module.Object(module.At(kData, kEffects.pool)),
                        module.Object(module.At(kData, kEffects.directory)), parts[kEffectsPart]);
    std::map<uint16_t, std::size_t> index_of_group;
    for (const Group& group : kGroups)
    {
        std::vector<uint8_t> samples = group.samples != nullptr
                                           ? read_file(group.samples)
                                           : std::vector<uint8_t>(parts[group.part].begin(), parts[group.part].end());
        SongGroup song_group = ModuleSongGroup(module, kData, group.files, std::move(samples), kFile);
        AddLoadedGroup(song_group, effects);
        index_of_group[group.files.id] = music.groups.size();
        music.groups.push_back(std::make_shared<const SongGroup>(std::move(song_group)));
    }

    // The songs, by their IDs.
    struct Song
    {
        uint16_t group = 0;
        uint16_t id = 0;
        std::size_t at = 0;
    };
    std::vector<Song> songs;
    for (const auto& [group, table] : module.TablePointers(module.At(kData, kSongTables)))
    {
        for (const auto& [id, at] : module.TablePointers(table))
        {
            songs.push_back({group, id, at});
        }
    }
    std::stable_sort(songs.begin(), songs.end(), [](const Song& a, const Song& b) { return a.id < b.id; });

    const std::span<const uint8_t> volumes =
        xsflib::Reader(parts[0], true).Sub(module.At(kData, kVolumes), kMenuVolumeRows * kMenuVolumeColumns * 4);
    for (const Song& song : songs)
    {
        const auto group = index_of_group.find(song.group);
        if (group == index_of_group.end())
        {
            throw FormatError(std::string(kFile) + " has no files for song " + std::to_string(song.id) + "'s group");
        }

        const std::string number = (song.id < 10 ? "0" : "") + std::to_string(song.id);
        AddSong(music, "menu-" + number, kFile, song.id, MenuVolume(volumes, song.group, song.id), master_volume,
                group->second, module.Object(song.at));
    }
}

// One of the games whose module holds its songs and its MusyX files: Dr. Mario and Panel de Pon. The game pushes groups
// 1 and 0 of the files. Its songs play in group 1. A song's ID is its place in the table of songs.
struct PuzzleGame
{
    const char* name = nullptr;         // the start of the songs' names
    const char* file = nullptr;         // the module's file
    uint32_t song_section = 0;          // the section that holds the table of songs
    uint32_t songs = 0;                 // the table's offset there
    uint32_t file_section = 0;          // the section that holds the MusyX files
    ModuleGroup files;                  // their offsets there
    const char* samples = nullptr;      // the file of the samples
    std::span<const char* const> names; // the songs' names, by their IDs
    uint32_t volumes = 0;               // the table of the songs' volumes in `song_section`, or 0 for none
};

// The names of Dr. Mario's songs, by their IDs: the songs' symbols in the debug module Dr_MARIO.plf, without the prefix
// "song_seq_". A name that ends in "f" names the fast form of the song before it.
constexpr std::array<const char*, 24> kDrMarioSongs{
    "fever",  "feverf",  "chil",         "chilf",   "game_c",  "game_cf", "game_d", "game_df",
    "game_e", "game_ef", "game_opening", "tile",    "menu",    "staff",   "end_a",  "end_b",
    "end_bs", "end_c",   "end_cs",       "story_a", "story_b", "story_c", "ending", "coffee"};

// Dr. Mario. dr_mario.szp unpacks to its module. The module's section 5 holds the table of songs at 0x10. Section 4
// holds the MusyX files: the project at 0x10e424, the pool at 0x1035b0 and the sample directory at 0x10f2f4.
// dr_test.samp holds the samples. During play, the songs play at full volume. The game lowers a song's volume during a
// pause (0x13df4 in section 1), in its manual and in some other states.
constexpr PuzzleGame kDrMario{"dr_mario",     "dr_mario.szp", 5, 0x10, 4, {1, 0x10e424, 0x1035b0, 0x10f2f4},
                              "dr_test.samp", kDrMarioSongs,  0};

// The names of Panel de Pon's songs, by their IDs: the songs' symbols in the debug module PANEPON.plf, without the "_"
// and the "_song" around them.
constexpr std::array<const char*, 59> kPanelDePonSongs{"flower_01",
                                                       "flower_02",
                                                       "jewel_01",
                                                       "jewel_02",
                                                       "water_01",
                                                       "water_02",
                                                       "sea_01",
                                                       "sea_02",
                                                       "cloud_01",
                                                       "cloud_02",
                                                       "moon_01",
                                                       "moon_02",
                                                       "green_01",
                                                       "green_02",
                                                       "fire_01",
                                                       "fire_02",
                                                       "ice_01",
                                                       "ice_02",
                                                       "prince_01",
                                                       "prince_02",
                                                       "lion_01",
                                                       "lion_02",
                                                       "dragon_01",
                                                       "dragon_02",
                                                       "joker_01",
                                                       "joker_02",
                                                       "sanatos_01",
                                                       "sanatos_02",
                                                       "goddess_01",
                                                       "goddess_02",
                                                       "whale_01",
                                                       "whale_02",
                                                       "opening",
                                                       "select",
                                                       "ending",
                                                       "ending_easymode",
                                                       "you_lose",
                                                       "you_win",
                                                       "intro_beginning",
                                                       "keyroad_part2",
                                                       "puzzle_edit",
                                                       "keyload_part1",
                                                       "goddess_appear",
                                                       "joker_apper",
                                                       "which_appear",
                                                       "ending_frog",
                                                       "obaba_01",
                                                       "obaba_02",
                                                       "tutorial",
                                                       "intro_storm",
                                                       "ending_whale_eyes",
                                                       "ending_which_photo",
                                                       "keyroad_part3",
                                                       "ending_staff_credits",
                                                       "ending_whale_gets_back_eyes",
                                                       "opening_skip",
                                                       "reslut",
                                                       "reserved1",
                                                       "reserved2"};

// Panel de Pon. panepon.szp unpacks to its module. The module's section 5 holds the table of songs at 0xbd0 and the
// MusyX files: the project at 0x20350, the pool at 0x8fc0 and the sample directory at 0x23010. panepon.samp holds the
// samples. The game plays a song through PlayMIDI (0xad230 in section 1). That function sets the song's volume from the
// table at 0x134148 in section 5. The table gives each song 4 bytes. The second byte gives the song's volume. A volume
// above 127 gives the game's default, 110 (0x8ba40 in section 1).
constexpr PuzzleGame kPanelDePon{"panepon",      "panepon.szp",    5,       0xbd0, 5, {1, 0x20350, 0x8fc0, 0x23010},
                                 "panepon.samp", kPanelDePonSongs, 0x134148};

// Add the songs of `game`.
void AddGame(PuzzleMusic& music, const ReadFile& read_file, uint8_t master_volume, const PuzzleGame& game)
{
    constexpr std::size_t kVolumeEntry = 4;
    constexpr uint8_t kDefaultVolume = 110;

    const std::vector<uint8_t> file = UnpackYay0(read_file(game.file));
    const RelModule module(file, game.file);
    const std::size_t group = music.groups.size();
    music.groups.push_back(std::make_shared<const SongGroup>(
        ModuleSongGroup(module, game.file_section, game.files, read_file(game.samples), game.file)));
    const xsflib::Reader r(file, true);
    for (const auto& [id, at] : module.TablePointers(module.At(game.song_section, game.songs)))
    {
        uint8_t volume = 127;
        if (game.volumes != 0)
        {
            volume = r.U8(module.At(game.song_section, game.volumes) + id * kVolumeEntry + 1);
            volume = volume > 127 ? kDefaultVolume : volume;
        }

        const std::string name = id < game.names.size() ? game.names[id] : std::to_string(id);
        AddSong(music, std::string(game.name) + "-" + name, game.file, id, volume, master_volume, group,
                module.Object(at));
    }
}

// Yoshi's Cookie's songs. ycookie.szp unpacks to the game's module. ycookie.bin holds the game's sound files as items.
// The table at 0xd16d0 in the module's section 5 gives each item's offset in units of 32 bytes. An item runs up to the
// next item's offset. Items 0 to 8 hold the project, the pool and the sample directory of groups 0, 1 and 2, in that
// order. Items 9 to 34 hold songs 0 to 25, and items 35 to 37 the samples of groups 0 to 2 (0x4574 in section 1). The
// table of songs at 0x17928c in section 5 gives each song 44 bytes: its group and its ID in big-endian words, then the
// path of its file in 32 bytes. A song's name gives the path's file name. Group 1 holds the game's effects. The game
// pushes groups 0 and 1 at start-up, and group 2 before a song of that group. A song of group 2 therefore finds objects
// and samples in all three groups. The songs play at the volume for music in the game's options. The table at 0x179b74
// in section 5 gives the volume of each of the options' 11 levels. The options start at level 5.
void AddYoshisCookie(PuzzleMusic& music, const ReadFile& read_file, uint8_t master_volume)
{
    constexpr char kFile[] = "ycookie.szp";
    constexpr char kItems[] = "ycookie.bin";
    constexpr uint32_t kData = 5;
    constexpr uint32_t kItemTable = 0xd16d0;
    constexpr uint32_t kSongTable = 0x17928c;
    constexpr uint32_t kLevels = 0x179b74;
    constexpr std::size_t kItemUnit = 32;
    constexpr std::size_t kSongEntry = 44;
    constexpr std::size_t kPath = 8;
    constexpr std::size_t kPathSize = 32;
    constexpr std::size_t kFirstLevel = 5;
    constexpr uint32_t kFirstSong = 9;
    constexpr uint32_t kSongCount = 26;
    constexpr uint32_t kFirstSamples = 35;
    constexpr std::array<uint16_t, 2> kSongGroups{0, 2};
    constexpr std::array<uint16_t, 2> kLoadedGroups{0, 1};

    const std::vector<uint8_t> file = UnpackYay0(read_file(kFile));
    const RelModule module(file, kFile);
    const std::vector<uint8_t> items = read_file(kItems);
    const xsflib::Reader r(file, true);
    const xsflib::Reader item_reader(items, true);
    const std::size_t offsets = module.At(kData, kItemTable);

    const auto item = [&](uint32_t number)
    {
        const std::size_t start = r.U32(offsets + number * 4) * kItemUnit;
        const std::size_t end = r.U32(offsets + number * 4 + 4) * kItemUnit;
        if (end < start)
        {
            throw FormatError(std::string(kItems) + "'s items overlap");
        }

        return item_reader.Sub(start, end - start);
    };

    // Groups 0 and 1 stay loaded. A song of group 2 finds objects in all three.
    std::map<uint16_t, std::size_t> index_of_group;
    for (const uint16_t g : kSongGroups)
    {
        const std::span<const uint8_t> samples = item(kFirstSamples + g);
        SongGroup group =
            MakeSongGroup(item(g * 3), item(g * 3 + 1), item(g * 3 + 2), {samples.begin(), samples.end()}, g, kItems);
        for (const uint16_t loaded : kLoadedGroups)
        {
            if (loaded != g)
            {
                AddLoadedGroup(
                    group, ReadLoadedGroup(item(loaded * 3 + 1), item(loaded * 3 + 2), item(kFirstSamples + loaded)));
            }
        }
        index_of_group[g] = music.groups.size();
        music.groups.push_back(std::make_shared<const SongGroup>(std::move(group)));
    }

    const uint8_t volume = r.U8(module.At(kData, kLevels) + kFirstLevel);
    const std::size_t songs = module.At(kData, kSongTable);
    for (uint32_t s = 0; s < kSongCount; s++)
    {
        const std::size_t entry = songs + s * kSongEntry;
        const uint32_t group = r.U32(entry);
        const uint32_t id = r.U32(entry + 4);
        const auto index = index_of_group.find(static_cast<uint16_t>(group));
        if (group > 0xffff || id > 0xffff || index == index_of_group.end())
        {
            throw FormatError(std::string(kFile) + "'s song " + std::to_string(s) + " has no group of songs");
        }

        const std::string path = r.CString(entry + kPath, kPathSize);
        const std::string name = path.substr(path.find_last_of('/') + 1);
        AddSong(music, "ycookie-" + name.substr(0, name.find_last_of('.')), kItems, static_cast<uint16_t>(id), volume,
                master_volume, index->second, item(kFirstSong + s));
    }
}

} // namespace

PuzzleMusic ReadPuzzleCollection(const ReadFile& read_file, uint8_t master_volume)
{
    PuzzleMusic music;
    AddMenu(music, read_file, master_volume);
    AddGame(music, read_file, master_volume, kDrMario);
    AddGame(music, read_file, master_volume, kPanelDePon);
    AddYoshisCookie(music, read_file, master_volume);

    return music;
}

} // namespace dolphinsf2::musyx
