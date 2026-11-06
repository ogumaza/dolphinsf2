// SPDX-License-Identifier: MIT

// The catalogues of the MusyX games whose code keeps its sounds in layouts of its own (see musyx_catalogue.h).

#include "musyx_catalogue.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

#include "ntdlib/disc/compression.h"
#include "ntdlib/disc/error.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"
#include "xsflib/utf8.h"

namespace dolphinsf2::musyx
{

namespace
{

using ntdlib::musyx::GroupType;
using xsflib::FormatError;

// The disc's file at `path`. Throw FormatError if the disc lacks it or it can't be read.
std::vector<uint8_t> ReadDiscFile(const ntdlib::disc::GameFiles& disc, const std::string& path)
{
    std::vector<uint8_t> data;
    try
    {
        if (!disc.read_file(path, data))
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

// The `size` bytes at `offset` of `data`. Throw FormatError, naming `what`, if they run past its end.
std::span<const uint8_t> Part(std::span<const uint8_t> data, std::size_t offset, std::size_t size, const char* what)
{
    if (offset > data.size() || size > data.size() - offset)
    {
        throw FormatError(std::string(what) + " runs past the end of its file");
    }

    return data.subspan(offset, size);
}

// The `size` bytes at load address `address` of `dol`. Throw FormatError, naming `what`, if no section holds them.
std::span<const uint8_t> DolBytes(const ntdlib::Dol& dol, uint32_t address, std::size_t size, const char* what)
{
    const std::span<const uint8_t> bytes = dol.At(address);
    if (bytes.size() < size)
    {
        throw FormatError(std::string("main.dol has no data at ") + what);
    }

    return bytes.first(size);
}

// The text that starts at `bytes`, up to its zero byte or the end of `bytes`.
std::string TextBeforeZero(std::span<const uint8_t> bytes)
{
    const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{0});
    return std::string(bytes.begin(), end);
}

// The text at load address `address` of `dol`, up to its zero byte, at most 64 bytes. Throw FormatError, naming
// `what`, if no section holds the address.
std::string DolText(const ntdlib::Dol& dol, uint32_t address, const char* what)
{
    constexpr std::size_t kLongest = 64;
    const std::span<const uint8_t> bytes = dol.At(address);
    if (bytes.empty())
    {
        throw FormatError(std::string("main.dol has no data at ") + what);
    }

    return TextBeforeZero(bytes.first(std::min(bytes.size(), kLongest)));
}

// The volume that most of `volumes` give. Of volumes that tie, the one that comes first wins.
uint8_t MostCommon(const std::vector<uint8_t>& volumes)
{
    uint8_t best = volumes.front();
    std::ptrdiff_t best_count = 0;
    for (const uint8_t volume : volumes)
    {
        const std::ptrdiff_t count = std::count(volumes.begin(), volumes.end(), volume);
        if (count > best_count)
        {
            best = volume;
            best_count = count;
        }
    }

    return best;
}

// An effect of an effects group in a file set: the group's ID, the effect's ID, the set and the effect's row.
struct SetEffect
{
    uint16_t group = 0;
    uint16_t id = 0;
    std::size_t set = 0;
    ntdlib::musyx::Effect row;
};

// The effects of the effects groups in `project`, the project of file set `set`, in the file's order. MusyX never
// starts an effect whose row allows no voices. The list leaves such an effect out.
std::vector<SetEffect> ProjectEffectsOf(std::span<const uint8_t> project, std::size_t set)
{
    std::vector<SetEffect> effects;
    for (const ntdlib::musyx::Group& group : ntdlib::musyx::ReadProject(project))
    {
        if (group.type != GroupType::kEffects)
        {
            continue;
        }

        for (const ntdlib::musyx::Effect& row : group.effects)
        {
            if (row.max_voices != 0)
            {
                effects.push_back({.group = group.id, .id = row.id, .set = set, .row = row});
            }
        }
    }

    return effects;
}

// Sort `effects` by ID and keep the first effect of each ID. The sort keeps the order of effects that share an ID.
void KeepFirstOfEachId(std::vector<SetEffect>& effects)
{
    const auto by_id = [](const SetEffect& a, const SetEffect& b)
    {
        return a.id < b.id;
    };
    std::stable_sort(effects.begin(), effects.end(), by_id);

    const auto same_id = [](const SetEffect& a, const SetEffect& b)
    {
        return a.id == b.id;
    };
    effects.erase(std::unique(effects.begin(), effects.end(), same_id), effects.end());
}

// The file set of the four files `stem` with the extensions .pro, .poo, .sdi and .sam: the project, the pool, the
// sample directory and the samples.
FileSet ShortNamedSet(const ntdlib::disc::GameFiles& disc, const std::string& stem)
{
    return {.project = ReadDiscFile(disc, stem + ".pro"),
            .pool = ReadDiscFile(disc, stem + ".poo"),
            .sdir = ReadDiscFile(disc, stem + ".sdi"),
            .samples = ReadDiscFile(disc, stem + ".sam")};
}

// The volumes of Star Fox Adventures' effects, by the effects' IDs. The game starts its effects through the rows of
// audio/data/Sfx.bin, 32 bytes each. A row gives the effect's volume in its byte at 2. Six effect IDs follow from 0xA,
// in big-endian halves. The top four bits of the row's last byte give how many of them the row uses. The game picks
// one of those at random. An ID of 0 picks no effect. An effect plays at the volume that most of its rows with a volume
// above 0 give it. An effect whose rows all give 0 plays at 0.
std::map<uint16_t, uint8_t> StarFoxEffectVolumes(std::span<const uint8_t> rows)
{
    constexpr std::size_t kRow = 32, kVolume = 2, kIds = 0xa, kMostIds = 6, kCount = 0x1f;
    std::map<uint16_t, std::vector<uint8_t>> volumes;
    for (std::size_t at = 0; at + kRow <= rows.size(); at += kRow)
    {
        const uint8_t* row = rows.data() + at;
        const std::size_t count = std::min<std::size_t>(row[kCount] >> 4, kMostIds);
        for (std::size_t i = 0; i < count; i++)
        {
            const uint16_t id = xsflib::LoadBe16(row + kIds + i * 2);
            if (id != 0)
            {
                volumes[id].push_back(row[kVolume]);
            }
        }
    }

    std::map<uint16_t, uint8_t> result;
    for (const auto& [id, all] : volumes)
    {
        std::vector<uint8_t> above_0;
        std::copy_if(all.begin(), all.end(), std::back_inserter(above_0), [](uint8_t volume) { return volume > 0; });
        result[id] = MostCommon(above_0.empty() ? all : above_0);
    }

    return result;
}

// Star Fox Adventures (USA, disc version 1). The game's code lists 99 songs at 0x802c5e80, in entries of 16 bytes: the
// song's ID in a big-endian half, its group in a byte, and the address of its name at +4. The game fills in the
// entry's last two words. audio/midi.wad holds the songs by ID: their sizes in 100 big-endian words at its start, then
// the songs from 0x1A0, each padded to 32 bytes. The game plays a song through sndSeqPlayEx (0x8027c140), with the
// song's group and ID. At start-up it pushes group 0 of the starfoxm files, then groups 1 to 55 of the starfoxs files,
// and keeps them all. The songs come in the order of their IDs.
//
// The game starts its music through triggers: the entries of audio/data/Music.bin, 16 bytes each. An entry gives its
// trigger's number and the song's ID in big-endian halves, and the song's volume in its byte at 0xC. A song plays at
// the volume that most of its triggers give it. Trigger 0xEC plays a song that its caller picks. A song that no other
// trigger names takes that trigger's volume. The trigger's code (0x8000B440) starts the song at a volume of 0 (flag 4
// of sndSeqPlayEx's parameters, with a time and a volume of 0). It then raises the volume to the trigger's over 500 ms
// (sndSeqVolume at 0x8000B4B4).
//
// The effects follow the songs, in the order of their IDs: every effect of the effects groups that the game pushes.
// The game starts an effect through 0x8000C4D8, which calls sndFXStartEx (0x8027300c) with the volume of a row of
// audio/data/Sfx.bin (StarFoxEffectVolumes), a pan of 0x40 and studio 0. An effect that no row plays takes the volume
// and the pan of its row in its group's table of effects. Nothing names the effects. An effect is named after its set
// and its ID, such as "starfoxs-0420". An effect whose rows all give a volume of 0 makes no sound. It's left out. The
// sound set-up (0x80009BF8) calls sndVolume(127, 0, 0xFF) at 0x80009D84 and leaves MusyX's master volumes at 127.
Catalogue GatherStarFox(const ntdlib::disc::GameFiles& disc)
{
    constexpr uint32_t kSongTable = 0x802c5e80, kSongCount = 99;
    constexpr std::size_t kSongEntry = 16, kWadSongs = 100, kWadHeader = 0x1a0, kTrigger = 16;
    constexpr uint16_t kPickedSong = 0xec, kLastGroup = 55;
    constexpr uint8_t kCentre = 0x40;
    constexpr double kFadeInMs = 500.0;
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc.dol);

    // The volumes that the triggers give each song, in the file's order.
    const std::vector<uint8_t> triggers = ReadDiscFile(disc, "audio/data/Music.bin");
    std::map<uint16_t, std::vector<uint8_t>> song_volumes;
    uint8_t picked_volume = 127;
    for (std::size_t at = 0; at + kTrigger <= triggers.size(); at += kTrigger)
    {
        const uint8_t* trigger = triggers.data() + at;
        if (xsflib::LoadBe16(trigger) == kPickedSong)
        {
            picked_volume = trigger[0xc];
        }
        else
        {
            song_volumes[xsflib::LoadBe16(trigger + 2)].push_back(trigger[0xc]);
        }
    }

    // The songs' files, by ID.
    Catalogue catalogue;
    const std::vector<uint8_t> wad = ReadDiscFile(disc, "audio/midi.wad");
    const std::span<const uint8_t> sizes = Part(wad, 0, kWadHeader, "audio/midi.wad's table");
    std::size_t at = kWadHeader;
    for (std::size_t id = 0; id < kWadSongs; id++)
    {
        const uint32_t size = xsflib::LoadBe32(sizes.data() + id * 4);
        const std::span<const uint8_t> song = Part(wad, at, size, "a song of audio/midi.wad");
        catalogue.songs.emplace_back(song.begin(), song.end());
        at += (std::size_t{size} + 31) & ~std::size_t{31};
    }

    catalogue.sets.push_back(ShortNamedSet(disc, "audio/starfoxm"));
    catalogue.sets.push_back(ShortNamedSet(disc, "audio/starfoxs"));
    std::vector<PushedGroup> groups = {{.id = 0, .set = 0}};
    for (uint16_t group = 1; group <= kLastGroup; group++)
    {
        groups.push_back({.id = group, .set = 1});
    }

    // The code's table of songs, in the order of their IDs.
    std::vector<CatalogueSound> songs;
    for (uint32_t i = 0; i < kSongCount; i++)
    {
        const std::span<const uint8_t> entry =
            DolBytes(dol, kSongTable + i * static_cast<uint32_t>(kSongEntry), kSongEntry, "the table of songs");
        CatalogueSound song;
        song.id = xsflib::LoadBe16(entry.data());
        song.song_group = entry[2];
        song.name = DolText(dol, xsflib::LoadBe32(entry.data() + 4), "a song's name");
        if (song.id >= kWadSongs)
        {
            throw FormatError("audio/midi.wad has no song " + std::to_string(song.id));
        }

        const auto found = song_volumes.find(song.id);
        song.volume = found == song_volumes.end() ? picked_volume : MostCommon(found->second);
        song.fade_in_ms = kFadeInMs;
        song.song = song.id;
        song.groups = groups;
        songs.push_back(std::move(song));
    }

    const auto by_id = [](const CatalogueSound& a, const CatalogueSound& b)
    {
        return a.id < b.id;
    };
    std::stable_sort(songs.begin(), songs.end(), by_id);
    catalogue.sounds = std::move(songs);

    // The effects of the pushed groups, in the order of their IDs.
    const std::map<uint16_t, uint8_t> effect_volumes = StarFoxEffectVolumes(ReadDiscFile(disc, "audio/data/Sfx.bin"));
    const std::array<const char*, 2> kSetNames = {"starfoxm", "starfoxs"};
    std::vector<SetEffect> effects;
    for (std::size_t set = 0; set < catalogue.sets.size(); set++)
    {
        for (const SetEffect& effect : ProjectEffectsOf(catalogue.sets[set].project, set))
        {
            const auto pushed = [&effect](const PushedGroup& g)
            {
                return g.id == effect.group && g.set == effect.set;
            };
            if (std::any_of(groups.begin(), groups.end(), pushed))
            {
                effects.push_back(effect);
            }
        }
    }

    KeepFirstOfEachId(effects);
    for (const SetEffect& effect : effects)
    {
        CatalogueSound sound;
        sound.name = EffectName(kSetNames[effect.set], effect.id);
        sound.effect = true;
        sound.id = effect.id;
        sound.groups = groups;
        if (const auto found = effect_volumes.find(effect.id); found != effect_volumes.end())
        {
            if (found->second == 0)
            {
                continue;
            }

            sound.volume = found->second;
            sound.pan = kCentre;
        }

        catalogue.sounds.push_back(std::move(sound));
    }

    return catalogue;
}

// A row of Paper Mario: The Thousand-Year Door's table of sounds (see GatherPaperMario): the sound's name, its kind and
// ID, its volume, its pan, its send to the reverb and the bend of its pitch.
struct PaperMarioRow
{
    std::string name;
    uint32_t sound = 0;
    uint8_t volume = 0, pan = 0, send = 0;
    int16_t bend = 0;
};

// The bits of a row's second word that give the sound's kind, and the kinds of Paper Mario's effects and songs.
constexpr uint32_t kPaperMarioKind = 0xf0000000, kPaperMarioEffect = 0x20000000, kPaperMarioSong = 0x40000000;

// The rows of Paper Mario's table of sounds that play sounds of `kind`: an effect's row by the effect's ID, a song's
// row by the song's record (see GatherPaperMario). Several rows can play one sound. The sound takes the first row with
// a volume above 0, or the first row if none has one.
std::map<uint32_t, PaperMarioRow> PaperMarioRows(const ntdlib::Dol& dol, uint32_t kind)
{
    constexpr uint32_t kTable = 0x80316b28, kRows = 4242, kRow = 20, kId = 0xffff, kRecord = 0xff;
    std::map<uint32_t, PaperMarioRow> rows;
    for (uint32_t i = 0; i < kRows; i++)
    {
        const std::span<const uint8_t> row = DolBytes(dol, kTable + i * kRow, kRow, "the table of sounds");
        const uint32_t sound = xsflib::LoadBe32(row.data() + 4);
        const uint32_t key = sound & (kind == kPaperMarioSong ? kRecord : kId);
        const auto found = rows.find(key);
        if ((sound & kPaperMarioKind) != kind || (found != rows.end() && (found->second.volume != 0 || row[0xc] == 0)))
        {
            continue;
        }

        std::string name;
        if (const uint32_t at = xsflib::LoadBe32(row.data()); at != 0)
        {
            name = DolText(dol, at, "a sound's name");
        }

        rows[key] = {.name = std::move(name),
                     .sound = sound,
                     .volume = row[0xc],
                     .pan = row[0xd],
                     .send = row[0xf],
                     .bend = static_cast<int16_t>(xsflib::LoadBe16(row.data() + 0x12))};
    }

    return rows;
}

// Paper Mario: The Thousand-Year Door (USA). sound/proj/pmario.proj, .pool, .sdir and .samp hold every MusyX group of
// the game. The sound set-up (0x800E239C) pushes groups 0 to 11 and keeps them all. It calls sndVolume(127, 0, 0xFF)
// and leaves the master volumes at 127. The game streams its music. Its jingles and some of its effects are MusyX
// songs. Each sound plays from its group alone.
//
// The game's code lists its sounds at 0x80316B28, in 4,242 rows of 20 bytes. A row starts with the address of the
// sound's name. The word after it gives the sound's kind in its top bits and its ID in its low 16 bits. 0x20000000
// marks an effect of MusyX, 0x40000000 a song and 0x80000000 a stream. The bytes at 0xC, 0xD and 0xF give an
// effect's volume, its pan and its send to the reverb. The big-endian half at 0x12 bends its pitch. The game starts
// the effect with sndFXStartEx, a volume of 0x7F and the row's pan (0x800E0E4C). It then sets MIDI controller 7 to the
// row's volume, the pitch wheel to 0x2000 plus the bend, and controller 91 to the send. Most of the game's sounds play
// at a place in the world. Their volumes and pans then follow the listener. dolphinsf2 plays an effect as its row gives
// it. An effect takes its row's name. One that no row plays is named after the project and its ID, such as
// "pmario-0051", and plays at MusyX's defaults. An effect whose rows all give a volume of 0 makes no sound. It's left
// out.
//
// The songs come in the order of sound/proj/pmario.slib. The game reads the file at start-up. The file holds the
// songs, one record after another. A record starts with its size in a big-endian word, its 32 bytes of header
// included. The song follows the header. A size of 0xFFFFFFFF ends the file. A song's row gives the song's group in
// bits 16 to 23 of its second word, MusyX's song ID in bits 8 to 15, and the song's record in bits 0 to 7. The game
// plays the song with sndSeqPlayEx (0x80287fe0), the group, the ID and the record's song (0x800E124C). It sets the
// song's volume to the row's volume. pmario.stbl names the records: 32 bytes for each, the song's name, ended by a
// byte 0, then the song's group at 0x1E and its ID at 0x1F. A song that no row plays takes its name, its group and its
// ID from there, and plays at MusyX's default volume.
Catalogue GatherPaperMario(const ntdlib::disc::GameFiles& disc)
{
    constexpr uint8_t kStartVolume = 0x7f;
    constexpr uint32_t kEndOfSongs = 0xffffffff;
    constexpr std::size_t kHeader = 32, kName = 32, kNameGroup = 0x1e, kNameId = 0x1f;
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc.dol);

