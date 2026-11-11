// SPDX-License-Identifier: MIT

// A MusyX game's songs and song groups (see musyx_game.h).

#include "musyx_game.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"
#include "ntdlib/musyx/msm.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "musyx_catalogue.h"

namespace dolphinsf2::musyx
{

using ntdlib::musyx::MsmEffect;
using ntdlib::musyx::MsmFile;
using ntdlib::musyx::MsmGroup;
using ntdlib::musyx::MsmSong;

// What the code of a game with Hudson's sound manager says about its music: the sound manager's file, and the sound
// test that lists the songs. `sound_test_entries` entries start at offset `sound_test` of the code module's file
// `module`. The message file `messages` holds the songs' names. Every kind of rules gives the address of MusyX's volume
// table in main.dol and the game's master volume for music. The module's list of `voice_entries` voices at `voices`
// names some of the effects (ReadSoundTestEffects). With `japanese_messages`, the messages take the game's Japanese
// font.
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
    uint32_t voices = 0;
    uint32_t voice_entries = 0;
    bool japanese_messages = false;
};

// What the code of a game with Intelligent Systems' sound layer says about its music. `project` names the four files of
// its MusyX project without their extensions: the project (.proj), the pool (.pool), the sample directory (.sdir) and
// the samples (.samp). The file `songs` holds `song_count` songs back to back. The table at `song_sizes` gives their
// sizes. The sound room's table at `sound_room` lists `sound_room_entries` of the songs (ReadSoundRoom). The tables of
// `effect_song_entries` songs at `effect_songs` (ReadEffectSongs) and of `effect_entries` effects at `effects`
// (ReadPlayedEffects) list the sounds that the game plays as effects. The master volume applies to both kinds.
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
    uint32_t effect_songs = 0;
    uint32_t effect_song_entries = 0;
    uint32_t effects = 0;
    uint32_t effect_entries = 0;
};

// What the code of a game by Camelot says about its effects. Each group of effects lies in four files. So does the
// song group (Rules). The game pushes the group whose first file is `startup_files` at start-up. The table at
// `ranges` gives the IDs of that group's effects that the game starts: pairs of words, the first ID and the last, up
// to a word of 0xFFFFFFFF. The table at `voice_banks` holds `voice_bank_count` entries of `voice_bank_size` bytes. An
// entry starts with the bank's name in `voice_bank_name_size` bytes, or with the name's address for a size of 0. The
// number of the bank's first file, the count of its effects, the first effect's ID and the group's ID follow, a word
// each. The table at `narrations` holds `narration_count` entries of 8 bytes: the number of the narration's first file
// in a word, then the group's ID and the effect's ID in a half each. A file of 0xFFFFFFFF marks a narration of the
// first group of effects in the files from `narration_files`. The table at `courts` holds `court_count` entries of 8
// bytes: the number of the first file of a court's group in a word, then the group's ID in the low half of a word. The
// effects of a court are those of its group. `other_group_count` groups that no table lists follow each other on the
// disc from the file numbered `other_groups`. Their effects are those of each project's first group of effects. The
// game starts each effect in volume groups at 127 and at the master volume `master_volume`. It then sets the effect's
// reverb send (CC91) to `reverb`.
struct CamelotEffectRules
{
    uint32_t startup_files = 0;
    uint32_t ranges = 0;
    uint32_t voice_banks = 0;
    uint32_t voice_bank_count = 0;
    uint32_t voice_bank_size = 0;
    uint32_t voice_bank_name_size = 0;
    uint32_t narrations = 0;
    uint32_t narration_count = 0;
    uint32_t narration_files = 0;
    uint32_t courts = 0;
    uint32_t court_count = 0;
    uint32_t other_groups = 0;
    uint32_t other_group_count = 0;
    uint8_t master_volume = 127;
    uint8_t reverb = 0;
};

