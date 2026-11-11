// SPDX-License-Identifier: MIT

// The games with a catalogue of their sounds, and the music of a catalogue (see musyx_catalogue.h).

#include "musyx_catalogue.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/musyx/catalogue.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "musyx_game.h"
#include "musyx_voices.h"

namespace dolphinsf2::musyx
{
namespace
{

using ntdlib::musyx::CatalogueLayout;
using ntdlib::musyx::GroupType;
using ntdlib::musyx::Sound;
using ntdlib::musyx::SoundKind;
using xsflib::FormatError;

// Rogue Leader's songs that play a stream, each as its group << 16 | its song ID. Eight songs play a stream along with
// their notes: The Battle Of Yavin I and III, Hyperspace, Imperial Attack, The Asteroid Field, Celebration & End
// Titles, and two that the hall doesn't list, group 0x4D's song 0x1A and group 0x61's song 0x3D. A MIDI file can't
// hold the stream. Those songs are left out. Three more play only a stream, through groups whose other songs play
// notes: Tatooine Training Cutscene 3 and 4 and Prisons Of The Maw Cutscene 1. Each starts one note, whose voice plays
// a stream buffer. They're left out too. BuildMusic leaves out the songs whose groups hold only stream buffers.
constexpr std::array<uint32_t, 11> kRogueLeaderStreams = {0x0046000b, 0x0072006d, 0x005a0030, 0x005d0039,
                                                          0x0047000c, 0x004d001b, 0x004d001a, 0x0061003d,
                                                          0x00620040, 0x00620041, 0x0065004b};

// A song that Rogue Leader's music hall doesn't list. The menus play one of them, group 0x6E's song 0x65, at 0x67 in
// two of their three calls (0x7FCD9A28 and 0x7FD719A8 in the game). It takes that volume.
constexpr std::array<std::pair<uint32_t, uint8_t>, 1> kRogueLeaderVolumes = {{{0x006e0065, 0x67}}};

// The games with a catalogue that dolphinsf2 converts. ntdlib's reader of each layout says what the game's code does.
//
// Resident Evil (USA). The game's sound driver gives the music's master volume 110 at the options' start (0x800620B4,
// 0x80066064). Its set-up ends with sndVolume(100, 0, 0xFF) (0x800662C4). The time from a song's start to its mix is
// 250 ms. It comes after the controllers of each song's first pattern and before its first note. The controllers end
// at tick 64 (83 ms at sg7a's 120 beats a minute and 104 ms at sg74's 96). The first note is at tick 496 (646 ms and
// 807 ms).
//
// Star Fox Adventures (USA, disc version 1). The game's code lists 99 songs at 0x802c5e80. The sound set-up
// (0x80009BF8) calls sndVolume(127, 0, 0xFF) at 0x80009D84 and leaves MusyX's master volumes at 127. An effect whose
// rows all give a volume of 0 makes no sound. It's left out.
//
// Paper Mario: The Thousand-Year Door (USA). The sound set-up (0x800E239C) pushes groups 0 to 11 and keeps them all.
// It calls sndVolume(127, 0, 0xFF) and leaves the master volumes at 127. The game's code lists its sounds at
// 0x80316B28, in 4,242 rows of 20 bytes. dolphinsf2 plays an effect as its row gives it. An effect whose rows all give
// a volume of 0 makes no sound. It's left out.
//
// Pokémon Channel (USA). The game's code lists its music at 0x8015EB50, in 38 rows of 8 bytes. The script of the sound
// viewer holds the names of the sounds from 0x197C3 on. It holds their effects from 0x4552 on. dolphinsf2 plays every
// sound at the music's volume and pan. The sound set-up (0x8001498C) calls sndMasterVolume(100, 100, 1, 1) and
// sndVolume(127, 100, 0xFF).
//
// Star Wars Rogue Squadron II: Rogue Leader (USA). main.dol is a loader. It holds the OS, MusyX and the game, a second
// DOL at 0x80100000. The game's sound set-up (0x7FD49254 in the game) calls sndVolume(127, 0, 0xFF) and leaves the
// master volumes at 127. The game's music hall lists its songs at 0x80060C38, in 114 entries of 16 bytes. The game's
// table of sounds (0x7FDDBF30 in the game) holds 522 rows of 12 bytes.
//
// Star Wars Rogue Squadron III: Rebel Strike (USA, disc version 1). The game's set-up calls sndVolume(127, 0, 0xFF)
// and leaves the master volumes at 127.
//
// Resident Evil Zero (USA). Both discs hold the same main.dol. The game's code lists its songs at 0x80243BCC. The
// set-up calls sndMasterVolume(127, 0, 1, 1) and sndVolume(127, 0, 0xFF) (0x80148F70 and 0x80148F88).
//
// Tales of Symphonia (USA). Both discs hold the same main.dol. The game's code lists its songs at 0x801F9830. The
// set-up calls sndVolume(127, 0, 0xFF) at 0x800A2374 and leaves the master volumes at 127.
//
// Metroid Prime (USA, disc version 2) and Metroid Prime 2: Echoes (USA). The game's start-up calls
// sndVolume(127, 0, 0xFF) and leaves the master volumes at 127.
//
// Pokémon Colosseum (USA). The module common_rel in common.fsys holds the game's table of 1,236 sounds at 0x141654 and
// the table of its songs' 79 files at 0x1133d8. The table gives no song a volume. Every song plays at full volume. The
// game calls sndMasterVolume (0x8014d928) through 0x80166d18 with 127, or with 0 to silence the music (at 0x8000c648,
// 0x8000c660, 0x8000c6ac and 0x8000c6c4). The set-up calls sndVolume(100, 0, SND_FX_VOLGROUPS) (0x80167008). The
// effects' volume groups play at 100/127.
//
// The NDDEMO disc. The disc never went on sale. Its code lists the paths of its nine songs at 0x80065394. The disc's
// code keeps the master volume for music at 127 (0x80026a90). Its menu of scenes (0x80007b20) fades the music to 64
// through sndMasterVolume (0x80047ef8). Each scene starts its song and fades the master volume back to 127 over a
// second. The disc links an early build of MusyX (TickStep::kInteger). That build's salCalcVolume (0x800538e8) takes
// the volume table at 0x8006a0f0, or the DLS table at 0x80069eb4. Its ScaleVolume (0x8004c8bc) shifts the product of
// the volume and the scale right by 7 bits. In Pokémon Colosseum, ScaleVolume divides it by 127. The set-up calls
// sndVolume (0x80047e98) with 127, 0 and 0xff. Every volume group plays at full volume.
//
// Nintendo Puzzle Collection (Japan). main.dol holds the OS, the SDK's libraries and MusyX 2.0. Its module loader
// (0x80006a10) loads the menu and the three games as modules of code. The sound set-up (0x80006d5c) calls sndInit for
// 64 voices. dolphinsf2 plays the music at MusyX's default master volume of 127. MusyX's salCalcVolume (0x8002fb78)
// takes the volume table at 0x800bf7e8, or the DLS table at 0x800bf5cc.
constexpr std::array<CatalogueRules, 13> kCatalogueGames{{
    {.dol_sha1 = "7e514a94826c672605a4f484367ec511fd27933f",
     .layout = CatalogueLayout::kResidentEvil,
     .volume_table = 0x801d6738,
     .music_master_volume = 110,
     .group_volume = 100,
     .controls_ms = 250.0},
    {.dol_sha1 = "8bde4669c75260cc058b8b44ef36bacff3a67d20",
     .layout = CatalogueLayout::kStarFox,
     .tables = {.songs = 0x802c5e80, .song_count = 99},
     .volume_table = 0x80330a18},
    {.dol_sha1 = "bc62d5e4674d139fd50d6c05694b34f955e37039",
     .layout = CatalogueLayout::kPaperMario,
     .tables = {.effects = 0x80316b28, .effect_count = 4242},
     .volume_table = 0x803be458},
    {.dol_sha1 = "3523501ee45ea3d84e12096616cb05c1b9733c21",
     .layout = CatalogueLayout::kPokemonChannel,
     .tables = {.songs = 0x8015eb50, .song_count = 38, .viewer_names = 0x197c3, .viewer_effects = 0x4552},
     .volume_table = 0x802abb68,
     .music_master_volume = 100,
     .effect_master_volume = 100},
    {.dol_sha1 = "bc595e8b8c89ec8307ef61658b6cd5c854670d60",
     .layout = CatalogueLayout::kRogueLeader,
     .tables = {.songs = 0x80060c38,
                .song_count = 114,
                .effects = 0x7fddbf30,
                .effect_count = 522,
                .embedded_dol = 0x80100000,
                .song_volumes = kRogueLeaderVolumes},
     .volume_table = 0x80063320,
     .streamed_songs = kRogueLeaderStreams},
    {.dol_sha1 = "dd1797f6dcdc88abeaecc153b1e76c21e7733cae",
     .layout = CatalogueLayout::kRebelStrike,
     .volume_table = 0x80050938},
    {.dol_sha1 = "5c10973f82a1946ebad3dcfe1ec56e8623d50404",
     .layout = CatalogueLayout::kResidentEvilZero,
     .tables = {.songs = 0x80243bcc, .effects = 0x80258be4, .effect_count = 135},
     .volume_table = 0x80284c78},
    {.dol_sha1 = "01a4ad21219e5635cbca144ea5de0c3dd284dc39",
     .layout = CatalogueLayout::kTales,
     .tables = {.songs = 0x801f9830},
     .volume_table = 0x802aaa98},
    {.dol_sha1 = "39a2f928159ad46491e9b1ef0a72613d91e0cc40",
     .layout = CatalogueLayout::kRetro,
     .volume_table = 0x803f4fe8},
    {.dol_sha1 = "6ef9b491d0cc08bc81a124fdedb8bfaec34d0010",
     .layout = CatalogueLayout::kRetro,
     .volume_table = 0x803c2858},
    {.dol_sha1 = "870e8b9693ca780782d80f22a6a4572d8ba9458f",
     .layout = CatalogueLayout::kColosseum,
     .tables =
         {.songs = 0x141654, .song_count = 1236, .file_table = 0x1133d8, .file_count = 79, .reverb_presets = 0x145044},
     .volume_table = 0x80369a68,
     .group_volume = 100,
     .pushed_sets = PushedSets::kLoaded},
    {.dol_sha1 = "d46883ce81a884b65c6e40d483b196add1dd1bf4",
     .layout = CatalogueLayout::kNdDemo,
     .tables = {.songs = 0x80065394, .song_count = 9},
     .volume_table = 0x8006a0f0,
     .pushed_sets = PushedSets::kLoaded,
     .tick_step = TickStep::kInteger,
     .volume_scale_divisor = 128},
    {.dol_sha1 = "00ab0f4fd4cff96b9087ed1176f9a6e839b57dde",
     .layout = CatalogueLayout::kPuzzleCollection,
     .volume_table = 0x800bf7e8,
     .pushed_sets = PushedSets::kLoaded},
}};

// The song that `sound` plays in the music's group `group` under `rules`. MusyX multiplies a voice's volume by the
// song's volume and by the master volume for music. A sound that sets the master volume plays its song at that volume,
// under the game's master volume.
GameSong SongOf(const Sound& sound, std::size_t group, const CatalogueRules& rules)
{
    GameSong song;
    song.name = sound.name;
    song.id = sound.id;
    song.volume = sound.master_volume ? *sound.master_volume : sound.volume.value_or(127);
    song.master_volume = rules.music_master_volume;
    song.group = group;
    song.tick_step = rules.tick_step;
    song.fade_in_ms = sound.fade_in_ms;
    for (const ntdlib::musyx::Control& control : sound.controls)
    {
        song.controls.push_back({.channel = control.channel,
                                 .controller = control.controller,
                                 .value = static_cast<uint8_t>(control.value)});
    }

    song.controls_ms = sound.controls.empty() ? 0.0 : rules.controls_ms;

    return song;
}

// The effect that `sound` starts with the row `row` in the music's group `group` under `rules`. The effect takes the
// controllers that the game sets. The pitch wheel's value stays within its 14 bits.
GameEffect EffectOf(const Sound& sound, const ntdlib::musyx::Effect& row, std::size_t group,
                    const CatalogueRules& rules)
{
    GameEffect effect{.name = sound.name,
                      .group = group,
                      .row = row,
                      .volume = sound.volume,
                      .pan = sound.pan,
                      .group_volume = sound.group_volume.value_or(rules.group_volume),
                      .master_volume = sound.master_volume.value_or(rules.effect_master_volume)};
    for (const ntdlib::musyx::Control& control : sound.controls)
    {
        const auto value = static_cast<uint8_t>(control.value);
        switch (control.controller)
        {
        case kVolumeController:
            effect.volume_controller = value;
            break;
        case kPanController:
            effect.pan_controller = value;
            break;
        case kReverbController:
            effect.reverb = value;
            break;
        case kChorusController:
            effect.chorus = value;
            break;
        case kPitchWheelControl:
            effect.pitch_wheel = static_cast<uint16_t>(std::clamp(control.value, 0, kWheelHighest));
            break;
        default:
            break;
        }
    }

    return effect;
}

// The error of a sound that no group of those that the game pushes plays.
FormatError NoPlayingGroup(const Sound& sound)
{
    return FormatError("no group that the game pushes plays " + sound.name);
}

// Whether `rules` leave out `sound`: a song that plays a stream, or an effect that the game starts at a volume of 0,
// through the start's volume or the volume controller. MusyX plays such an effect at no volume.
bool LeftOut(const Sound& sound, const CatalogueRules& rules)
{
    if (sound.kind == SoundKind::kEffect)
    {
        const auto silences = [](const ntdlib::musyx::Control& c)
        {
            return c.controller == kVolumeController && c.value == 0;
        };
        return sound.volume == 0 || std::any_of(sound.controls.begin(), sound.controls.end(), silences);
    }

    const uint32_t key = uint32_t{sound.song_group} << 16 | sound.id;
    return std::find(rules.streamed_songs.begin(), rules.streamed_songs.end(), key) != rules.streamed_songs.end();
}

// The samples of a sample directory without its stream buffers, and the stream buffers' IDs.
struct SetSamples
{
    std::vector<ntdlib::musyx::SampleInfo> samples;
    std::set<uint16_t> streams;
};

// The samples in sample directory `sdir`, without its stream buffers. A sample of format 5 is a stream buffer. It holds
// no sound of its own: the game streams audio into it while a voice plays it. Rogue Leader's banks hold such samples.
// Each later entry moves up into a stream buffer's place, and the end marker follows the last entry. The entries
// give the offsets of their DSP-ADPCM settings from the directory's start. Those settings stay in place.
SetSamples ReadSamplesWithoutStreams(std::vector<uint8_t> sdir)
{
    constexpr std::size_t kEntry = 32;
    constexpr std::size_t kFormat = 16;
    constexpr uint8_t kStreamBuffer = 5;
    constexpr uint16_t kEnd = 0xffff;
    SetSamples read;
    std::size_t kept = 0;
    std::size_t at = 0;
    for (; at + kEntry <= sdir.size() && xsflib::LoadBe16(sdir.data() + at) != kEnd; at += kEntry)
    {
        if (sdir[at + kFormat] == kStreamBuffer)
        {
            read.streams.insert(xsflib::LoadBe16(sdir.data() + at));
            continue;
        }

        std::memmove(sdir.data() + kept, sdir.data() + at, kEntry);
        kept += kEntry;
    }

    if (kept != at)
    {
        sdir[kept] = kEnd >> 8;
        sdir[kept + 1] = kEnd & 0xff;
    }

    read.samples = ntdlib::musyx::ReadSampleDirectory(sdir);

    return read;
}

// The music of a catalogue whose groups find missing samples in the holders of the sets that a sound pushes
// (PushedSets::kHolders).
class HolderMusic
{
public:
    HolderMusic(ntdlib::musyx::Catalogue& catalogue, const CatalogueRules& rules)
        : catalogue_(catalogue), rules_(rules), sets_(catalogue.sets.size())
    {
    }