    Catalogue catalogue;
    catalogue.sets.push_back({.project = ReadDiscFile(disc, "sound/proj/pmario.proj"),
                              .pool = ReadDiscFile(disc, "sound/proj/pmario.pool"),
                              .sdir = ReadDiscFile(disc, "sound/proj/pmario.sdir"),
                              .samples = ReadDiscFile(disc, "sound/proj/pmario.samp")});

    // The songs of the song library, with the names and the volumes of their rows.
    const std::map<uint32_t, PaperMarioRow> song_rows = PaperMarioRows(dol, kPaperMarioSong);
    const std::vector<uint8_t> library = ReadDiscFile(disc, "sound/proj/pmario.slib");
    const std::vector<uint8_t> names = ReadDiscFile(disc, "sound/proj/pmario.stbl");
    std::size_t at = 0;
    for (uint32_t record = 0;; record++)
    {
        const uint32_t size = xsflib::LoadBe32(Part(library, at, 4, "the song library").data());
        if (size == kEndOfSongs)
        {
            break;
        }
        if (size < kHeader)
        {
            throw FormatError("a record of the song library is shorter than its header");
        }

        const std::span<const uint8_t> song = Part(library, at + kHeader, size - kHeader, "the song library");
        const std::span<const uint8_t> name = Part(names, record * kName, kName, "the table of song names");
        at += size;

        CatalogueSound sound;
        if (const auto found = song_rows.find(record); found != song_rows.end())
        {
            sound.name = found->second.name;
            sound.id = static_cast<uint16_t>(found->second.sound >> 8 & 0xff);
            sound.song_group = static_cast<uint16_t>(found->second.sound >> 16 & 0xff);
            sound.volume = found->second.volume;
        }
        else
        {
            sound.id = name[kNameId];
            sound.song_group = name[kNameGroup];
        }
        if (sound.name.empty())
        {
            sound.name = TextBeforeZero(name.first(kNameGroup));
        }

        sound.song = catalogue.songs.size();
        sound.groups = {{.id = sound.song_group, .set = 0}};
        catalogue.songs.emplace_back(song.begin(), song.end());
        catalogue.sounds.push_back(std::move(sound));
    }

    // The effects of the project's effects groups, in the order of their IDs.
    const std::map<uint32_t, PaperMarioRow> effect_rows = PaperMarioRows(dol, kPaperMarioEffect);
    std::vector<SetEffect> effects = ProjectEffectsOf(catalogue.sets[0].project, 0);
    KeepFirstOfEachId(effects);
    for (const SetEffect& effect : effects)
    {
        CatalogueSound sound;
        sound.name = EffectName("pmario", effect.id);
        sound.effect = true;
        sound.id = effect.id;
        sound.groups = {{.id = effect.group, .set = 0}};
        if (const auto found = effect_rows.find(effect.id); found != effect_rows.end())
        {
            const PaperMarioRow& row = found->second;
            if (row.volume == 0)
            {
                continue;
            }
            if (!row.name.empty())
            {
                sound.name = row.name;
            }

            sound.volume = kStartVolume;
            sound.pan = row.pan;
            sound.volume_controller = row.volume;
            sound.pitch_wheel = static_cast<uint16_t>(std::clamp(0x2000 + row.bend, 0, 0x3fff));
            sound.reverb = row.send;
        }

        catalogue.sounds.push_back(std::move(sound));
    }

    return catalogue;
}

// The names that Pokémon Channel's sound viewer, pch/scripts/sndviewer.pkc, gives the effects, by the effects' IDs.
// The viewer's script holds its names from 0x197C3 on. Each name starts and ends with a tab, and a byte 0 follows it.
// The first string that doesn't start with a tab ends the names. From 0x4552 on, the script's code gives the names'
// effects in a run of entries. An entry holds 0x31 0x00 and the name's number in a big-endian word, 0x02 and a word,
// then 0x19 and the value. The value is 0x00 and the effect's ID in a big-endian word, or one byte other than 0, which
// names no effect that dolphinsf2 can tell. The bytes 0x1F 0x35 follow every entry but the last. An effect that
// several names give takes the first name.
std::map<uint16_t, std::string> ViewerNames(std::span<const uint8_t> script)
{
    constexpr std::size_t kNames = 0x197c3, kEffects = 0x4552;
    constexpr uint8_t kTab = '\t';
    std::vector<std::string> names;
    for (std::size_t at = kNames; at < script.size() && script[at] == kTab;)
    {
        const auto end = std::find(script.begin() + static_cast<std::ptrdiff_t>(at), script.end(), uint8_t{0});
        if (end == script.end())
        {
            throw FormatError("a name of the sound viewer doesn't end");
        }

        const std::string text(script.begin() + static_cast<std::ptrdiff_t>(at), end);
        if (text.size() < 2 || text.back() != kTab)
        {
            break;
        }

        names.push_back(text.substr(1, text.size() - 2));
        at += text.size() + 1;
    }

    // An entry's parts: its number at +2, the 0x02 at +6, the 0x19 at +11 and its value from +12.
    constexpr std::size_t kNumber = 2, kJump = 6, kValue = 11, kShortEntry = 13, kLongEntry = 17;
    std::map<uint16_t, std::string> by_effect;
    std::size_t at = kEffects;
    while (at + kShortEntry <= script.size() && script[at] == 0x31 && script[at + 1] == 0 &&
           script[at + kJump] == 0x02 && script[at + kValue] == 0x19)
    {
        const uint32_t number = xsflib::LoadBe32(&script[at + kNumber]);
        std::size_t next = at + kShortEntry;
        if (script[at + kValue + 1] == 0)
        {
            if (at + kLongEntry > script.size())
            {
                break;
            }

            const uint32_t id = xsflib::LoadBe32(&script[at + kValue + 2]);
            if (number < names.size() && id <= 0xffff)
            {
                by_effect.try_emplace(static_cast<uint16_t>(id), names[number]);
            }
            next = at + kLongEntry;
        }
        if (next + 2 > script.size() || script[next] != 0x1f || script[next + 1] != 0x35)
        {
            break;
        }

        at = next + 2;
    }

    return by_effect;
}

// Pokémon Channel (USA). en/pch/pchsound.proj, .pool, .sdir and .samp hold every MusyX group of the game. The game
// plays its music as MusyX effects: the effect's layer starts two voices that play a stereo recording, panned hard
// left and right. The game's code lists its music at 0x8015EB50, in 38 rows of 8 bytes: the effect's ID, then the
// number of a sound whose group the game pushes first, a big-endian word each. The game's music function (0x80015398)
// pushes the group that the project gives the effect. It then starts the effect. The music comes first, in the order
// of the table. The project's other effects follow, in the order of their IDs. Each sound plays from its group alone.
//
// The game starts every sound through 0x80015570: sndFXStartEx (0x80119b38) with a volume of 0x7F and the default
// pan, then MIDI controller 7 with the sound's volume, and the pan through controllers 0x83 (the surround pan) and 10.
// The music plays at a volume of 127 and a pan of 0x40 until a scene changes them. A scene's effect takes the volume
// and the pan of an object of the scene. dolphinsf2 plays every sound at the music's volume and pan. The sound set-up
// (0x8001498C) calls sndMasterVolume(100, 100, 1, 1) and sndVolume(127, 100, 0xFF). The game's sound viewer names most
// of the sounds (ViewerNames). A sound that it doesn't name is named after the project and its ID, such as
// "pchsound-1064".
Catalogue GatherPokemonChannel(const ntdlib::disc::GameFiles& disc)
{
    constexpr uint32_t kMusic = 0x8015eb50, kPieces = 38, kRow = 8;
    constexpr uint8_t kStartVolume = 0x7f;
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc.dol);