namespace
{

using ntdlib::musyx::EffectName;
using xsflib::FormatError;

// What a game's code says about its music: the number of its song group's first file, and its lists of songs. The
// group's four files come one after another: the pool, the project, the sample directory and the samples. A list
// with no entries stands for none. The code also lists the game's effects.
struct Rules
{
    std::string_view dol_sha1;
    uint32_t group_file = 0;
    std::array<SongList, 2> lists;
    uint32_t volume_table = 0;
    uint8_t master_volume = 127;
    CamelotEffectRules effects;
};

// The games that dolphinsf2 converts.
//
// Mario Golf: Toadstool Tour (USA). Its 90 groups are in 360 files: each group's pool, project, sample directory and
// samples, one after another. The song group, 0x59, plays every song. The code lists 47 songs in two lists of 16-byte
// entries: the address of the song's name, then the three words. The sound set-up (0x80014088) calls 0x8001307c with a
// master volume of 127 (at 0x800140ac). That function passes it to sndMasterVolume (0x8007394c) for the music.
//
// Groups 0 to 88 hold Mario Golf's 737 effects. The bank loader (0x80012a9c) pushes group 87 from B/T/M with the song
// group. fx_play (0x80011b88) starts its effects. The table of voice banks at 0x80137308 holds 37 entries of 0x14
// bytes. JVoiceLoad1 (0x80012844) loads a bank, and voiceFXcall (0x800120a8) starts its effects. The narration player
// (0x80013be8) loads the group of an entry of the table at 0x801375ec and starts the entry's effect. Each one calls
// sndFXStartEx with a pan of 0xff and studio 0. The narration player and some other calls give a volume of 0xff. MusyX
// then takes the row's volume, and dolphinsf2 starts every effect at it. The sound set-up (0x80012ecc) gives every
// volume group a volume of 127 through sndVolume (0x800738ec). It then gives the music's groups 86. The master volume
// for effects stays at 127.
//
// Mario Power Tennis (USA). Its 66 groups are in files of the same kind. The song group is 0x40. The code lists 111
// songs in entries of 0x24 bytes: the name in the first 0x18 bytes, then the three words. The sound set-up calls
// 0x8001a8d4 with a volume of 127 (at 0x80019f6c). That function multiplies the volume by 0.8 and truncates the
// product. It passes 101 to sndMasterVolume (0x800a4ee4) for the music and for the effects.
//
// Its 980 effects lie in the other 65 groups. The bank loader (0x8001a214) pushes group 63 from A/O/F with the song
// group. The table of voice banks at 0x80215e98 holds 18 entries of 0x20 bytes, the bank's name in the first 16.
// JVoiceLoad1 (0x8001b77c) and voiceFXcall (0x8001ba3c) load and start them. NarrationInit (0x8001a31c) pushes group
// 65 from A/O/N. Each of the 106 entries of the table of narrations at 0x80216298 names an effect of that group. The
// court loader (0x8001a670) loads the group of an entry of the table at 0x80217828. The 37 groups from B/X/J to C/D/A
// hold 56 more effects. No table in main.dol lists them. Tables in the code and the data that the files A/O/E and
// C/I/L unpack to name them. The game starts every effect with studio 1 and a pan of 0xff. It then sets the effect's
// controller 0x5b to 0x40 through sndFXCtrl (0x800a4550). The sound set-up (0x80019d94) gives the volume groups the
// same volumes as Mario Golf's.
constexpr std::array<Rules, 2> kGames{{
    {"7fa79da5a7cc3159d4af8c99cb38a971080bdbb5",
     4650,
     {{{0x80137784, 38, 0x10, false}, {0x801379e4, 9, 0x10, false}}},
     0x8015c608,
     127,
     {.startup_files = 2364, // B/T/M
      .ranges = 0x801320cc,
      .voice_banks = 0x80137308,
      .voice_bank_count = 37,
      .voice_bank_size = 0x14,
      .narrations = 0x801375ec,
      .narration_count = 51}},
    {"beae7cba9bc987de9c9962bf0d273143aa4c8be6",
     746,
     {{{0x802165e8, 111, 0x24, true}, {}}},
     0x802220b8,
     101,
     {.startup_files = 738, // A/O/F
      .ranges = 0x802177e8,
      .voice_banks = 0x80215e98,
      .voice_bank_count = 18,
      .voice_bank_size = 0x20,
      .voice_bank_name_size = 16,
      .narrations = 0x80216298,
      .narration_count = 106,
      .narration_files = 754, // A/O/N
      .courts = 0x80217828,
      .court_count = 10,
      .other_groups = 2566, // B/X/J
      .other_group_count = 37,
      .master_volume = 101,
      .reverb = 0x40}},
}};

// The games with Hudson's sound manager that dolphinsf2 converts.
//
// Mario Party 4 (USA, disc version 1). The sound manager's file holds 127 groups and 78 songs. The options module's
// sound test lists 76 of the songs on 14 pages of 10 entries. Bank 47 of the board messages holds their names. The
// sound manager calls sndMasterVolume (0x800fa09c) only with 0, to silence the music at the game's resets (0x8005adb8
// and 0x8005b0d8). dolphinsf2 plays the music at a master volume of 127.
//
// The sound manager's table holds 2,308 entries of effects. 2,072 of them hold an effect. The options module's list of
// voices names 93 of them in 123 entries. The sound manager's set-up (0x801176ac) calls sndVolume(127, 0, 0xff).
// Every volume group plays at full volume. The game plays an effect through msmSePlay (0x80119a94). That function
// starts the effect through sndFXStartParaInfo with a volume and a pan of 0xff and studio 0. It passes the settings of
// the effect's entry as controllers (MsmEffect). It then sets the effect's surround pan.
//
// Mario Party 4 (USA, disc version 0). Its main.dol differs from disc version 1's only in three of the debugger's
// functions. The options module, the messages and the sound manager's file are disc version 1's byte for byte.
//
// Mario Party 4 (Europe, disc versions 0 and 2). Both discs hold the same main.dol. Its sound manager and MusyX are the
// American code at other addresses. msmSePlay is at 0x801198c4, and the volume table at 0x8015cec0. The options module
// holds the American sound test and list of voices 0x68 bytes lower. The English messages give the American names,
// apart from five. The sound manager's file is the American one byte for byte.
//
// Mario Party 4 (Japan). Its sound manager and MusyX are the American code at other addresses. msmSePlay is at
// 0x80119684, and the volume table at 0x8013f7c0. The options module holds the American sound test and list of voices
// at the American offsets. The messages name the sounds in the game's Japanese font. The sound manager's file leaves
// out two macros and two samples of group 0x5A. That group holds se-2066 to se-2071.
constexpr std::array<SoundManagerRules, 4> kSoundManagerGames{{
    {"64b42eac733a57a53d2ce25e7e47f54fc1e31915", "sound/mpgcsnd.msm", "dll/option.rel", 0xac58, 140, "mess/board_e.dat",
     0x8013fb20, 127, 0xb518, 123},
    {"ff8d894776d37167e454ae3f0c9e644407c1e6b1", "sound/mpgcsnd.msm", "dll/option.rel", 0xac58, 140, "mess/board_e.dat",
     0x8013fb20, 127, 0xb518, 123},
    {"65ef91650e9201047f50a3c39c7693a21fff2b29", "sound/mpgcsnd.msm", "dll/option.rel", 0xabf0, 140, "mess/board_e.dat",
     0x8015cec0, 127, 0xb4b0, 123},
    {"abe951fa239e7cb5de67e1ac091cfbc14010d6e0", "sound/mpgcsnd.msm", "dll/option.rel", 0xac58, 140, "mess/board.dat",
     0x8013f7c0, 127, 0xb518, 123, true},
}};

// The games with Intelligent Systems' sound layer that dolphinsf2 converts.
//
// WarioWare, Inc.: Mega Party Game$! (USA). Its MusyX project holds 12 groups: 9 of songs and 3 of effects. The game
// loads all of them at once. sound_data.bin holds the game's 1,606 songs. The songs of the sound effects are among
// them. The sound room lists the 231 songs of the game's music. The sound set-up calls sndVolume (0x800cdce8) with 127,
// 0 and 0xff (at 0x80033294). Every volume group plays at full volume. It calls sndMasterVolume (0x800cdd48) with 100
// for the music and the effects (at 0x800332a8).
//
// The game's play function (0x800348b8) takes a number. A number below 1,606 plays a song, at the volume of its entry
// in the sound room's table or in the table of 933 songs that play as effects at 0x801b4918. A number from 1,606 on
// starts MusyX's effect of the number less 1,606, with the settings of its entry in the table of 145 effects at
// 0x801b6640.
constexpr std::array<SoundRoomRules, 1> kSoundRoomGames{{
    {"422e40ca1283a91e9f3c6a8e5da6ae82639b1e02", "gcwario_sound", "sound_data.bin", 0x80264a94, 1606, 0x801b2c38, 231,
     0x8026f6f8, 100, 0x801b4918, 933, 0x801b6640, 145},
}};

// The longest name that a list's entry gives.
constexpr std::size_t kLongestName = 64;

// The size of an entry of a sound room's table, and the offset of the entry's English name.
constexpr uint32_t kSoundRoomEntrySize = 0x20;
constexpr uint32_t kEnglishName = 0xc;

// The bytes with which a sound room's names mark each space.
constexpr std::string_view kSoundRoomSpace = "\x94\xbc";

// The sizes of an entry of the table of songs that play as effects, and of the table of effects that a play function
// starts.
constexpr uint32_t kEffectSongEntrySize = 8;
constexpr uint32_t kPlayedEffectSize = 6;

// The rules in `games` of the game whose main.dol has the SHA-1 `sha1`. Return nullptr for a game that `games` leaves
// out.
template <typename GameRules>
const GameRules* RulesOf(std::span<const GameRules> games, std::string_view sha1)
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

// The group of `type` whose MusyX ID is `id` among the groups of `project`, a MusyX project, with the pool `pool`, the
// sample directory `directory` and the samples `samples`. Without `id`, the first group of that type. Throw FormatError
// with the message that `missing` makes from the kind of group, "song" or "effects", if the project has no such group.
std::shared_ptr<SongGroup> MakeSongGroup(std::span<const uint8_t> project, std::span<const uint8_t> pool,
                                         std::span<const uint8_t> directory, std::vector<uint8_t> samples,
                                         ntdlib::musyx::GroupType type, std::optional<uint16_t> id,
                                         const std::function<std::string(const char* kind)>& missing)
{
    const std::vector<ntdlib::musyx::Group> groups = ntdlib::musyx::ReadProject(project);
    const ntdlib::musyx::Group* found = FindGroup(groups, type, id);
    if (found == nullptr)
    {
        throw FormatError(missing(type == ntdlib::musyx::GroupType::kSongs ? "song" : "effects"));
    }

    auto group = std::make_shared<SongGroup>();
    group->group = *found;
    group->pool = ntdlib::musyx::ReadPool(pool);
    group->samples = ntdlib::musyx::ReadSampleDirectory(directory);
    group->sample_data = std::move(samples);

    return group;
}

// The group of `type` whose MusyX ID is `id` in the sound manager's file `file`: the group of its project, with its
// pool, its sample directory and its samples. MusyX finds a sample among the samples of every group that it has
// loaded. The base groups stay loaded. A group's macros can therefore start their samples. `path` is the file's path
// on the disc, for the errors. Throw FormatError if the parts can't be read or the project has no group of that type
// with that ID.
std::shared_ptr<const SongGroup> ReadSoundManagerGroup(const MsmFile& file, uint16_t id, ntdlib::musyx::GroupType type,
                                                       const std::string& path)
{
    const MsmGroup parts = file.Group(id);
    const auto no_group = [&path, id](const char* kind)
    {
        return path + "'s group " + std::to_string(id) + " has no " + kind + " group";
    };
    std::shared_ptr<SongGroup> group = MakeSongGroup(parts.project, parts.pool, parts.directory,
                                                     {parts.samples.begin(), parts.samples.end()}, type, id, no_group);

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
        const std::vector<ntdlib::musyx::SampleInfo> samples = ntdlib::musyx::ReadSampleDirectory(base.directory);
        const auto lacking = [&missing](const ntdlib::musyx::SampleInfo& s)
        {
            return missing.contains(s.id);
        };
        if (std::none_of(samples.begin(), samples.end(), lacking))
        {
            continue;
        }

        AddMissingSamples(*group, samples, base.samples);
        for (const ntdlib::musyx::SampleInfo& sample : samples)
        {
            missing.erase(sample.id);
        }
    }