    // Adds `sound` to the music, unless MusyX wouldn't start it or it plays only a stream. Throws FormatError if a file
    // set is damaged, or if no pushed group plays the sound.
    void Add(const Sound& sound);

    CatalogueMusic Take()
    {
        return std::move(music_);
    }

private:
    // A file set as MusyX reads it: the groups of its project, its pool, a group that holds its samples, and the IDs
    // of its stream buffers (ReadSamplesWithoutStreams).
    struct ReadSet
    {
        std::vector<ntdlib::musyx::Group> groups;
        ntdlib::musyx::Pool pool;
        std::shared_ptr<const SongGroup> holder;
        std::set<uint16_t> streams;
    };

    // File set `number`. Each set is read once, when a sound first pushes one of its groups. Its holder joins the
    // music's groups. The holder holds the set's samples, and SFZ files name the samples by it.
    const ReadSet& SetOf(std::size_t number);

    // Whether `sound`, a song, plays. MusyX plays a song through the first pushed group with the song's group ID
    // (sndSeqPlayEx, 0x8001D040 in Rogue Leader's loader). It plays nothing if that group isn't a song group with a
    // MIDI setup for the song. A song group whose samples are all stream buffers plays only a stream. A MIDI file
    // can't hold the stream. Such songs are left out.
    bool Plays(const Sound& sound);