    Catalogue catalogue;
    catalogue.sets.push_back({.project = ReadDiscFile(disc, "en/pch/pchsound.proj"),
                              .pool = ReadDiscFile(disc, "en/pch/pchsound.pool"),
                              .sdir = ReadDiscFile(disc, "en/pch/pchsound.sdir"),
                              .samples = ReadDiscFile(disc, "en/pch/pchsound.samp")});
    std::vector<SetEffect> effects = ProjectEffectsOf(catalogue.sets[0].project, 0);
    KeepFirstOfEachId(effects);
    const std::map<uint16_t, std::string> names = ViewerNames(ReadDiscFile(disc, "pch/scripts/sndviewer.pkc"));
    const auto add = [&](const SetEffect& effect)
    {
        CatalogueSound sound;
        const auto name = names.find(effect.id);
        sound.name = name != names.end() ? name->second : EffectName("pchsound", effect.id);
        sound.effect = true;
        sound.id = effect.id;
        sound.groups = {{.id = effect.group, .set = 0}};
        sound.volume = kStartVolume;
        catalogue.sounds.push_back(std::move(sound));
    };

    // The music, then the other effects.
    std::vector<uint16_t> music;
    for (uint32_t i = 0; i < kPieces; i++)
    {
        const uint32_t id = xsflib::LoadBe32(DolBytes(dol, kMusic + i * kRow, kRow, "the table of music").data());
        const auto is_id = [id](const SetEffect& e)
        {
            return e.id == id;
        };
        const auto found = std::find_if(effects.begin(), effects.end(), is_id);
        if (found == effects.end())
        {
            throw FormatError("the table of music names an effect that en/pch/pchsound.proj doesn't hold");
        }

        add(*found);
        music.push_back(found->id);
    }

    for (const SetEffect& effect : effects)
    {
        if (std::find(music.begin(), music.end(), effect.id) == music.end())
        {
            add(effect);
        }
    }

    return catalogue;
}

// Converts a UTF-16 string in big-endian halves to UTF-8. The string ends at a half of 0. A lone surrogate becomes
// U+FFFD.
std::string Utf16Be(std::span<const uint8_t> bytes)
{
    std::string out;
    for (std::size_t at = 0; at + 2 <= bytes.size(); at += 2)
    {
        const uint32_t c = xsflib::LoadBe16(bytes.data() + at);
        if (c == 0)
        {
            break;
        }

        const bool high = c >= 0xd800 && c < 0xdc00;
        const uint32_t low = at + 4 <= bytes.size() ? xsflib::LoadBe16(bytes.data() + at + 2) : 0;
        if (high && low >= 0xdc00 && low < 0xe000)
        {
            xsflib::AppendUtf8(out, 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00));
            at += 2;
        }
        else
        {
            xsflib::AppendUtf8(out, c >= 0xd800 && c < 0xe000 ? 0xfffd : c);
        }
    }

    return out;
}

// The bytes of the file `path` in the pack whose list is `dir`. `folder`/data.dat packs most of the game's files.
// `folder`/data.dir lists them in lines of text: the file's path, padded with spaces to 64 characters, then its offset
// in the pack and its size in decimal, and two more numbers, all split by tabs. Rebel Strike's list pads the numbers
// with spaces on the left. Throw FormatError if the list doesn't give the file, or if it runs past the pack's end.
std::vector<uint8_t> PackedFile(const ntdlib::disc::GameFiles& disc, const std::string& folder, const std::string& path)
{
    constexpr std::size_t kNameWidth = 64;
    const std::string list = folder + "/data.dir";
    const std::vector<uint8_t> dir = ReadDiscFile(disc, list);
    const std::string text(dir.begin(), dir.end());
    for (std::size_t at = 0; at < text.size();)
    {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.size() <= kNameWidth || line.substr(0, line.find_last_not_of(' ', kNameWidth - 1) + 1) != path)
        {
            continue;
        }

        std::array<std::size_t, 2> numbers{};
        std::size_t field = kNameWidth;
        for (std::size_t& number : numbers)
        {
            while (field < line.size() && line[field] == ' ')
            {
                field++;
            }

            std::size_t digits = 0;
            for (; field < line.size() && line[field] >= '0' && line[field] <= '9'; field++, digits++)
            {
                number = number * 10 + static_cast<std::size_t>(line[field] - '0');
            }
            if (digits == 0 || field >= line.size() || line[field] != '\t')
            {
                throw FormatError(list + "'s line for " + path + " is damaged");
            }

            field++;
        }

        const std::vector<uint8_t> pack = ReadDiscFile(disc, folder + "/data.dat");
        const std::span<const uint8_t> bytes = Part(pack, numbers[0], numbers[1], path.c_str());
        return {bytes.begin(), bytes.end()};
    }

    throw FormatError(list + " doesn't list " + path);
}

// The big-endian word at `at` in a Rogue Squadron game's audio/data (see ReadRogueBanks).
uint32_t AudioWord(std::span<const uint8_t> audio, std::size_t at)
{
    return xsflib::LoadBe32(Part(audio, at, 4, "audio/data").data());
}

// A song's row in a bank's table of songs (see ReadRogueBanks), with the number of its bank.
struct RogueSong
{
    uint32_t offset = 0, size = 0;
    uint16_t group = 0, id = 0;
    std::size_t bank = 0;
};

// The banks of a Rogue Squadron game's audio/data: each bank's MusyX files and groups, and the songs of every bank.
struct RogueBanks
{
    std::vector<FileSet> files;
    std::vector<std::vector<PushedGroup>> groups;
    std::vector<RogueSong> songs;
};

// The banks of audio/data, in the order of its table of banks, and their songs in the order of each bank's table. Bank
// n is file set n. audio/data starts with the size of its index and the offset of its table of banks. The table gives
// the number of banks, then the offset of each bank's entry. An entry gives the offsets and sizes of MusyX's four files
// (the project, the pool, the sample directory and the samples), the offset and size of a block before the songs, the
// number of songs, the offset of their table and the offset of the bank's list of groups. A song's row in the table
// gives the song's offset and size, then its group and its ID in big-endian halves. The list of groups holds
// big-endian halves and ends with 0xFFFF. Every word is big-endian. Every offset counts from the start of audio/data.
RogueBanks ReadRogueBanks(std::span<const uint8_t> audio)
{
    constexpr std::size_t kBankTable = 4, kBankEntry = 13 * 4, kSongRow = 12;
    constexpr uint16_t kEndOfGroups = 0xffff;
    RogueBanks banks;
    const uint32_t table = AudioWord(audio, kBankTable);
    const uint32_t count = AudioWord(audio, table);
    for (uint32_t b = 0; b < count; b++)
    {
        const uint32_t entry = AudioWord(audio, table + 4 + b * 4);
        Part(audio, entry, kBankEntry, "a bank's entry");
        const auto file = [&](std::size_t n)
        {
            const std::span<const uint8_t> bytes =
                Part(audio, AudioWord(audio, entry + n * 8), AudioWord(audio, entry + n * 8 + 4), "a bank");
            return std::vector<uint8_t>(bytes.begin(), bytes.end());
        };
        banks.files.push_back({.project = file(0), .pool = file(1), .sdir = file(2), .samples = file(3)});

        std::vector<PushedGroup> groups;
        for (uint32_t at = AudioWord(audio, entry + 48);; at += 2)
        {
            const uint16_t group = xsflib::LoadBe16(Part(audio, at, 2, "a bank's groups").data());
            if (group == kEndOfGroups)
            {
                break;
            }

            groups.push_back({.id = group, .set = b});
        }
        banks.groups.push_back(std::move(groups));

        const uint32_t songs = AudioWord(audio, entry + 40), song_table = AudioWord(audio, entry + 44);
        for (uint32_t s = 0; s < songs; s++)
        {
            const std::span<const uint8_t> row = Part(audio, song_table + s * kSongRow, kSongRow, "a bank's songs");
            const uint32_t key = xsflib::LoadBe32(row.data() + 8);
            banks.songs.push_back({.offset = xsflib::LoadBe32(row.data()),
                                   .size = xsflib::LoadBe32(row.data() + 4),
                                   .group = static_cast<uint16_t>(key >> 16),
                                   .id = static_cast<uint16_t>(key),
                                   .bank = b});
        }
    }

    return banks;
}