    return group;
}

// The effects of a game by Camelot, by their IDs, and the groups that play them. Each group is read once, by its first
// file. The game has pushed the song group and the group of start-up before each group (AddPushedGroups). Of two
// tables that give an ID, the first that Game::ReadCamelotEffects reads wins.
class CamelotEffects
{
public:
    // Reads the effects group whose first file is `first_file` and whose MusyX ID is `id`, or the first effects group
    // of the files without `id`.
    using GroupReader = std::function<std::shared_ptr<SongGroup>(uint32_t first_file, std::optional<uint16_t> id)>;

    // The effects of the game whose groups are `groups`. The song group comes first there. `read_group` reads a group.
    // Each effect plays at the master volume `rules.master_volume` with the reverb send `rules.reverb`.
    CamelotEffects(std::vector<std::shared_ptr<const SongGroup>>& groups, GroupReader read_group,
                   const CamelotEffectRules& rules)
        : groups_(groups), read_group_(std::move(read_group)), rules_(rules), pushed_{groups.at(0)}
    {
    }

    // The index in `groups` of the group of start-up, whose first file is `first_file`. Its objects and samples come
    // before the song group's in each later group. Call it before GroupAt.
    std::size_t StartUp(uint32_t first_file)
    {
        const std::size_t startup = GroupAt(first_file, std::nullopt);
        pushed_.insert(pushed_.begin(), groups_[startup]);

        return startup;
    }