    // The index of the pushed group that plays `sound`: the first that holds the song's group or the effect. Throws
    // FormatError if none does.
    std::size_t PlayingGroup(const Sound& sound);

    // The music's group that plays `sound` from its pushed group `pushed_index`, by the set and the ID of the group
    // and the groups that the sound pushes. Its pool holds the objects of every pushed set. An object of an earlier set
    // comes first. Its samples are in the holders of the sets, in the same order.
    std::size_t GroupOf(const Sound& sound, std::size_t pushed_index);

    ntdlib::musyx::Catalogue& catalogue_;
    const CatalogueRules& rules_;
    std::vector<std::optional<ReadSet>> sets_;
    std::map<std::pair<std::vector<std::pair<std::size_t, uint16_t>>, std::size_t>, std::size_t> playing_;
    CatalogueMusic music_;
};

const HolderMusic::ReadSet& HolderMusic::SetOf(std::size_t number)
{
    std::optional<ReadSet>& set = sets_.at(number);
    if (set)
    {
        return *set;
    }

    ntdlib::musyx::FileSet& files = catalogue_.sets[number];
    set.emplace();
    set->groups = ntdlib::musyx::ReadProject(files.project);
    set->pool = ntdlib::musyx::ReadPool(files.pool);
    auto holder = std::make_shared<SongGroup>();
    if (!set->groups.empty())
    {
        holder->group = set->groups.front();
    }

    holder->pool = set->pool;
    SetSamples samples = ReadSamplesWithoutStreams(std::move(files.sdir));
    holder->samples = std::move(samples.samples);
    set->streams = std::move(samples.streams);
    holder->sample_data = std::move(files.samples);
    set->holder = holder;
    music_.groups.push_back(std::move(holder));

    return *set;
}

bool HolderMusic::Plays(const Sound& sound)
{
    for (const ntdlib::musyx::PushedGroup& pushed : sound.groups)
    {
        const ReadSet& set = SetOf(pushed.set);
        const auto has_id = [&pushed](const ntdlib::musyx::Group& g)
        {
            return g.id == pushed.id;
        };
        const auto group = std::find_if(set.groups.begin(), set.groups.end(), has_id);
        if (pushed.id != sound.song_group || group == set.groups.end())
        {
            continue;
        }

        const auto plays_song = [&sound](const ntdlib::musyx::MidiSetup& m)
        {
            return m.song == sound.id;
        };
        const auto is_stream = [&set](uint16_t sample)
        {
            return set.streams.contains(sample);
        };
        const bool streams_only =
            !group->samples.empty() && std::all_of(group->samples.begin(), group->samples.end(), is_stream);
        return group->type == GroupType::kSongs &&
               std::any_of(group->midi_setups.begin(), group->midi_setups.end(), plays_song) && !streams_only;
    }

    return false;
}

std::size_t HolderMusic::PlayingGroup(const Sound& sound)
{
    const bool effect = sound.kind == SoundKind::kEffect;
    const GroupType type = effect ? GroupType::kEffects : GroupType::kSongs;
    for (std::size_t g = 0; g < sound.groups.size(); g++)
    {
        const ntdlib::musyx::PushedGroup& pushed = sound.groups[g];
        const uint16_t id = effect ? pushed.id : sound.song_group;
        const ntdlib::musyx::Group* group = FindGroup(SetOf(pushed.set).groups, type, pushed.id);
        if (pushed.id == id && group != nullptr && (!effect || FindEffect(*group, sound.id) != nullptr))
        {
            return g;
        }
    }

    throw NoPlayingGroup(sound);
}

std::size_t HolderMusic::GroupOf(const Sound& sound, std::size_t pushed_index)
{
    std::vector<std::pair<std::size_t, uint16_t>> key;
    for (const ntdlib::musyx::PushedGroup& g : sound.groups)
    {
        key.emplace_back(g.set, g.id);
    }

    const auto found = playing_.find({key, pushed_index});
    if (found != playing_.end())
    {
        return found->second;
    }

    const ntdlib::musyx::PushedGroup& plays = sound.groups[pushed_index];
    const GroupType type = sound.kind == SoundKind::kEffect ? GroupType::kEffects : GroupType::kSongs;
    auto group = std::make_shared<SongGroup>();
    group->group = *FindGroup(SetOf(plays.set).groups, type, plays.id);
    group->volume_scale_divisor = rules_.volume_scale_divisor;
    std::vector<std::size_t> order;
    for (const ntdlib::musyx::PushedGroup& g : sound.groups)
    {
        if (std::find(order.begin(), order.end(), g.set) == order.end())
        {
            order.push_back(g.set);
        }
    }

    for (const std::size_t number : order)
    {
        const ReadSet& set = SetOf(number);
        MergePool(group->pool, set.pool);
        group->pushed.push_back(set.holder);
    }

    music_.groups.push_back(std::move(group));
    playing_.emplace(std::pair{std::move(key), pushed_index}, music_.groups.size() - 1);

    return music_.groups.size() - 1;
}

void HolderMusic::Add(const Sound& sound)
{
    if (sound.kind == SoundKind::kSong && !Plays(sound))
    {
        return;
    }

    const std::size_t group = GroupOf(sound, PlayingGroup(sound));
    if (sound.kind == SoundKind::kSong)
    {
        music_.songs.push_back(SongOf(sound, group, rules_));
        music_.song_data.push_back(catalogue_.songs.at(sound.song));
        return;
    }

    music_.effects.push_back(EffectOf(sound, *FindEffect(music_.groups[group]->group, sound.id), group, rules_));
}

// The music of a catalogue whose groups take the objects and the samples of the other sets that a sound pushes
// (PushedSets::kLoaded).
class LoadedMusic
{
public:
    LoadedMusic(const ntdlib::musyx::Catalogue& catalogue, const CatalogueRules& rules)
        : catalogue_(catalogue), rules_(rules), sets_(catalogue.sets.size())
    {
    }