// Star Wars Rogue Squadron II: Rogue Leader (USA). main.dol is a loader. It holds the OS, MusyX and the game, a second
// DOL at 0x80100000. Rogue2/data.dat packs audio/data (PackedFile), which holds the sound in banks (ReadRogueBanks).
// The game's sound set-up (0x7FD49254 in the game) calls sndVolume(127, 0, 0xFF) and leaves the master volumes at
// 127.
//
// The game's music hall lists its songs at 0x80060C38, in 114 entries of 16 bytes: the song's ID and group in
// big-endian halves, then the address of its name, in UTF-16 in the game's DOL. The byte at 0xA gives the volume at
// which the hall plays the song. The hall starts a song through 0x7FD4CE78 in the game (0x7FD71AD4 and 0x7FD71C78) with
// a time of 300 ms. That time goes to sndSeqPlayEx's volume (flag 4), which moves the song's volume group from its
// level to the song's volume (SetupFader). The group's level depends on the song that played before. The files leave
// that out. The levels start their songs through the same function with a time of 0. The game loads a bank whole and
// pushes each of its groups. The songs of most banks play samples of the bank that holds the hall's first song. A song
// pushes that bank's groups, then the groups of its bank, each bank's in its order. The songs that the hall doesn't
// list follow, at MusyX's volume. The menus play one of them, group 0x6E's song 0x65, at 0x67 in two of their three
// calls (0x7FCD9A28 and 0x7FD719A8 in the game). It takes that volume. Eight songs play a stream along with their
// notes: The Battle Of Yavin I and III, Hyperspace, Imperial Attack, The Asteroid Field, Celebration & End Titles, and
// two that the hall doesn't list, group 0x4D's song 0x1A and group 0x61's song 0x3D. A MIDI file can't hold the stream.
// Those songs are left out. Three more play only a stream, through groups whose other songs play notes: Tatooine
// Training Cutscene 3 and 4 and Prisons Of The Maw Cutscene 1. Each starts one note, whose voice plays a stream buffer.
// They're left out too. BuildMusic leaves out the songs whose groups hold only stream buffers.
//
// Banks 0 to 19 hold the game's effects, in the order of the banks and of each bank's project. Some effects of banks 1
// to 19 play samples of bank 0. An effect pushes the groups of bank 0, then those of its bank. The game's table of
// sounds (0x7FDDBF30 in the game) holds 522 rows of 12 bytes. A row gives an effect's ID in a big-endian half and the
// effect's volume in the byte at 3. An ID of 0xFFFF marks an empty row. The function at 0x7FD5123C plays a row through
// sndFXStartEx, with a pan from its caller and studio 0. An effect plays at the most common volume of its rows, in the
// centre. An effect that no row names takes the volume and the pan of its row in its group's table of effects. An
// effect is named after its ID, such as "RogueLeader-0048".
Catalogue GatherRogueLeader(const ntdlib::disc::GameFiles& disc)
{
    constexpr uint32_t kGame = 0x80100000, kHall = 0x80060c38, kHallSongs = 114, kHallEntry = 16;
    constexpr uint32_t kSounds = 0x7fddbf30, kSoundRows = 522, kSoundRow = 12;
    constexpr uint16_t kNoEffect = 0xffff;
    constexpr uint8_t kCentre = 0x40;
    constexpr uint32_t kMenuSong = 0x006e0065;
    constexpr uint8_t kMenuVolume = 0x67;
    constexpr std::array<uint32_t, 11> kStreamed = {0x0046000b, 0x0072006d, 0x005a0030, 0x005d0039,
                                                    0x0047000c, 0x004d001b, 0x004d001a, 0x0061003d,
                                                    0x00620040, 0x00620041, 0x0065004b};
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc.dol);
    const std::optional<ntdlib::Dol> game = ntdlib::Dol::Parse(dol.At(kGame));
    if (!game)
    {
        throw FormatError("main.dol holds no second DOL at 0x80100000");
    }

    const std::vector<uint8_t> audio = PackedFile(disc, "Rogue2", "audio/data");
    RogueBanks banks = ReadRogueBanks(audio);
    Catalogue catalogue;
    catalogue.sets = std::move(banks.files);

    // The bank of the hall's first song holds the samples that the songs of the other banks play too.
    std::optional<std::size_t> base;
    std::vector<bool> listed(banks.songs.size(), false);
    const auto add = [&](std::size_t r, std::string name, std::optional<uint8_t> volume)
    {
        const RogueSong& row = banks.songs[r];
        base = base.value_or(row.bank);
        const uint32_t key = uint32_t{row.group} << 16 | row.id;
        if (std::find(kStreamed.begin(), kStreamed.end(), key) != kStreamed.end())
        {
            return;
        }

        const std::span<const uint8_t> song = Part(audio, row.offset, row.size, "a song");
        CatalogueSound sound;
        sound.name = std::move(name);
        sound.volume = volume;
        sound.id = row.id;
        sound.song_group = row.group;
        sound.song = catalogue.songs.size();
        if (row.bank != *base)
        {
            sound.groups = banks.groups[*base];
        }
        sound.groups.insert(sound.groups.end(), banks.groups[row.bank].begin(), banks.groups[row.bank].end());
        catalogue.songs.emplace_back(song.begin(), song.end());
        catalogue.sounds.push_back(std::move(sound));
    };

    for (uint32_t i = 0; i < kHallSongs; i++)
    {
        const std::span<const uint8_t> hall = DolBytes(dol, kHall + i * kHallEntry, kHallEntry, "the music hall");
        const uint16_t id = xsflib::LoadBe16(hall.data()), group = xsflib::LoadBe16(hall.data() + 2);
        const auto is_song = [&](const RogueSong& r)
        {
            return r.group == group && r.id == id;
        };
        const auto row = std::find_if(banks.songs.begin(), banks.songs.end(), is_song);
        if (row == banks.songs.end())
        {
            throw FormatError("audio/data has no song " + std::to_string(id) + " in group " + std::to_string(group));
        }

        const auto r = static_cast<std::size_t>(row - banks.songs.begin());
        listed[r] = true;
        add(r, Utf16Be(game->At(xsflib::LoadBe32(hall.data() + 4))), hall[0xa]);
    }

    for (std::size_t r = 0; r < banks.songs.size(); r++)
    {
        const RogueSong& row = banks.songs[r];
        if (listed[r])
        {
            continue;
        }

        char name[24];
        std::snprintf(name, sizeof(name), "Song 0x%02X-0x%02X", static_cast<unsigned>(row.group),
                      static_cast<unsigned>(row.id));
        const uint32_t key = uint32_t{row.group} << 16 | row.id;
        add(r, name, key == kMenuSong ? std::optional<uint8_t>(kMenuVolume) : std::nullopt);
    }

    // The effects, bank by bank, at the volumes that the table of sounds gives them.
    std::map<uint16_t, std::vector<uint8_t>> volumes;
    for (uint32_t i = 0; i < kSoundRows; i++)
    {
        const std::span<const uint8_t> row = DolBytes(*game, kSounds + i * kSoundRow, kSoundRow, "the table of sounds");
        if (const uint16_t id = xsflib::LoadBe16(row.data()); id != kNoEffect)
        {
            volumes[id].push_back(row[3]);
        }
    }

    for (std::size_t b = 0; b < catalogue.sets.size(); b++)
    {
        for (const SetEffect& effect : ProjectEffectsOf(catalogue.sets[b].project, b))
        {
            CatalogueSound sound;
            sound.name = EffectName("RogueLeader", effect.id);
            sound.effect = true;
            sound.id = effect.id;
            if (const auto found = volumes.find(effect.id); found != volumes.end())
            {
                sound.volume = MostCommon(found->second);
                sound.pan = kCentre;
            }
            if (b != 0)
            {
                sound.groups = banks.groups[0];
            }
            sound.groups.insert(sound.groups.end(), banks.groups[b].begin(), banks.groups[b].end());
            catalogue.sounds.push_back(std::move(sound));
        }
    }

    return catalogue;
}

// Star Wars Rogue Squadron III: Rebel Strike (USA, disc version 1). main.dol is a loader. The game's code and data,
// MusyX among them, are a second DOL that main.dol holds packed. Rogue3/data.dat packs the game's files as Rogue
// Leader's Rogue2/data.dat does (PackedFile). The banks in its audio/data have the same layout too (ReadRogueBanks).
// The game streams its music. The 172 banks list 871 effects. Many banks hold copies of the same groups. Each effect ID
// belongs to one group. An effect plays from the first bank whose project lists it. It pushes the effect's group alone.
// The game passes a volume and a pan of 0xFF to sndFXStartEx: an effect plays at the volume and the pan of its row in
// its group's table of effects. The game's set-up calls sndVolume(127, 0, 0xFF) and leaves the master volumes at 127.
// An effect is named after its ID, such as "RebelStrike-0048".
Catalogue GatherRebelStrike(const ntdlib::disc::GameFiles& disc)
{
    const std::vector<uint8_t> audio = PackedFile(disc, "Rogue3", "audio/data");
    RogueBanks banks = ReadRogueBanks(audio);
    std::vector<SetEffect> effects;
    for (std::size_t b = 0; b < banks.files.size(); b++)
    {
        const std::vector<SetEffect> listed = ProjectEffectsOf(banks.files[b].project, b);
        effects.insert(effects.end(), listed.begin(), listed.end());
    }

    KeepFirstOfEachId(effects);

    // Only the banks that the effects take become file sets.
    Catalogue catalogue;
    std::map<std::size_t, std::size_t> sets;
    for (const SetEffect& effect : effects)
    {
        const auto [set, added] = sets.try_emplace(effect.set, catalogue.sets.size());
        if (added)
        {
            catalogue.sets.push_back(std::move(banks.files[effect.set]));
        }

        CatalogueSound sound;
        sound.name = EffectName("RebelStrike", effect.id);
        sound.effect = true;
        sound.id = effect.id;
        sound.groups = {{.id = effect.group, .set = set->second}};
        catalogue.sounds.push_back(std::move(sound));
    }

    return catalogue;
}

// The disc's files, found by their paths in any case. The disc libraries of Tales of Symphonia and Resident Evil Zero
// match paths in any case.
class AnyCaseFiles
{
public:
    explicit AnyCaseFiles(const ntdlib::disc::GameFiles& disc) : disc_(disc)
    {
        for (const ntdlib::disc::GameFile& file : disc.files)
        {
            paths_.emplace(xsflib::AsciiLower(file.path), file.path);
        }
    }

    // The disc's paths, by their paths in lower case. Of two paths that differ only in case, the first on the disc
    // stands for both.
    const std::map<std::string, std::string>& Paths() const
    {
        return paths_;
    }

    // The disc's path of the file whose path matches `path` in any case, or null if the disc has none.
    const std::string* Find(const std::string& path) const
    {
        const auto found = paths_.find(xsflib::AsciiLower(path));
        return found == paths_.end() ? nullptr : &found->second;
    }

    // The file whose path matches `path` in any case. Throw FormatError if the disc has none, or if it can't be read.
    std::vector<uint8_t> Read(const std::string& path) const
    {
        const std::string* found = Find(path);
        if (found == nullptr)
        {
            throw FormatError("the disc has no " + path);
        }

        return ReadDiscFile(disc_, *found);
    }

private:
    const ntdlib::disc::GameFiles& disc_;
    std::map<std::string, std::string> paths_;
};

// The files of `folder` (in lower case, ending with a slash) whose names end with `extension`, outside its subfolders,
// in the order of their paths in lower case: each one's path in lower case and on the disc.
std::vector<std::pair<std::string, std::string>> FilesIn(const AnyCaseFiles& files, std::string_view folder,
                                                         std::string_view extension)
{
    std::vector<std::pair<std::string, std::string>> found;
    for (const auto& [lower, path] : files.Paths())
    {
        if (lower.starts_with(folder) && lower.ends_with(extension) &&
            lower.find('/', folder.size()) == std::string::npos)
        {
            found.emplace_back(lower, path);
        }
    }

    return found;
}

// The ID of the first group in `project`, a MusyX project file. A group's header starts with the offset of the next
// group, then the group's ID in a big-endian half.
uint16_t FirstGroupId(std::span<const uint8_t> project)
{
    return xsflib::LoadBe16(Part(project, 4, 2, "a project").data());
}

// A part of one of Resident Evil Zero's archives: its type and its bytes.
struct ArchivePart
{
    std::string type;
    std::span<const uint8_t> data;
};

// The parts of one of Resident Evil Zero's archives (.arc). The archive starts with a word 0, the number of its parts
// and the size of its header. Each part then has 0x20 bytes: its offset and size, its name in 8 bytes and its type in
// 16. Every word is big-endian.
std::vector<ArchivePart> ArchiveParts(std::span<const uint8_t> archive)
{
    constexpr std::size_t kPart = 0x20, kType = 0x10, kTypeSize = 16;
    const std::span<const uint8_t> header = Part(archive, 0, 12, "an archive's header");
    const uint32_t count = xsflib::LoadBe32(header.data() + 4);
    const uint32_t first = xsflib::LoadBe32(header.data() + 8);
    std::vector<ArchivePart> parts;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::span<const uint8_t> row = Part(archive, first + std::size_t{i} * kPart, kPart, "an archive's parts");
        parts.push_back({.type = TextBeforeZero(row.subspan(kType, kTypeSize)),
                         .data = Part(archive, xsflib::LoadBe32(row.data()), xsflib::LoadBe32(row.data() + 4),
                                      "an archive's part")});
    }

    return parts;
}