    // The index in `groups` of the group whose first file is `first_file` (see GroupReader).
    std::size_t GroupAt(uint32_t first_file, std::optional<uint16_t> id)
    {
        const auto [found, added] = indexes_.try_emplace(first_file, groups_.size());
        if (added)
        {
            std::shared_ptr<SongGroup> group = read_group_(first_file, id);
            AddPushedGroups(*group, pushed_);
            groups_.push_back(std::move(group));
        }

        return found->second;
    }

    // Adds the effect `id` of the group `group`, named after `prefix` and the ID. Adds nothing if the group has no row
    // for the effect that allows voices, or if an earlier table gave the effect.
    void Add(std::size_t group, uint32_t id, const std::string& prefix)
    {
        const ntdlib::musyx::Effect* row =
            id <= 0xffff ? FindEffect(groups_[group]->group, static_cast<uint16_t>(id)) : nullptr;
        if (row != nullptr && !effects_.contains(row->id))
        {
            effects_.emplace(row->id, GameEffect{.name = EffectName(prefix, row->id),
                                                 .group = group,
                                                 .row = *row,
                                                 .master_volume = rules_.master_volume,
                                                 .reverb = rules_.reverb});
        }
    }

    // Adds every effect of the group `group` (see Add).
    void AddAll(std::size_t group, const std::string& prefix)
    {
        for (const ntdlib::musyx::Effect& row : groups_[group]->group.effects)
        {
            Add(group, row.id, prefix);
        }
    }