    // Adds `sounds`: the songs, then the effects. The groups that play the songs come first, then those that play only
    // effects, each kind in the order of the sets that play them. Throws FormatError if a file set is damaged, or if
    // no pushed group plays a sound.
    void Add(std::span<const Sound* const> sounds);

    CatalogueMusic Take()
    {
        return std::move(music_);
    }

private:
    // A file set as MusyX reads it: the groups of its project, its pool and its samples.
    struct ReadSet
    {
        std::vector<ntdlib::musyx::Group> groups;
        ntdlib::musyx::Pool pool;
        std::vector<ntdlib::musyx::SampleInfo> samples;
    };

    // A group of the music by what plays in it: the set and the ID of the group that plays, and the other sets that
    // the sound pushes, in their order.
    using Key = std::tuple<std::size_t, uint16_t, std::vector<std::size_t>>;

    // File set `number`, read once.
    const ReadSet& SetOf(std::size_t number);

    // The key of the group that plays `sound`. A song plays from the first pushed group with the song's group ID, and
    // an effect from the first group that it pushes. Throws FormatError if the sound pushes no such group.
    static Key KeyOf(const Sound& sound);

    // Makes the music's group for `sound`, a sound of `type`'s groups, unless that group is made already. Throws
    // FormatError if the set has no such group.
    void MakeGroup(const Sound& sound, GroupType type);