// The contents of one of Resident Evil Zero's compressed files (.alz). The file's bits come from each byte's lowest bit
// up. The header gives the mode in 8 bits and the size of the contents in 32. Mode 0 stores the contents after the
// header. In modes 1 and 2, a 1 bit starts a literal byte in the next 8 bits. A 0 bit starts a copy of earlier
// contents: its distance less 1, then its length. Mode 1 gives the distance in 10 bits. Mode 2 gives it as a class and
// a value. A class is the number of 0 bits before a 1 bit. Classes 0 to 3 give the value in 2, 4, 6 and 10 bits. The
// length is a class and a value in both modes. The game copies the bytes as memmove does (0x80007360). Throw
// FormatError if the file ends early, if a copy reaches outside the contents, or if the contents would be larger than
// the console's memory.
std::vector<uint8_t> Unalz(std::span<const uint8_t> file)
{
    constexpr uint32_t kMaxSize = 24 << 20;
    constexpr std::array<uint32_t, 4> kWidths = {2, 4, 6, 10};
    constexpr std::size_t kHeader = 5;
    std::size_t bit = 0;
    const auto read = [&](uint32_t count)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; i++, bit++)
        {
            if (bit / 8 >= file.size())
            {
                throw FormatError("a compressed file ends early");
            }

            value |= uint32_t{(file[bit / 8] >> (bit % 8)) & 1u} << i;
        }

        return value;
    };
    const auto width = [&]
    {
        std::size_t zeros = 0;
        while (read(1) == 0)
        {
            if (++zeros >= kWidths.size())
            {
                throw FormatError("a compressed file is damaged");
            }
        }

        return kWidths[zeros];
    };

    const uint32_t mode = read(8);
    const uint32_t size = read(32);
    if (size > kMaxSize)
    {
        throw FormatError("a compressed file is too large");
    }
    if (mode == 0)
    {
        const std::span<const uint8_t> stored = Part(file, kHeader, size, "a stored file");
        return {stored.begin(), stored.end()};
    }

    std::vector<uint8_t> contents(size);
    for (std::size_t at = 0; at < size;)
    {
        if (read(1) != 0)
        {
            contents[at++] = static_cast<uint8_t>(read(8));
            continue;
        }

        const uint32_t distance = read(mode == 1 ? 10 : width()) + 1;
        const uint32_t length = read(width());
        if (distance > at || length > size - at)
        {
            throw FormatError("a compressed file copies from outside its contents");
        }

        std::memmove(contents.data() + at, contents.data() + at - distance, length);
        at += length;
    }

    return contents;
}

// A song of one of Resident Evil Zero's cutscenes: the files of its group, and the song.
struct CutsceneSong
{
    FileSet set;
    std::vector<uint8_t> song;
};

// The sound effects of one of Resident Evil Zero's cutscenes, which the game plays as songs: the cutscene's name, the
// ID of the effects' group, the group's files, and the songs in the order of their parts.
struct CutsceneEffects
{
    std::string name;
    uint16_t group = 0;
    FileSet set;
    std::vector<std::vector<uint8_t>> songs;
};

// The sounds of Resident Evil Zero's cutscenes: the songs of their music, by the ID of each song's group, and their
// sound effects, in the order of the cutscenes' paths in lower case.
struct Cutscenes
{
    std::map<uint16_t, CutsceneSong> songs;
    std::vector<CutsceneEffects> effects;
};

// The sounds of Resident Evil Zero's cutscenes. The game loads a cutscene's compressed archive, bio0/demo/<name>.alz
// (0x80169280). Part 7 of the archive holds the music: an archive of MusyX's pool, project and sample directory, then
// the song. Part 1 of bio0/demo/<name>sam.arc holds the music's samples (0x8016A3C0). The ID of the project's first
// group is the group in the song's row of the game's list. The music's files come from the first cutscene that holds
// the group, in the order of the paths in lower case. Part 6 of the archive holds the cutscene's sound effects, if it
// has any: the pool, the project and the sample directory of their group, then an archive of songs. Part 0 of the
// archive of samples holds their samples.
Cutscenes ReadCutscenes(const AnyCaseFiles& files)
{
    constexpr std::string_view kCutscenes = "bio0/demo/", kCompressed = ".alz";
    constexpr std::size_t kEffectsPart = 6, kMusicPart = 7, kEffectSamples = 0, kMusicSamples = 1;
    const auto bytes = [](std::span<const uint8_t> data)
    {
        return std::vector<uint8_t>(data.begin(), data.end());
    };
    // A part of the archive that holds MusyX's pool, project and sample directory, and then the songs.
    const auto group_files = [](const ArchivePart& part, const std::string& path)
    {
        std::vector<ArchivePart> group = ArchiveParts(part.data);
        if (group.size() < 4)
        {
            throw FormatError(path + " lacks a part of its sounds");
        }

        return group;
    };

    Cutscenes cutscenes;
    for (const auto& [lower, path] : FilesIn(files, kCutscenes, kCompressed))
    {
        const std::string stem = lower.substr(0, lower.size() - kCompressed.size());
        const std::string* samples = files.Find(stem + "sam.arc");
        if (samples == nullptr)
        {
            continue;
        }

        const std::vector<uint8_t> archive = Unalz(files.Read(path));
        const std::vector<ArchivePart> parts = ArchiveParts(archive);
        const auto holds = [&parts](std::size_t part)
        {
            return parts.size() > part && !parts[part].data.empty();
        };
        if (!holds(kEffectsPart) && !holds(kMusicPart))
        {
            continue;
        }

        const std::vector<uint8_t> sample_archive = files.Read(*samples);
        const std::vector<ArchivePart> sample_parts = ArchiveParts(sample_archive);
        if (sample_parts.size() <= kMusicSamples)
        {
            throw FormatError(*samples + " lacks a part of its samples");
        }

        if (holds(kMusicPart))
        {
            const std::vector<ArchivePart> music = group_files(parts[kMusicPart], path);
            cutscenes.songs.try_emplace(FirstGroupId(music[1].data),
                                        CutsceneSong{.set = {.project = bytes(music[1].data),
                                                             .pool = bytes(music[0].data),
                                                             .sdir = bytes(music[2].data),
                                                             .samples = bytes(sample_parts[kMusicSamples].data)},
                                                     .song = bytes(music[3].data)});
        }

        if (holds(kEffectsPart))
        {
            const std::vector<ArchivePart> effects = group_files(parts[kEffectsPart], path);
            CutsceneEffects sounds{
                .name = path.substr(kCutscenes.size(), path.size() - kCutscenes.size() - kCompressed.size()),
                .group = FirstGroupId(effects[1].data),
                .set = {.project = bytes(effects[1].data),
                        .pool = bytes(effects[0].data),
                        .sdir = bytes(effects[2].data),
                        .samples = bytes(sample_parts[kEffectSamples].data)}};
            for (const ArchivePart& song : ArchiveParts(effects[3].data))
            {
                sounds.songs.push_back(bytes(song.data));
            }

            cutscenes.effects.push_back(std::move(sounds));
        }
    }

    return cutscenes;
}

// Resident Evil Zero (USA). Both discs hold the same main.dol. The game's code lists its songs at 0x80243BCC, 12 bytes
// each: MusyX's song ID and the song's group in big-endian halves, then the addresses of two names. The first names
// the archive that holds the group, and the second the song's file. A song ID of 0xFFFF ends the list. The game reads
// the files from bio0/sound/: the archive (.arc), its samples (.sam) and the song (.son). The archive's parts of the
// types "proj", "pool" and "sdir" are MusyX's project, pool and sample directory. The list names the songs of both
// discs, and songs that neither disc holds. Some of the songs that bio0/sound/ lacks are the music of a cutscene
// (ReadCutscenes). The songs that the disc holds come in the list's order.
//
// The game pushes a song's group and plays the song through sndSeqPlayEx. A room or a cutscene starts a song at the
// volume of one of the game's sound options. Each option starts at 127 (0x80148F8C). A cutscene raises its music to
// that volume over 133 ms: the cutscene's code (0x8016C154 and 0x8016C254) passes the time to the song start
// (0x8014AAB8 and 0x8014A9C0). That starts the song at a volume of 0 and calls sndSeqVolume (0x801CF0DC) with the time.
// The rooms pass a time of 0 (0x800183BC and 0x80018478). No room plays bgm_52. The extras play it at 100/127
// of the music's option (0x8017B970). The set-up calls sndMasterVolume(127, 0, 1, 1) and sndVolume(127, 0, 0xFF)
// (0x80148F70 and 0x80148F88).
//
// The effects follow the songs. Each archive of bio0/sound/ whose project lists effects holds one group of them, in
// the order of the archives' names. The game's start-up pushes the group of all.arc and keeps it. The effects of each
// archive start only macros and samples of that archive. An effect pushes the group of all.arc, then its group. It
// plays at the volume and the pan of its row in its group's table of effects. A row of volume 0 makes no sound at
// that volume. Such rows look like the tables' unused rows. Their effects are left out. An effect is named after its
// archive and its ID, such as "sh00-1467".
//
// Some cutscenes play sound effects as songs. 0x8016C06C plays song N of a cutscene's effects (ReadCutscenes) through
// 0x8014AAB8, with the 133 ms fade-in of the cutscenes' music. The game's table of the cutscenes' effects, at
// 0x80258BE4, gives each of the cutscenes 0 to 134 four bytes: the ID of the effects' group and the song ID of their
// first song, in big-endian halves. Song N takes the ID that follows the first by N. A group of 0 means that the
// cutscene plays no effects as songs. The effects of each cutscene whose group the table names follow the other
// effects, in the order of the cutscenes' paths in lower case. Each is named after the cutscene and its song ID, such
// as "dd01-0413".
Catalogue GatherResidentEvil(const ntdlib::disc::GameFiles& disc)
{
    constexpr uint32_t kSongs = 0x80243bcc, kEntry = 12;
    constexpr uint32_t kCutsceneEffects = 0x80258be4, kCutsceneEntry = 4, kCutsceneCount = 135;
    constexpr uint16_t kEnd = 0xffff, kExtrasSong = 0x90;
    constexpr uint8_t kOptionVolume = 0x7f, kExtrasVolume = 100;
    constexpr double kCutsceneFadeInMs = 133.0;
    constexpr std::string_view kSounds = "bio0/sound/", kArchive = ".arc";
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc.dol);
    const AnyCaseFiles files(disc);
    const Cutscenes cutscenes = ReadCutscenes(files);
    const std::map<uint16_t, CutsceneSong>& cutscene_songs = cutscenes.songs;

    // The file set of an archive in bio0/sound/, by the archive's name. The first call for an archive adds its set.
    Catalogue catalogue;
    std::map<std::string, std::size_t> sets;
    const auto archive_set = [&](const std::string& archive)
    {
        const auto [set, added] = sets.try_emplace(archive, catalogue.sets.size());
        if (added)
        {
            const std::string stem = std::string(kSounds) + archive;
            const std::vector<uint8_t> file = files.Read(stem + ".arc");
            const std::vector<ArchivePart> parts = ArchiveParts(file);
            const auto part = [&](const char* type)
            {
                const auto is_type = [type](const ArchivePart& p)
                {
                    return p.type == type;
                };
                const auto found = std::find_if(parts.begin(), parts.end(), is_type);
                if (found == parts.end())
                {
                    throw FormatError(stem + ".arc has no part of the type " + type);
                }

                return std::vector<uint8_t>(found->data.begin(), found->data.end());
            };
            catalogue.sets.push_back({.project = part("proj"),
                                      .pool = part("pool"),
                                      .sdir = part("sdir"),
                                      .samples = files.Read(stem + ".sam")});
        }

        return set->second;
    };

    std::map<uint16_t, std::size_t> cutscene_sets;
    for (uint32_t at = kSongs;; at += kEntry)
    {
        const std::span<const uint8_t> row = DolBytes(dol, at, kEntry, "the table of songs");
        const uint16_t id = xsflib::LoadBe16(row.data());
        if (id == kEnd)
        {
            break;
        }

        CatalogueSound sound;
        sound.name = DolText(dol, xsflib::LoadBe32(row.data() + 8), "a song's name");
        sound.volume = id == kExtrasSong ? kExtrasVolume : kOptionVolume;
        sound.id = id;
        sound.song_group = xsflib::LoadBe16(row.data() + 2);
        const std::string song_path = std::string(kSounds) + sound.name + ".son";
        if (files.Find(song_path) != nullptr)
        {
            const std::string archive = DolText(dol, xsflib::LoadBe32(row.data() + 4), "an archive's name");
            sound.groups = {{.id = sound.song_group, .set = archive_set(archive)}};
            catalogue.songs.push_back(files.Read(song_path));
        }
        else if (const auto cutscene = cutscene_songs.find(sound.song_group); cutscene != cutscene_songs.end())
        {
            const auto [set, added] = cutscene_sets.try_emplace(sound.song_group, catalogue.sets.size());
            if (added)
            {
                catalogue.sets.push_back(cutscene->second.set);
            }
            sound.groups = {{.id = sound.song_group, .set = set->second}};
            sound.fade_in_ms = kCutsceneFadeInMs;
            catalogue.songs.push_back(cutscene->second.song);
        }
        else
        {
            continue;
        }

        sound.song = catalogue.songs.size() - 1;
        catalogue.sounds.push_back(std::move(sound));
    }

    // The effects of each archive, after the group of all.arc.
    const std::size_t common = archive_set("all");
    const PushedGroup common_group = {.id = FirstGroupId(catalogue.sets[common].project), .set = common};
    for (const auto& [lower, path] : FilesIn(files, kSounds, kArchive))
    {
        const std::vector<uint8_t> file = files.Read(path);
        const std::vector<ArchivePart> parts = ArchiveParts(file);
        const auto is_project = [](const ArchivePart& p)
        {
            return p.type == "proj";
        };
        const auto project = std::find_if(parts.begin(), parts.end(), is_project);
        if (project == parts.end() || ProjectEffectsOf(project->data, 0).empty())
        {
            continue;
        }

        const std::string archive = path.substr(kSounds.size(), path.size() - kSounds.size() - kArchive.size());
        const std::size_t set = archive_set(archive);
        for (const SetEffect& effect : ProjectEffectsOf(catalogue.sets[set].project, set))
        {
            if (effect.row.volume == 0)
            {
                continue;
            }

            CatalogueSound sound;
            sound.name = EffectName(archive, effect.id);
            sound.effect = true;
            sound.id = effect.id;
            sound.groups = {common_group};
            if (set != common)
            {
                sound.groups.push_back({.id = effect.group, .set = set});
            }
            catalogue.sounds.push_back(std::move(sound));
        }
    }

    // The cutscenes' effects that the game plays as songs, by the song ID of each group's first song.
    std::map<uint16_t, uint16_t> first_songs;
    for (uint32_t i = 0; i < kCutsceneCount; i++)
    {
        const std::span<const uint8_t> row =
            DolBytes(dol, kCutsceneEffects + i * kCutsceneEntry, kCutsceneEntry, "the table of the cutscenes' effects");
        const uint16_t group = xsflib::LoadBe16(row.data());
        if (group != 0)
        {
            first_songs.try_emplace(group, xsflib::LoadBe16(row.data() + 2));
        }
    }

    for (const CutsceneEffects& effects : cutscenes.effects)
    {
        const auto first = first_songs.find(effects.group);
        if (first == first_songs.end())
        {
            continue;
        }

        const std::size_t set = catalogue.sets.size();
        catalogue.sets.push_back(effects.set);
        for (std::size_t n = 0; n < effects.songs.size(); n++)
        {
            CatalogueSound sound;
            sound.id = static_cast<uint16_t>(first->second + n);
            sound.name = EffectName(effects.name, sound.id);
            sound.volume = kOptionVolume;
            sound.song_group = effects.group;
            sound.groups = {{.id = effects.group, .set = set}};
            sound.fade_in_ms = kCutsceneFadeInMs;
            catalogue.songs.push_back(effects.songs[n]);
            sound.song = catalogue.songs.size() - 1;
            catalogue.sounds.push_back(std::move(sound));
        }
    }

    return catalogue;
}