    // The effects, in the order of their IDs.
    std::vector<GameEffect> Take()
    {
        std::vector<GameEffect> effects;
        for (auto& entry : effects_)
        {
            effects.push_back(std::move(entry.second));
        }

        return effects;
    }

private:
    std::vector<std::shared_ptr<const SongGroup>>& groups_;
    GroupReader read_group_;
    const CamelotEffectRules& rules_;
    std::vector<std::shared_ptr<const SongGroup>> pushed_;
    std::map<uint32_t, std::size_t> indexes_;
    std::map<uint16_t, GameEffect> effects_;
};

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
    constexpr char kList[] = "the song list";
    std::vector<GameSong> songs;
    for (uint32_t i = 0; i < list.count; i++)
    {
        const uint32_t entry = list.address + i * list.entry_size;
        const uint32_t words = entry + list.entry_size - 12;
        GameSong song;
        song.name = list.name_inline ? dol.Text(entry, words - entry, kList)
                                     : dol.Text(dol.Word(entry, kList), kLongestName, "a song's name");
        if (song.name.ends_with(".sng"))
        {
            song.name.resize(song.name.size() - 4);
        }
        song.path = PathOf(dol.Word(words, kList));
        song.id = static_cast<uint16_t>(dol.Word(words + 4, kList));
        song.volume = static_cast<uint8_t>(dol.Word(words + 8, kList) >> 24);
        songs.push_back(std::move(song));
    }

    return songs;
}

std::vector<SoundRoomSong> ReadSoundRoom(const ntdlib::Dol& dol, uint32_t address, uint32_t count)
{
    constexpr char kRoom[] = "the sound room";
    std::vector<SoundRoomSong> songs;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = address + i * kSoundRoomEntrySize;
        const uint32_t id = dol.Word(entry, kRoom);
        SoundRoomSong song;
        song.number = static_cast<uint16_t>(id);
        song.group_id = static_cast<uint16_t>(id >> 24);
        song.volume = dol.Bytes(entry + 4, 1, kRoom)[0];
        song.name = dol.Text(dol.Word(entry + kEnglishName, kRoom), kLongestName, "a song's name");
        for (std::size_t at = song.name.find(kSoundRoomSpace); at != std::string::npos;
             at = song.name.find(kSoundRoomSpace, at + 1))
        {
            song.name.replace(at, kSoundRoomSpace.size(), " ");
        }
        songs.push_back(std::move(song));
    }

    return songs;
}

std::vector<SoundRoomSong> ReadEffectSongs(const ntdlib::Dol& dol, uint32_t address, uint32_t count)
{
    std::vector<SoundRoomSong> songs;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::span<const uint8_t> entry = dol.Bytes(address + i * kEffectSongEntrySize, kEffectSongEntrySize,
                                                         "the table of songs that play as effects");
        SoundRoomSong song;
        song.number = xsflib::LoadBe16(entry.data() + 2);
        song.group_id = entry[0];
        song.volume = entry[4];
        song.name = EffectName("se", song.number);
        songs.push_back(std::move(song));
    }

    return songs;
}

std::vector<PlayedEffect> ReadPlayedEffects(const ntdlib::Dol& dol, uint32_t address, uint32_t count)
{
    std::vector<PlayedEffect> effects;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::span<const uint8_t> entry =
            dol.Bytes(address + i * kPlayedEffectSize, kPlayedEffectSize, "the table of effects");
        effects.push_back(
            {.number = xsflib::LoadBe16(entry.data()), .volume = entry[2], .reverb = entry[4], .chorus = entry[5]});
    }

    return effects;
}

std::span<const uint8_t> SongIn(const ntdlib::Dol& dol, uint32_t sizes, uint32_t count, std::span<const uint8_t> songs,
                                uint16_t number)
{
    constexpr char kSizes[] = "the table of song sizes";
    if (number >= count)
    {
        throw FormatError("the table of song sizes has no song " + std::to_string(number));
    }

    // A song starts where the songs before it end.
    uint64_t offset = 0;
    for (uint32_t i = 0; i < number; i++)
    {
        offset += dol.Word(sizes + i * 4, kSizes);
    }
    if (offset >= songs.size())
    {
        throw FormatError("song " + std::to_string(number) + " starts past the end of the file of songs");
    }

    const uint64_t size = std::min<uint64_t>(dol.Word(sizes + number * 4u, kSizes), songs.size() - offset);
    return songs.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}