    // The row of `sound`, an effect, in the table of effects of the first pushed group that holds it. Throws
    // FormatError if no pushed group holds it.
    const ntdlib::musyx::Effect& RowOf(const Sound& sound);

    const ntdlib::musyx::Catalogue& catalogue_;
    const CatalogueRules& rules_;
    std::vector<std::optional<ReadSet>> sets_;
    std::map<Key, std::size_t> groups_;
    CatalogueMusic music_;
};

const LoadedMusic::ReadSet& LoadedMusic::SetOf(std::size_t number)
{
    std::optional<ReadSet>& set = sets_.at(number);
    if (!set)
    {
        const ntdlib::musyx::FileSet& files = catalogue_.sets[number];
        set = ReadSet{.groups = ntdlib::musyx::ReadProject(files.project),
                      .pool = ntdlib::musyx::ReadPool(files.pool),
                      .samples = ntdlib::musyx::ReadSampleDirectory(files.sdir)};
    }

    return *set;
}

LoadedMusic::Key LoadedMusic::KeyOf(const Sound& sound)
{
    const auto plays = [&sound](const ntdlib::musyx::PushedGroup& g)
    {
        return sound.kind == SoundKind::kEffect || g.id == sound.song_group;
    };
    const auto playing = std::find_if(sound.groups.begin(), sound.groups.end(), plays);
    if (playing == sound.groups.end())
    {
        throw NoPlayingGroup(sound);
    }

    std::vector<std::size_t> others;
    for (const ntdlib::musyx::PushedGroup& g : sound.groups)
    {
        if (g.set != playing->set && std::find(others.begin(), others.end(), g.set) == others.end())
        {
            others.push_back(g.set);
        }
    }

    return {playing->set, playing->id, std::move(others)};
}

void LoadedMusic::MakeGroup(const Sound& sound, GroupType type)
{
    Key key = KeyOf(sound);
    if (groups_.contains(key))
    {
        return;
    }

    const auto& [number, id, others] = key;
    const ReadSet& set = SetOf(number);
    const ntdlib::musyx::Group* plays = FindGroup(set.groups, type, id);
    if (plays == nullptr)
    {
        throw NoPlayingGroup(sound);
    }

    auto group = std::make_shared<SongGroup>();
    group->group = *plays;
    group->pool = set.pool;
    group->samples = set.samples;
    group->sample_data = catalogue_.sets[number].samples;
    group->volume_scale_divisor = rules_.volume_scale_divisor;
    for (const std::size_t other : others)
    {
        const ReadSet& loaded = SetOf(other);
        MergePool(group->pool, loaded.pool);
        AddMissingSamples(*group, loaded.samples, catalogue_.sets[other].samples);
    }

    groups_.emplace(std::move(key), music_.groups.size());
    music_.groups.push_back(std::move(group));
}

const ntdlib::musyx::Effect& LoadedMusic::RowOf(const Sound& sound)
{
    for (const ntdlib::musyx::PushedGroup& pushed : sound.groups)
    {
        const ntdlib::musyx::Group* group = FindGroup(SetOf(pushed.set).groups, GroupType::kEffects, pushed.id);
        if (const ntdlib::musyx::Effect* row = group != nullptr ? FindEffect(*group, sound.id) : nullptr)
        {
            return *row;
        }
    }

    throw NoPlayingGroup(sound);
}

void LoadedMusic::Add(std::span<const Sound* const> sounds)
{
    for (const SoundKind kind : {SoundKind::kSong, SoundKind::kEffect})
    {
        std::vector<const Sound*> played;
        for (const Sound* sound : sounds)
        {
            if (sound->kind == kind && !LeftOut(*sound, rules_))
            {
                played.push_back(sound);
            }
        }

        // The groups, in the order of the sets that play in them, then the sounds.
        std::vector<const Sound*> by_set = played;
        const auto set_of = [](const Sound* a, const Sound* b)
        {
            return std::get<0>(KeyOf(*a)) < std::get<0>(KeyOf(*b));
        };
        std::stable_sort(by_set.begin(), by_set.end(), set_of);
        for (const Sound* sound : by_set)
        {
            MakeGroup(*sound, kind == SoundKind::kSong ? GroupType::kSongs : GroupType::kEffects);
        }

        for (const Sound* sound : played)
        {
            const std::size_t group = groups_.at(KeyOf(*sound));
            if (kind == SoundKind::kSong)
            {
                music_.songs.push_back(SongOf(*sound, group, rules_));
                music_.song_data.push_back(catalogue_.songs.at(sound->song));
            }
            else
            {
                music_.effects.push_back(EffectOf(*sound, RowOf(*sound), group, rules_));
            }
        }
    }
}

// The music of `catalogue`, whose groups find missing samples in the holders of the pushed sets. The songs
// that the game plays as music come first, then the other sounds, each in the catalogue's order.
CatalogueMusic HolderMusicOf(ntdlib::musyx::Catalogue& catalogue, const CatalogueRules& rules)
{
    std::vector<const Sound*> sounds;
    for (const Sound& sound : catalogue.sounds)
    {
        sounds.push_back(&sound);
    }

    const auto music_first = [](const Sound* sound)
    {
        return sound->kind == SoundKind::kSong && !sound->sound_effect;
    };
    std::stable_partition(sounds.begin(), sounds.end(), music_first);

    HolderMusic music(catalogue, rules);
    for (const Sound* sound : sounds)
    {
        if (!LeftOut(*sound, rules))
        {
            music.Add(*sound);
        }
    }

    return music.Take();
}

// The music of `catalogue`, whose groups take the objects and the samples of the other pushed sets. The sounds of each
// module come in turn, in the order of the modules. A catalogue without modules has one.
CatalogueMusic LoadedMusicOf(const ntdlib::musyx::Catalogue& catalogue, const CatalogueRules& rules)
{
    LoadedMusic music(catalogue, rules);
    const std::size_t modules = std::max<std::size_t>(catalogue.modules.size(), 1);
    for (std::size_t m = 0; m < modules; m++)
    {
        std::vector<const Sound*> sounds;
        for (const Sound& sound : catalogue.sounds)
        {
            if (sound.module == m)
            {
                sounds.push_back(&sound);
            }
        }

        music.Add(sounds);
    }

    return music.Take();
}

} // namespace

std::span<const CatalogueRules> CatalogueGames()
{
    return kCatalogueGames;
}

CatalogueMusic BuildMusic(ntdlib::musyx::Catalogue catalogue, const CatalogueRules& rules)
{
    return rules.pushed_sets == PushedSets::kHolders ? HolderMusicOf(catalogue, rules)
                                                     : LoadedMusicOf(catalogue, rules);
}

} // namespace dolphinsf2::musyx