// MusyX's four files from one file of Tales of Symphonia. The file starts with the number of its parts, 4, then gives
// each part's offset and size, a big-endian word each. The parts are the project, the pool, the sample directory and
// the samples.
FileSet SoundFile(std::span<const uint8_t> file)
{
    constexpr uint32_t kParts = 4;
    if (file.size() < 4 + kParts * 8 || xsflib::LoadBe32(file.data()) != kParts)
    {
        throw FormatError("a sound file doesn't hold four parts");
    }

    std::array<std::vector<uint8_t>, kParts> parts;
    for (uint32_t i = 0; i < kParts; i++)
    {
        const std::span<const uint8_t> part = Part(file, xsflib::LoadBe32(file.data() + 4 + i * 8),
                                                   xsflib::LoadBe32(file.data() + 8 + i * 8), "a sound file's part");
        parts[i].assign(part.begin(), part.end());
    }

    return {.project = std::move(parts[0]),
            .pool = std::move(parts[1]),
            .sdir = std::move(parts[2]),
            .samples = std::move(parts[3])};
}

// Tales of Symphonia (USA). Both discs hold the same main.dol. The game's code lists its songs at 0x801F9830, 8 bytes
// each: the address of the file's name, then MusyX's song ID and the song's kind, a big-endian half each. A song ID of
// 0xFFFF ends the list. The game plays a song through sndSeqPlayEx, with the song ID and the group of S/inst.snd. The
// song plays at the music's volume in the game's options: 127 by default (0x80081D80), its most. The game's disc
// library matches paths in any case. The code names its files in lower case. The disc's folder is "S". The set-up
// calls sndVolume(127, 0, 0xFF) at 0x800A2374 and leaves the master volumes at 127.
//
// The effects follow the songs. Each of the other .snd files in S/ holds one group of effects: se.snd the game's
// common effects, se_ev00.snd to se_ev07.snd the effects of events, and v_01_llo.snd to v_70_bh18.snd the voices in
// battle, in the order of the files' names. The game's start-up (0x800708C8) pushes the music's group and then the
// common effects' group, and keeps both. The common effects take some samples and tables from the music's group. The
// effects of events take samples, macros and tables from the common effects' group. An effect therefore pushes those
// two groups first, then its group. A file without a pool holds no macros. MusyX can't push its group. Its effects are
// left out. The game starts an effect through sndFXStartEx with a volume of 127, a pan of 64 and studio 0. Nothing
// names the effects. An effect is named after its file and its ID, such as "se-0012".
Catalogue GatherTales(const ntdlib::disc::GameFiles& disc)
{
    constexpr uint32_t kSongs = 0x801f9830, kEntry = 8;
    constexpr uint16_t kEnd = 0xffff;
    constexpr uint8_t kMusicVolume = 0x7f, kEffectVolume = 127, kCentre = 64;
    constexpr std::string_view kFolder = "s/", kExtension = ".snd", kCommon = "s/se.snd", kBank = "s/inst.snd";
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc.dol);
    const AnyCaseFiles files(disc);

    Catalogue catalogue;
    catalogue.sets.push_back(SoundFile(files.Read(std::string(kBank))));
    const uint16_t group = FirstGroupId(catalogue.sets[0].project);
    for (uint32_t at = kSongs;; at += kEntry)
    {
        const std::span<const uint8_t> row = DolBytes(dol, at, kEntry, "the table of songs");
        const uint16_t id = xsflib::LoadBe16(row.data() + 4);
        if (id == kEnd)
        {
            break;
        }

        const std::string path = DolText(dol, xsflib::LoadBe32(row.data()), "a song's file");
        const std::string file = path.substr(path.find_last_of('/') + 1);
        CatalogueSound sound;
        sound.name = file.substr(0, file.find_last_of('.'));
        sound.volume = kMusicVolume;
        sound.id = id;
        sound.song_group = group;
        sound.song = catalogue.songs.size();
        sound.groups = {{.id = group, .set = 0}};
        catalogue.songs.push_back(files.Read(path));
        catalogue.sounds.push_back(std::move(sound));
    }

    const std::size_t common = catalogue.sets.size();
    catalogue.sets.push_back(SoundFile(files.Read(std::string(kCommon))));
    const std::vector<PushedGroup> start_groups = {{.id = group, .set = 0},
                                                   {.id = FirstGroupId(catalogue.sets[common].project), .set = common}};
    for (const auto& [lower, path] : FilesIn(files, kFolder, kExtension))
    {
        if (lower == kBank)
        {
            continue;
        }

        std::size_t set = common;
        if (lower != kCommon)
        {
            FileSet file = SoundFile(files.Read(path));
            if (file.pool.empty())
            {
                continue;
            }

            set = catalogue.sets.size();
            catalogue.sets.push_back(std::move(file));
        }

        const std::string name = path.substr(kFolder.size(), path.size() - kFolder.size() - kExtension.size());
        for (const SetEffect& effect : ProjectEffectsOf(catalogue.sets[set].project, set))
        {
            CatalogueSound sound;
            sound.name = EffectName(name, effect.id);
            sound.effect = true;
            sound.volume = kEffectVolume;
            sound.pan = kCentre;
            sound.id = effect.id;
            sound.groups = start_groups;
            if (set != common)
            {
                sound.groups.push_back({.id = effect.group, .set = set});
            }
            catalogue.sounds.push_back(std::move(sound));
        }
    }

    return catalogue;
}

// A resource of a Retro Studios pack: its type, its ID, its name if the pack names it, and its data, decompressed.
struct PakResource
{
    std::string type;
    uint32_t id = 0;
    std::string name;
    std::vector<uint8_t> data;
};

// The resources of a pack whose type is in `types` (Metroid Prime's pack format, version 3.5). The header gives the
// version, then the named resources: each one's type, ID, and the length and bytes of its name. A table follows: the
// number of resources, then for each one whether it's compressed, its type, its ID, its size and its offset. A
// compressed resource holds its size once decompressed, then a zlib stream.
std::vector<PakResource> ReadPak(std::span<const uint8_t> pak, const std::vector<std::string>& types)
{
    constexpr uint16_t kMajor = 3, kMinor = 5;
    if (pak.size() < 12 || xsflib::LoadBe16(pak.data()) != kMajor || xsflib::LoadBe16(pak.data() + 2) != kMinor)
    {
        throw FormatError("a pack isn't of version 3.5");
    }

    std::size_t at = 8;
    const auto word = [&pak, &at]()
    {
        const uint32_t value = xsflib::LoadBe32(Part(pak, at, 4, "a pack's table").data());
        at += 4;
        return value;
    };
    const auto four_cc = [&pak, &at]()
    {
        const std::span<const uint8_t> bytes = Part(pak, at, 4, "a pack's table");
        at += 4;
        return std::string(bytes.begin(), bytes.end());
    };

    std::map<std::pair<std::string, uint32_t>, std::string> names;
    const uint32_t named = word();
    for (uint32_t i = 0; i < named; i++)
    {
        const std::string type = four_cc();
        const uint32_t id = word();
        const uint32_t length = word();
        names[{type, id}] = TextBeforeZero(Part(pak, at, length, "a pack's name"));
        at += length;
    }

    std::vector<PakResource> resources;
    const uint32_t count = word();
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t compressed = word();
        PakResource resource;
        resource.type = four_cc();
        resource.id = word();
        const uint32_t size = word();
        const uint32_t offset = word();
        if (std::find(types.begin(), types.end(), resource.type) == types.end())
        {
            continue;
        }

        const std::span<const uint8_t> stored = Part(pak, offset, size, "a pack's resource");
        if (compressed != 0)
        {
            resource.data.resize(xsflib::LoadBe32(Part(stored, 0, 4, "a compressed resource").data()));
            try
            {
                resource.data.resize(ntdlib::disc::Inflate(stored.subspan(4), resource.data));
            }
            catch (const std::exception&)
            {
                throw FormatError("a pack's compressed resource is damaged");
            }
        }
        else
        {
            resource.data.assign(stored.begin(), stored.end());
        }

        if (const auto name = names.find({resource.type, resource.id}); name != names.end())
        {
            resource.name = name->second;
        }
        resources.push_back(std::move(resource));
    }

    return resources;
}