const ntdlib::musyx::Effect* FindEffect(const ntdlib::musyx::Group& group, uint16_t id)
{
    const auto is_id = [id](const ntdlib::musyx::Effect& e)
    {
        return e.id == id;
    };
    const auto row = std::find_if(group.effects.begin(), group.effects.end(), is_id);
    return row != group.effects.end() && row->max_voices != 0 ? &*row : nullptr;
}

const ntdlib::musyx::Group* FindGroup(const std::vector<ntdlib::musyx::Group>& groups, ntdlib::musyx::GroupType type,
                                      std::optional<uint16_t> id)
{
    const auto matches = [type, id](const ntdlib::musyx::Group& g)
    {
        return g.type == type && (!id || g.id == *id);
    };
    const auto found = std::find_if(groups.begin(), groups.end(), matches);
    return found == groups.end() ? nullptr : &*found;
}

void AddMissingSamples(SongGroup& group, const std::vector<ntdlib::musyx::SampleInfo>& samples,
                       std::span<const uint8_t> data)
{
    const auto offset = static_cast<uint32_t>(group.sample_data.size());
    bool added = false;
    for (const ntdlib::musyx::SampleInfo& sample : samples)
    {
        const auto held = [&sample](const ntdlib::musyx::SampleInfo& s)
        {
            return s.id == sample.id;
        };
        if (std::none_of(group.samples.begin(), group.samples.end(), held))
        {
            ntdlib::musyx::SampleInfo moved = sample;
            moved.offset += offset;
            group.samples.push_back(moved);
            added = true;
        }
    }
    if (added)
    {
        group.sample_data.insert(group.sample_data.end(), data.begin(), data.end());
    }
}

void MergePool(ntdlib::musyx::Pool& pool, const ntdlib::musyx::Pool& other)
{
    pool.macros.insert(other.macros.begin(), other.macros.end());
    pool.tables.insert(other.tables.begin(), other.tables.end());
    pool.keymaps.insert(other.keymaps.begin(), other.keymaps.end());
    pool.layers.insert(other.layers.begin(), other.layers.end());
}

void AddPushedGroups(SongGroup& group, const std::vector<std::shared_ptr<const SongGroup>>& pushed)
{
    for (const std::shared_ptr<const SongGroup>& other : pushed)
    {
        MergePool(group.pool, other->pool);
    }

    group.pushed.insert(group.pushed.end(), pushed.begin(), pushed.end());
}