// An audio group set (AGSC) as MusyX's four files, and the set's name.
struct AudioGroupSet
{
    std::string name;
    FileSet files;
};

// The text from `at` in `agsc` up to its zero byte. Move `at` past the zero byte. Throw FormatError if it doesn't end.
std::string SetString(std::span<const uint8_t> agsc, std::size_t& at)
{
    const std::span<const uint8_t> rest = agsc.subspan(std::min(at, agsc.size()));
    const auto end = std::find(rest.begin(), rest.end(), uint8_t{0});
    if (end == rest.end())
    {
        throw FormatError("an audio group set has no name");
    }

    std::string text(rest.begin(), end);
    at = std::min(at, agsc.size()) + text.size() + 1;

    return text;
}

// An audio group set in either of its layouts. Metroid Prime's set starts with two strings: a folder and the set's
// name. The pool, the project, the samples and the sample directory follow. A word before each gives its size. Metroid
// Prime 2: Echoes' set starts with a word 1, the set's name, and its group's ID in a big-endian half. Four words give
// the sizes of the pool, the project, the sample directory and the samples. The four parts follow in that order. A
// folder's name never starts with a byte 0. A set that starts with the word 1 therefore has the second layout.
AudioGroupSet ReadAudioGroupSet(std::span<const uint8_t> agsc)
{
    constexpr uint32_t kEchoesLayout = 1;
    AudioGroupSet set;
    std::size_t at = 0;
    if (xsflib::LoadBe32(Part(agsc, 0, 4, "an audio group set").data()) == kEchoesLayout)
    {
        at = 4;
        set.name = SetString(agsc, at);
        at += 2;
        const std::span<const uint8_t> sizes = Part(agsc, at, 16, "an audio group set");
        at += 16;
        const std::array<std::vector<uint8_t>*, 4> parts = {&set.files.pool, &set.files.project, &set.files.sdir,
                                                            &set.files.samples};
        for (std::size_t i = 0; i < parts.size(); i++)
        {
            const std::span<const uint8_t> bytes =
                Part(agsc, at, xsflib::LoadBe32(sizes.data() + i * 4), "an audio group set");
            parts[i]->assign(bytes.begin(), bytes.end());
            at += bytes.size();
        }

        return set;
    }

    SetString(agsc, at);
    set.name = SetString(agsc, at);
    const auto part = [&agsc, &at]()
    {
        const uint32_t size = xsflib::LoadBe32(Part(agsc, at, 4, "an audio group set").data());
        const std::span<const uint8_t> bytes = Part(agsc, at + 4, size, "an audio group set");
        at += 4 + size;
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };
    set.files.pool = part();
    set.files.project = part();
    set.files.samples = part();
    set.files.sdir = part();

    return set;
}

// The names in a string table (STLC): the number of strings in a big-endian word, then the strings. A byte 0 ends each
// string. String n names effect n.
std::vector<std::string> StringTable(std::span<const uint8_t> table)
{
    const uint32_t count = xsflib::LoadBe32(Part(table, 0, 4, "a string table").data());
    std::vector<std::string> names;
    std::size_t at = 4;
    for (uint32_t i = 0; i < count; i++)
    {
        names.push_back(SetString(table, at));
    }

    return names;
}

// Metroid Prime (USA, disc version 2) and Metroid Prime 2: Echoes (USA). The game streams most of its music.
// MidiData.pak holds the songs as CSNG resources: a word 2, MusyX's song ID (its MIDI set-up), the song's group, the
// audio group set that holds the group, the song's size and the song. The game plays a song with sndSeqPlayEx, the
// group and the ID. The audio group sets are AGSC resources of the packs. Of two sets with one ID, the first pack's
// counts, in the order of the packs' paths. Metroid Prime 2 keeps Metroid Prime's four songs. None of its packs holds
// the set that they name. They can't play, and are left out.
//
// The effects follow the songs, in the order of their IDs: every effect of each set's effects group. Of two sets that
// give one ID, the set with the lower ID counts. The game plays an effect with sndFXStartEx and the effect's ID once it
// has pushed the group, with a volume and a pan of 0xFF: the effect's row gives both. A pack's string table
// audio_name_lookup names the effects by their IDs (StringTable). Metroid Prime 2's AudioGrp.pak holds one. An effect
// without a name there is named after its set and its ID, such as "Scarab-0721". The game's start-up calls
// sndVolume(127, 0, 0xFF) and leaves the master volumes at 127.
Catalogue GatherRetro(const ntdlib::disc::GameFiles& disc)
{
    // The audio group sets, by ID, from every pack, and the effects' names.
    std::map<uint32_t, AudioGroupSet> sets;
    std::vector<std::string> names;
    std::vector<PakResource> songs;
    for (const ntdlib::disc::GameFile& file : disc.files)
    {
        if (!xsflib::AsciiLower(file.path).ends_with(".pak"))
        {
            continue;
        }

        for (PakResource& resource : ReadPak(ReadDiscFile(disc, file.path), {"AGSC", "STLC", "CSNG"}))
        {
            if (resource.type == "STLC" && resource.name.starts_with("audio_name_lookup"))
            {
                names = StringTable(resource.data);
            }
            else if (resource.type == "AGSC" && !sets.contains(resource.id))
            {
                sets.emplace(resource.id, ReadAudioGroupSet(resource.data));
            }
            else if (resource.type == "CSNG" && xsflib::AsciiLower(file.path) == "mididata.pak")
            {
                songs.push_back(std::move(resource));
            }
        }
    }

    // A set becomes a file set when a sound pushes it.
    Catalogue catalogue;
    std::map<uint32_t, std::size_t> set_numbers;
    const auto set_number = [&](uint32_t id)
    {
        const auto [number, added] = set_numbers.try_emplace(id, catalogue.sets.size());
        if (added)
        {
            catalogue.sets.push_back(sets.at(id).files);
        }

        return number->second;
    };

    for (const PakResource& song : songs)
    {
        constexpr std::size_t kHeader = 20;
        const std::span<const uint8_t> head = Part(song.data, 0, kHeader, "a song");
        const uint32_t set = xsflib::LoadBe32(head.data() + 12);
        if (!sets.contains(set))
        {
            continue;
        }

        const std::span<const uint8_t> data = Part(song.data, kHeader, xsflib::LoadBe32(head.data() + 16), "a song");
        CatalogueSound sound;
        sound.name = song.name.empty() ? xsflib::Hex(song.id, 8) : song.name;
        sound.id = static_cast<uint16_t>(xsflib::LoadBe32(head.data() + 4));
        sound.song_group = static_cast<uint16_t>(xsflib::LoadBe32(head.data() + 8));
        sound.song = catalogue.songs.size();
        sound.groups = {{.id = sound.song_group, .set = set_number(set)}};
        catalogue.songs.emplace_back(data.begin(), data.end());
        catalogue.sounds.push_back(std::move(sound));
    }

    // The effects of every set, by ID. The sets come in the order of their IDs.
    std::vector<SetEffect> effects;
    std::map<std::size_t, uint32_t> set_ids;
    for (const auto& [id, set] : sets)
    {
        if (!set.files.project.empty())
        {
            const std::vector<SetEffect> listed = ProjectEffectsOf(set.files.project, set_ids.size());
            effects.insert(effects.end(), listed.begin(), listed.end());
            set_ids[set_ids.size()] = id;
        }
    }

    KeepFirstOfEachId(effects);

    for (const SetEffect& effect : effects)
    {
        const uint32_t set = set_ids.at(effect.set);
        CatalogueSound sound;
        sound.name = effect.id < names.size() && !names[effect.id].empty() ? names[effect.id]
                                                                           : EffectName(sets.at(set).name, effect.id);
        sound.effect = true;
        sound.id = effect.id;
        sound.groups = {{.id = effect.group, .set = set_number(set)}};
        catalogue.sounds.push_back(std::move(sound));
    }

    return catalogue;
}

// A level that a room's code gives a channel of one of Resident Evil's songs: the MIDI channel, its volume (CC7) and
// its pan (CC10). A pan of -1 leaves the channel's pan as the song sets it.
struct ResidentEvilLevel
{
    uint8_t channel = 0;
    uint8_t volume = 0;
    int pan = -1;
};

// A mix of a song's channels that a room's code sets: the conversion's name, MusyX's song ID and the levels.
struct ResidentEvilMix
{
    const char* name;
    uint16_t song;
    std::vector<ResidentEvilLevel> levels;
};

// The time from a song's start to its mix, in milliseconds. It comes after the controllers of each song's first
// pattern, which end at tick 64 (83 ms at sg7a's 120 beats a minute and 104 ms at sg74's 96), and before its first
// note, at tick 496 (646 ms and 807 ms).
constexpr double kResidentEvilMixMs = 250.0;

// The mixes of the ambience songs that Resident Evil's rooms set (from a reading of the game's code). A room's code
// calls 0x80061E6C with a song, a mask of channels, a volume and a pan. That function finds the slot that plays the
// song and sends the driver's command 55 for each channel in the mask. Command 55 (0x80064524) calls sndSeqCntrl
// (0x80155BB8) on the slot's sequence: CC7 for the volume and CC10 for the pan. A volume or a pan of -1 leaves that
// controller alone.
//
// sg74's first pattern gives channels 0 to 2 a volume of 80. 0x800AFB68 sets all three to 80: the mix of the file,
// which the conversion sg74 plays. The other rooms that give constant levels play one of the three alone at 80 and the
// other two at 0: channel 0 in 0x800DB360, channel 1 in 0x800CC938 and channel 2 in 0x800DAFE8. sg7a's first pattern
// gives its seven channels a volume of 0. 0x801002A0 gives channels 0 to 3 and 5 a volume of 80 and channels 4 and 6 a
// volume of 0, each at a pan of 64 (0x801002F4 to 0x80100330). 0x800FBF0C plays channel 5 alone at 80 (0x800FC1B8
// and 0x800FC1CC). 0x8010D640 plays channel 4 alone at 80 (0x8010D7A8 to 0x8010D820). Both set every pan to 64. Other
// rooms give channel 5 of sg7a other volumes and pans.
const std::vector<ResidentEvilMix>& ResidentEvilMixes()
{
    static const std::vector<ResidentEvilMix> kMixes = {
        {"sg74 ch0", 0x74, {{0, 80}, {1, 0}, {2, 0}}},
        {"sg74 ch1", 0x74, {{0, 0}, {1, 80}, {2, 0}}},
        {"sg74 ch2", 0x74, {{0, 0}, {1, 0}, {2, 80}}},
        {"sg7a ch0-3+5",
         0x7a,
         {{0, 80, 64}, {1, 80, 64}, {2, 80, 64}, {3, 80, 64}, {4, 0, 64}, {5, 80, 64}, {6, 0, 64}}},
        {"sg7a ch4", 0x7a, {{0, 0, 64}, {1, 0, 64}, {2, 0, 64}, {3, 0, 64}, {4, 80, 64}, {5, 0, 64}, {6, 0, 64}}},
        {"sg7a ch5", 0x7a, {{0, 0, 64}, {1, 0, 64}, {2, 0, 64}, {3, 0, 64}, {4, 0, 64}, {5, 80, 64}, {6, 0, 64}}},
    };

    return kMixes;
}