std::optional<Game> Game::Open(std::shared_ptr<ntdlib::disc::GameFiles> disc)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(disc->dol));
    Game game;
    game.disc_ = std::move(disc);

    // A game of one kind of rules: the volume table, then the music.
    const auto read = [&game]<typename GameRules>(const GameRules* rules, void (Game::*read_music)(const GameRules&))
    {
        if (rules == nullptr)
        {
            return false;
        }

        game.ReadVolumes(rules->volume_table);
        (game.*read_music)(*rules);

        return true;
    };
    if (read(RulesOf<SoundManagerRules>(kSoundManagerGames, sha1), &Game::ReadSoundManager) ||
        read(RulesOf<SoundRoomRules>(kSoundRoomGames, sha1), &Game::ReadSoundRoomGame) ||
        read(RulesOf<CatalogueRules>(CatalogueGames(), sha1), &Game::ReadCatalogueGame))
    {
        return game;
    }

    const Rules* rules = RulesOf<Rules>(kGames, sha1);
    if (rules == nullptr)
    {
        return std::nullopt;
    }

    game.ReadVolumes(rules->volume_table);
    game.groups_.push_back(game.ReadGroup(rules->group_file, ntdlib::musyx::GroupType::kSongs, std::nullopt));

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

    game.ReadCamelotEffects(rules->effects);

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
    for (const ntdlib::musyx::SoundTestEntry& listed :
         ntdlib::musyx::ReadSoundTest(module, rules.module, rules.sound_test, rules.sound_test_entries, messages,
                                      rules.messages, rules.japanese_messages))
    {
        if (listed.kind == ntdlib::musyx::kSoundTestStream)
        {
            continue;
        }

        const MsmSong song = sound_file.Song(listed.number);
        const auto [found, added] = indexes.try_emplace(song.group_id, groups_.size());
        if (added)
        {
            groups_.push_back(
                ReadSoundManagerGroup(sound_file, song.group_id, ntdlib::musyx::GroupType::kSongs, rules.sound_file));
        }

        songs_.push_back(
            {listed.name, rules.sound_file, song.song_id, song.volume, rules.master_volume, found->second});
        song_data_.emplace_back(song.data.begin(), song.data.end());
    }

    // The effects in the order of their table, each named after its number and its name in the list of voices. Each
    // effects group is read once. MusyX starts no effect that its group lacks.
    const std::map<uint32_t, std::string> names = ntdlib::musyx::ReadSoundTestEffects(
        module, rules.module, rules.voices, rules.voice_entries, messages, rules.messages, rules.japanese_messages);
    std::map<uint16_t, std::size_t> effect_groups;
    for (const MsmEffect& listed : sound_file.Effects())
    {
        const auto [found, added] = effect_groups.try_emplace(listed.group_id, groups_.size());
        if (added)
        {
            groups_.push_back(ReadSoundManagerGroup(sound_file, listed.group_id, ntdlib::musyx::GroupType::kEffects,
                                                    rules.sound_file));
        }

        const ntdlib::musyx::Effect* row = FindEffect(groups_[found->second]->group, listed.id);
        if (row == nullptr)
        {
            continue;
        }

        std::string name = EffectName("se", static_cast<uint16_t>(listed.number));
        if (const auto named = names.find(listed.number); named != names.end() && !named->second.empty())
        {
            name += " " + named->second;
        }

        effects_.push_back(
            {.name = std::move(name),
             .group = found->second,
             .row = *row,
             .volume_controller = listed.volume,
             .pan_controller = listed.pan,
             .reverb = listed.reverb,
             .chorus = listed.chorus,
             .pitch_wheel = static_cast<uint16_t>(std::clamp(kWheelCentre + listed.pitch, 0, kWheelHighest))});
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
    std::set<uint16_t> numbers; // the numbers that the songs and the effects so far take
    const auto add = [&](SoundRoomSong& listed)
    {
        const auto [found, added] = indexes.try_emplace(listed.group_id, groups_.size());
        if (added)
        {
            const ntdlib::musyx::Group* songs = FindGroup(groups, ntdlib::musyx::GroupType::kSongs, listed.group_id);
            if (songs == nullptr)
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
        numbers.insert(listed.number);
    };
    for (SoundRoomSong& listed : ReadSoundRoom(dol, rules.sound_room, rules.sound_room_entries))
    {
        add(listed);
    }

    // The songs that the play function plays as effects, unless the sound room lists them. The function starts an
    // effect for a number from the count of songs on.
    for (SoundRoomSong& listed : ReadEffectSongs(dol, rules.effect_songs, rules.effect_song_entries))
    {
        if (listed.number < rules.song_count && !numbers.contains(listed.number))
        {
            add(listed);
        }
    }

    // The play function's effects: the effect whose ID is the number less the count of songs. Every group of the
    // project holds the pool and the samples. The effects play from the first.
    for (const PlayedEffect& listed : ReadPlayedEffects(dol, rules.effects, rules.effect_entries))
    {
        if (listed.number < rules.song_count || !numbers.insert(listed.number).second)
        {
            continue;
        }

        const auto id = static_cast<uint16_t>(listed.number - rules.song_count);
        const ntdlib::musyx::Effect* row = nullptr;
        for (const ntdlib::musyx::Group& g : groups)
        {
            if (row == nullptr && g.type == ntdlib::musyx::GroupType::kEffects)
            {
                row = FindEffect(g, id);
            }
        }
        if (row != nullptr && !groups_.empty())
        {
            effects_.push_back({.name = EffectName("se", listed.number),
                                .group = 0,
                                .row = *row,
                                .volume = listed.volume,
                                .master_volume = rules.master_volume,
                                .reverb = listed.reverb,
                                .chorus = listed.chorus});
        }
    }
}

void Game::ReadCatalogueGame(const CatalogueRules& rules)
{
    CatalogueMusic music = BuildMusic(ntdlib::musyx::ReadCatalogue(rules.layout, *disc_, rules.tables), rules);
    groups_ = std::move(music.groups);
    songs_ = std::move(music.songs);
    song_data_ = std::move(music.song_data);
    effects_ = std::move(music.effects);
}

std::shared_ptr<SongGroup> Game::ReadGroup(uint32_t first_file, ntdlib::musyx::GroupType type,
                                           std::optional<uint16_t> id) const
{
    // The group's four files. A file's number goes up by 2 from one file to the next (PathOf).
    const std::vector<uint8_t> pool = ReadFile(PathOf(first_file));
    const std::vector<uint8_t> project = ReadFile(PathOf(first_file + 2));
    const std::vector<uint8_t> directory = ReadFile(PathOf(first_file + 4));
    std::vector<uint8_t> samples = ReadFile(PathOf(first_file + 6));

    // The project's group, with the pool and the samples.
    const auto no_group = [first_file, id](const char* kind)
    {
        return PathOf(first_file + 2) + " has no " + kind + " group" + (id ? " " + std::to_string(*id) : std::string());
    };
    return MakeSongGroup(project, pool, directory, std::move(samples), type, id, no_group);
}

void Game::ReadCamelotEffects(const CamelotEffectRules& rules)
{
    constexpr uint32_t kEndOfRanges = 0xffffffff, kNoFiles = 0xffffffff, kEntry = 8, kGroupStep = 8;
    constexpr uint32_t kMostRanges = 256;
    constexpr char kRanges[] = "the table of effects' ranges", kBanks[] = "the table of voice banks",
                   kNarrations[] = "the table of narrations", kCourts[] = "the table of courts";
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc_->dol);
    const auto table = [&dol](uint32_t address, uint32_t count, uint32_t size, const char* what)
    {
        if (address != 0)
        {
            dol.Bytes(address, std::size_t{count} * size, what);
        }
    };
    table(rules.voice_banks, rules.voice_bank_count, rules.voice_bank_size, kBanks);
    table(rules.narrations, rules.narration_count, kEntry, kNarrations);
    table(rules.courts, rules.court_count, kEntry, kCourts);

    // The effects of the tables below, in their order.
    const auto read_group = [this](uint32_t first_file, std::optional<uint16_t> id)
    {
        return ReadGroup(first_file, ntdlib::musyx::GroupType::kEffects, id);
    };
    CamelotEffects effects(groups_, read_group, rules);

    // The group that the game pushes at start-up, and the ranges of its effects.
    const std::size_t startup = effects.StartUp(rules.startup_files);
    for (uint32_t r = 0; r < kMostRanges; r++)
    {
        const uint32_t first = dol.Word(rules.ranges + r * kEntry, kRanges);
        if (first == kEndOfRanges)
        {
            break;
        }

        const uint32_t last = dol.Word(rules.ranges + r * kEntry + 4, kRanges);
        for (uint32_t id = first; id <= last && id <= 0xffff; id++)
        {
            effects.Add(startup, id, "se");
        }
    }

    // The voice banks, named by the table.
    const uint32_t name_size = rules.voice_bank_name_size != 0 ? rules.voice_bank_name_size : 4;
    for (uint32_t b = 0; b < rules.voice_bank_count; b++)
    {
        const uint32_t entry = rules.voice_banks + b * rules.voice_bank_size;
        const std::string name = rules.voice_bank_name_size != 0
                                     ? dol.Text(entry, rules.voice_bank_name_size, kBanks)
                                     : dol.Text(dol.Word(entry, kBanks), kLongestName, "a voice bank's name");
        const uint32_t count = dol.Word(entry + name_size + 4, kBanks);
        const uint32_t first = dol.Word(entry + name_size + 8, kBanks);
        const auto id = static_cast<uint16_t>(dol.Word(entry + name_size + 12, kBanks));
        const std::size_t group = effects.GroupAt(dol.Word(entry + name_size, kBanks), id);
        for (uint32_t e = first; e < first + count && e <= 0xffff; e++)
        {
            effects.Add(group, e, name);
        }
    }

    // The narrations, the courts' groups and the groups that no table lists.
    for (uint32_t n = 0; n < rules.narration_count; n++)
    {
        const uint32_t entry = rules.narrations + n * kEntry;
        const uint32_t file = dol.Word(entry, kNarrations);
        const uint32_t ids = dol.Word(entry + 4, kNarrations);
        const std::size_t group = file != kNoFiles ? effects.GroupAt(file, static_cast<uint16_t>(ids >> 16))
                                                   : effects.GroupAt(rules.narration_files, std::nullopt);
        effects.Add(group, ids & 0xffff, "narration");
    }

    for (uint32_t c = 0; c < rules.court_count; c++)
    {
        const uint32_t entry = rules.courts + c * kEntry;
        const uint32_t file = dol.Word(entry, kCourts);
        if (file != kNoFiles)
        {
            effects.AddAll(effects.GroupAt(file, static_cast<uint16_t>(dol.Word(entry + 4, kCourts))), "court");
        }
    }

    for (uint32_t g = 0; g < rules.other_group_count; g++)
    {
        effects.AddAll(effects.GroupAt(rules.other_groups + g * kGroupStep, std::nullopt), "se");
    }

    effects_ = effects.Take();
}

std::vector<uint8_t> Game::ReadFile(const std::string& path) const
{
    return ntdlib::disc::ReadGameFile(*disc_, path);
}

} // namespace dolphinsf2::musyx