// Resident Evil (USA). The game loads a song from bgm/sgNN.snd, with NN its number in hexadecimal, 00 to 7f
// (0x80061BF4). The file starts with 16 big-endian words: the offsets and the sizes of the pool, the project, the
// sample directory, the game's table of effects, the song and the samples, then the group's ID and the song's ID. Each
// file holds one song and one group. The game pushes the group and starts the song through sndSeqPlayEx, without
// parameters (0x80062364). The songs come in the order of the files' names. A file with the song and samples of an
// earlier file plays the same sound and is left out.
//
// The game's sound driver gives the music's master volume 110 at the options' start (0x800620B4, 0x80066064). Its
// set-up ends with sndVolume(100, 0, 0xFF) (0x800662C4). A room's table names up to four songs that play at once
// (0x80061B1C). The code of a room or an event changes the levels of channels through 0x80061E6C, above all in the
// ambience of songs 0x6E to 0x7C. Each file plays alone at the levels of its song. Song 0x7A starts with all seven
// channels at level 0. The conversion of its file alone stays silent. The mixes that rooms give songs 0x74 and 0x7A
// follow their songs (ResidentEvilMixes). The effects in se/ aren't converted. The game plays each one through a table
// in its file. The table gives the effect's level and sends.
Catalogue GatherResidentEvilSnd(const ntdlib::disc::GameFiles& disc)
{
    constexpr std::size_t kHeader = 0x40;
    // The words of the header that give each part's offset. The word after each gives its size.
    constexpr std::size_t kPool = 0, kProject = 2, kSdir = 4, kSong = 8, kSamples = 10, kGroup = 12, kSongId = 13;
    constexpr std::string_view kFolder = "bgm/", kPrefix = "sg", kSuffix = ".snd";

    const AnyCaseFiles files(disc);
    Catalogue catalogue;
    std::set<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> seen;
    for (const auto& [lower, path] : files.Paths())
    {
        if (!lower.starts_with(kFolder) || !lower.ends_with(kSuffix) ||
            lower.find('/', kFolder.size()) != std::string::npos)
        {
            continue;
        }

        const std::string name = lower.substr(kFolder.size(), lower.size() - kFolder.size() - kSuffix.size());
        if (!name.starts_with(kPrefix))
        {
            continue;
        }

        const std::vector<uint8_t> file = files.Read(path);
        const std::span<const uint8_t> header = Part(file, 0, kHeader, "a sound file's header");
        const auto word = [&](std::size_t index)
        {
            return xsflib::LoadBe32(header.data() + index * 4);
        };
        const auto part = [&](std::size_t index, const char* what)
        {
            const std::span<const uint8_t> data = Part(file, word(index), word(index + 1), what);
            return std::vector<uint8_t>(data.begin(), data.end());
        };

        std::vector<uint8_t> song = part(kSong, "a sound file's song");
        std::vector<uint8_t> samples = part(kSamples, "a sound file's samples");
        if (!seen.emplace(song, samples).second)
        {
            continue;
        }

        const std::size_t set = catalogue.sets.size();
        const auto group = static_cast<uint16_t>(word(kGroup));
        catalogue.sets.push_back({.project = part(kProject, "a sound file's project"),
                                  .pool = part(kPool, "a sound file's pool"),
                                  .sdir = part(kSdir, "a sound file's sample directory"),
                                  .samples = std::move(samples)});

        CatalogueSound sound;
        sound.name = name;
        sound.id = static_cast<uint16_t>(word(kSongId));
        sound.song_group = group;
        sound.song = catalogue.songs.size();
        sound.groups = {{group, set}};
        catalogue.songs.push_back(std::move(song));
        const CatalogueSound file_mix = sound;
        catalogue.sounds.push_back(std::move(sound));
        for (const ResidentEvilMix& mix : ResidentEvilMixes())
        {
            if (mix.song != file_mix.id)
            {
                continue;
            }

            constexpr uint8_t kVolume = 7, kPan = 10;
            CatalogueSound mixed = file_mix;
            mixed.name = mix.name;
            mixed.controls_ms = kResidentEvilMixMs;
            for (const ResidentEvilLevel& level : mix.levels)
            {
                mixed.controls.push_back({.channel = level.channel, .controller = kVolume, .value = level.volume});
                if (level.pan >= 0)
                {
                    mixed.controls.push_back(
                        {.channel = level.channel, .controller = kPan, .value = static_cast<uint8_t>(level.pan)});
                }
            }
            catalogue.sounds.push_back(std::move(mixed));
        }
    }

    return catalogue;
}

// The games with a catalogue that dolphinsf2 converts. Each game's reader above says what its code does.
constexpr std::array<CatalogueRules, 10> kCatalogueGames{{
    {"7e514a94826c672605a4f484367ec511fd27933f", 0x801d6738, 110, 127, 100, GatherResidentEvilSnd},
    {"8bde4669c75260cc058b8b44ef36bacff3a67d20", 0x80330a18, 127, 127, 127, GatherStarFox},
    {"bc62d5e4674d139fd50d6c05694b34f955e37039", 0x803be458, 127, 127, 127, GatherPaperMario},
    {"3523501ee45ea3d84e12096616cb05c1b9733c21", 0x802abb68, 100, 100, 127, GatherPokemonChannel},
    {"bc595e8b8c89ec8307ef61658b6cd5c854670d60", 0x80063320, 127, 127, 127, GatherRogueLeader},
    {"dd1797f6dcdc88abeaecc153b1e76c21e7733cae", 0x80050938, 127, 127, 127, GatherRebelStrike},
    {"5c10973f82a1946ebad3dcfe1ec56e8623d50404", 0x80284c78, 127, 127, 127, GatherResidentEvil},
    {"01a4ad21219e5635cbca144ea5de0c3dd284dc39", 0x802aaa98, 127, 127, 127, GatherTales},
    {"39a2f928159ad46491e9b1ef0a72613d91e0cc40", 0x803f4fe8, 127, 127, 127, GatherRetro},
    {"6ef9b491d0cc08bc81a124fdedb8bfaec34d0010", 0x803c2858, 127, 127, 127, GatherRetro},
}};

// A file set as MusyX reads it: the groups of its project, its pool, a group that holds its samples, and the IDs of its
// stream buffers (ReadSamplesWithoutStreams).
struct ReadSet
{
    std::vector<ntdlib::musyx::Group> groups;
    ntdlib::musyx::Pool pool;
    std::shared_ptr<const SongGroup> holder;
    std::set<uint16_t> streams;
};

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

} // namespace

const CatalogueRules* FindCatalogueGame(std::string_view sha1)
{
    const auto matches = [sha1](const CatalogueRules& r)
    {
        return r.dol_sha1 == sha1;
    };
    const auto rules = std::find_if(kCatalogueGames.begin(), kCatalogueGames.end(), matches);

    return rules != kCatalogueGames.end() ? &*rules : nullptr;
}

CatalogueMusic BuildMusic(Catalogue catalogue, const CatalogueRules& rules)
{
    CatalogueMusic music;

    // Each set is read once, when a sound first pushes one of its groups. Its holder joins the music's groups. The
    // holder holds the set's samples, and SFZ files name the samples by it.
    std::vector<std::optional<ReadSet>> sets(catalogue.sets.size());
    const auto set_of = [&](std::size_t number) -> const ReadSet&
    {
        std::optional<ReadSet>& set = sets.at(number);
        if (!set)
        {
            FileSet& files = catalogue.sets[number];
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
            music.groups.push_back(std::move(holder));
        }

        return *set;
    };

    // The group that plays a sound, by the set and the ID of the group and the groups that the sound pushes. Its pool
    // holds the objects of every pushed set. An object of an earlier set comes first. Its samples lie in the sets'
    // holders, in the same order.
    std::map<std::pair<std::vector<std::pair<std::size_t, uint16_t>>, std::size_t>, std::size_t> playing;
    const auto group_of = [&](const CatalogueSound& sound, std::size_t pushed_index)
    {
        std::vector<std::pair<std::size_t, uint16_t>> key;
        for (const PushedGroup& g : sound.groups)
        {
            key.emplace_back(g.set, g.id);
        }
        const auto found = playing.find({key, pushed_index});
        if (found != playing.end())
        {
            return found->second;
        }

        const PushedGroup& plays = sound.groups[pushed_index];
        const GroupType type = sound.effect ? GroupType::kEffects : GroupType::kSongs;
        auto group = std::make_shared<SongGroup>();
        group->group = *FindGroup(set_of(plays.set).groups, type, plays.id);
        std::vector<std::size_t> order;
        for (const PushedGroup& g : sound.groups)
        {
            if (std::find(order.begin(), order.end(), g.set) == order.end())
            {
                order.push_back(g.set);
            }
        }

        for (const std::size_t number : order)
        {
            const ReadSet& set = set_of(number);
            group->pool.macros.insert(set.pool.macros.begin(), set.pool.macros.end());
            group->pool.tables.insert(set.pool.tables.begin(), set.pool.tables.end());
            group->pool.keymaps.insert(set.pool.keymaps.begin(), set.pool.keymaps.end());
            group->pool.layers.insert(set.pool.layers.begin(), set.pool.layers.end());
            group->pushed.push_back(set.holder);
        }
        music.groups.push_back(std::move(group));
        playing.emplace(std::pair{std::move(key), pushed_index}, music.groups.size() - 1);

        return music.groups.size() - 1;
    };

    // MusyX plays a song through the first pushed group with the song's group ID (sndSeqPlayEx, 0x8001D040 in Rogue
    // Leader's loader). It plays nothing if that group isn't a song group with a MIDI setup for the song. A song group
    // whose samples are all stream buffers plays only a stream. A MIDI file can't hold the stream. Such songs are left
    // out.
    const auto song_sounds = [&](const CatalogueSound& sound)
    {
        for (const PushedGroup& pushed : sound.groups)
        {
            const ReadSet& set = set_of(pushed.set);
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
    };

    // The pushed group that plays each sound: the first that holds the song's group or the effect.
    for (std::size_t s = 0; s < catalogue.sounds.size(); s++)
    {
        const CatalogueSound& sound = catalogue.sounds[s];
        if (!sound.effect && !song_sounds(sound))
        {
            continue;
        }

        std::optional<std::size_t> plays;
        for (std::size_t g = 0; !plays && g < sound.groups.size(); g++)
        {
            const PushedGroup& pushed = sound.groups[g];
            const GroupType type = sound.effect ? GroupType::kEffects : GroupType::kSongs;
            const uint16_t id = sound.effect ? pushed.id : sound.song_group;
            const ntdlib::musyx::Group* group = FindGroup(set_of(pushed.set).groups, type, pushed.id);
            if (pushed.id == id && group != nullptr && (!sound.effect || FindEffect(*group, sound.id) != nullptr))
            {
                plays = g;
            }
        }
        if (!plays)
        {
            throw FormatError("no group that the game pushes plays " + sound.name);
        }

        const std::size_t group = group_of(sound, *plays);
        if (!sound.effect)
        {
            GameSong song;
            song.name = sound.name;
            song.id = sound.id;
            song.volume = sound.volume.value_or(127);
            song.master_volume = rules.music_master_volume;
            song.fade_in_ms = sound.fade_in_ms;
            song.controls = sound.controls;
            song.controls_ms = sound.controls_ms;
            song.group = group;
            music.songs.push_back(std::move(song));
            music.song_data.push_back(catalogue.songs.at(sound.song));
            continue;
        }

        music.effects.push_back({.name = sound.name,
                                 .group = group,
                                 .row = *FindEffect(music.groups[group]->group, sound.id),
                                 .volume = sound.volume,
                                 .pan = sound.pan,
                                 .group_volume = rules.group_volume,
                                 .master_volume = rules.effect_master_volume,
                                 .volume_controller = sound.volume_controller,
                                 .reverb = sound.reverb,
                                 .pitch_wheel = sound.pitch_wheel});
    }

    return music;
}

} // namespace dolphinsf2::musyx
