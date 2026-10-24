// SPDX-License-Identifier: MIT

// The sound manager's file and the game's sound test (see musyx_msm.h).

#include "musyx_msm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

namespace dolphinsf2::musyx
{
namespace
{

using xsflib::FormatError;

// The sound manager's file starts with a header of 24 words. Word 4 gives the offset of the settings. Words 8 and 9
// give the offset and the size of the table of groups. Words 10 and 11 give those of the table of songs. Words 14, 16
// and 18 give the offsets of the groups' data, the groups' samples and the songs that no group holds.
constexpr std::size_t kHeaderSize = 0x60;
constexpr std::size_t kSettings = 0x10;
constexpr std::size_t kGroupTable = 0x20;
constexpr std::size_t kSongTable = 0x28;
constexpr std::size_t kGroupData = 0x38;
constexpr std::size_t kGroupSamples = 0x40;
constexpr std::size_t kLooseSongs = 0x48;

// The settings give the number of base groups in their byte at 0x28. The table's indexes of the base groups follow in
// one byte each. They give the number of effects in a halfword at 6. Word 12 of the header gives the offset of the
// table of effects.
constexpr std::size_t kBaseGroupCount = 0x28;
constexpr std::size_t kBaseGroups = 0x29;
constexpr std::size_t kEffectCount = 6;
constexpr std::size_t kEffectTable = 0x30;

// An entry of the table of effects starts with two halfwords: the MusyX ID of the group that holds the effect and the
// effect's ID. The volume, the pan, the pitch wheel's offset in a signed halfword, the surround pan, the send to aux A
// and the send to aux B follow (msmSePlay at 0x80119a94 in main.dol). An ID of 0xffff stands for no effect.
constexpr std::size_t kEffectEntrySize = 0x10;
constexpr uint16_t kNoEffect = 0xffff;

// An entry of the table of groups gives the group's MusyX ID in its first halfword. The words at 4 and 8 give the
// offset and the size of the group's data in the groups' data. The words at 12 and 16 give those of its samples in the
// groups' samples.
constexpr std::size_t kGroupEntrySize = 0x20;

// An entry of the table of songs starts with two halfwords: the MusyX group that plays the song and the song's ID. The
// words at 4 and 8 give the offset and the size of its SNG file. The byte at 12 gives the table's group whose data
// holds the file. A value below 0 stands for none. The byte at 13 gives the volume.
constexpr std::size_t kSongEntrySize = 0x10;

// A group's data starts with four words. They give the offsets of its pool, its project, its sample directory and its
// songs from the data's start.
constexpr std::size_t kGroupHeadSize = 0x10;
constexpr std::size_t kSongsOffset = 12;

// The sound test's kinds of sound that aren't songs: a stream and a sound effect.
constexpr uint16_t kStreamKind = 1;
constexpr uint16_t kEffectKind = 28;

// An entry of the sound test starts with a word that is 0 for an unused entry. The next word gives the message ID of
// the name. Two halfwords give the kind and the group set that the sound test loads. The last word gives the sound's
// number.
constexpr std::size_t kTestEntrySize = 16;

// The character that the font draws for a byte of a message from 0x30 on. Digits and Latin letters keep their ASCII
// codes. A few punctuation marks have other codes. A byte without a glyph here becomes an underscore.
char Glyph(uint8_t c)
{
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
    {
        return static_cast<char>(c);
    }

    struct Mark
    {
        uint8_t code;
        char mark;
    };
    static constexpr Mark kMarks[] = {{0x3d, '-'}, {0x5c, '\''}, {0x7e, '&'}, {0x82, ','},
                                      {0x85, '.'}, {0xc2, '!'},  {0xc3, '?'}};
    const auto found = std::find_if(std::begin(kMarks), std::end(kMarks), [c](const Mark& m) { return m.code == c; });
    return found != std::end(kMarks) ? found->mark : '_';
}

// The text of the message `id` in the message file `file`. `path` is the file's path on the disc. The ID holds the
// bank in its top 16 bits and the message's number in the bank in its low 16 bits. The file starts with the number of
// its banks and the offset of its table of banks. The table gives each bank's ID and slot in two halfwords. The word
// at 4 + 4 times the slot gives the bank's offset in the file. The slots start at 1. A bank starts with the number of
// its messages. A word for each message then gives its offset from the bank's start. A message ends with a zero byte.
//
// Below 0x30 the bytes are control codes. Four of them become a space: 0x0A (a new line), 0x0C (the next tab stop),
// 0x10 and 0x20 (spaces). 0x0E, 0x1C, 0x1E and 0x1F take the byte after them: a symbol, a voice, a colour, or another
// message to insert. They're left out with that byte. The other control codes are left out too. The text has no
// spaces at either end. Throw FormatError if the file has no such message or the message runs past the file's end.
std::string MessageText(std::span<const uint8_t> file, uint32_t id, const std::string& path)
{
    const std::string where = path + "'s message " + std::to_string(id >> 16) + "-" + std::to_string(id & 0xffff);
    const std::string past_end = where + " runs past the file's end";
    const xsflib::Reader r(file, true, {.bounds_message = xsflib::BoundsMessage::kFixed, .fixed_message = past_end});

    const uint32_t banks = r.U32(0);
    const uint32_t table = r.U32(4);
    std::optional<uint32_t> bank;
    for (uint32_t i = 0; i < banks && !bank; i++)
    {
        const std::size_t entry = table + std::size_t{4} * i;
        if (r.U16(entry) == id >> 16)
        {
            bank = r.U32(4 + std::size_t{4} * r.U16(entry + 2));
        }
    }

    if (!bank || (id & 0xffff) >= r.U32(*bank))
    {
        throw FormatError(where + " is missing");
    }

    std::string text;
    std::size_t at = *bank + std::size_t{r.U32(*bank + 4 + std::size_t{4} * (id & 0xffff))};
    for (uint8_t c = r.U8(at); c != 0; c = r.U8(++at))
    {
        if (c == 0x0a || c == 0x0c || c == 0x10 || c == 0x20)
        {
            text += ' ';
        }
        else if (c == 0x0e || c == 0x1c || c == 0x1e || c == 0x1f)
        {
            at++;
        }
        else if (c >= 0x30)
        {
            text += Glyph(c);
        }
    }

    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        return {};
    }

    return text.substr(first, text.find_last_not_of(' ') + 1 - first);
}

} // namespace

MsmFile::MsmFile(std::span<const uint8_t> file, std::string path)
    : file_(file), path_(std::move(path)), cut_short_(path_ + " is cut short or damaged")
{
    Reader().Sub(0, kHeaderSize);
}

xsflib::Reader MsmFile::Reader() const
{
    return xsflib::Reader(file_, true, {.bounds_message = xsflib::BoundsMessage::kFixed, .fixed_message = cut_short_});
}

MsmSong MsmFile::Song(uint32_t number) const
{
    const xsflib::Reader r = Reader();
    if (number >= r.U32(kSongTable + 4) / kSongEntrySize)
    {
        throw FormatError(path_ + " has no song " + std::to_string(number));
    }

    const std::size_t entry = r.U32(kSongTable) + kSongEntrySize * number;
    MsmSong song;
    song.group_id = r.U16(entry);
    song.song_id = r.U16(entry + 2);
    song.volume = r.U8(entry + 13);

    // The SNG file lies among the songs of the group that holds it, or among the songs that no group holds.
    const auto holder = static_cast<int8_t>(r.U8(entry + 12));
    std::size_t songs = r.U32(kLooseSongs);
    if (holder >= 0)
    {
        const std::size_t data = GroupData(static_cast<std::size_t>(holder));
        songs = data + r.U32(data + kSongsOffset);
    }
    song.data = r.Sub(songs + r.U32(entry + 4), r.U32(entry + 8));

    return song;
}

MsmGroup MsmFile::Group(uint16_t id) const
{
    // The table's first entry holds no group.
    const xsflib::Reader r = Reader();
    for (std::size_t i = 1; i < GroupCount(); i++)
    {
        if (r.U16(r.U32(kGroupTable) + kGroupEntrySize * i) == id)
        {
            return PartsOf(i);
        }
    }

    throw FormatError(path_ + " has no group " + std::to_string(id));
}

std::vector<MsmEffect> MsmFile::Effects() const
{
    const xsflib::Reader r = Reader();
    const std::size_t table = r.U32(kEffectTable);
    const uint32_t count = r.U16(r.U32(kSettings) + kEffectCount);
    std::vector<MsmEffect> effects;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t entry = table + kEffectEntrySize * i;
        if (r.U16(entry + 2) == kNoEffect)
        {
            continue;
        }

        effects.push_back({.number = i,
                           .group_id = r.U16(entry),
                           .id = r.U16(entry + 2),
                           .volume = r.U8(entry + 4),
                           .pan = r.U8(entry + 5),
                           .pitch = static_cast<int16_t>(r.U16(entry + 6)),
                           .reverb = r.U8(entry + 9),
                           .chorus = r.U8(entry + 10)});
    }

    return effects;
}

std::vector<MsmGroup> MsmFile::BaseGroups() const
{
    const xsflib::Reader r = Reader();
    const std::size_t settings = r.U32(kSettings);
    std::vector<MsmGroup> groups;
    for (std::size_t i = 0; i < r.U8(settings + kBaseGroupCount); i++)
    {
        groups.push_back(PartsOf(r.U8(settings + kBaseGroups + i)));
    }

    return groups;
}

std::size_t MsmFile::GroupCount() const
{
    return Reader().U32(kGroupTable + 4) / kGroupEntrySize;
}

MsmGroup MsmFile::PartsOf(std::size_t index) const
{
    const std::size_t at = GroupData(index);
    const xsflib::Reader r = Reader();
    const std::size_t entry = r.U32(kGroupTable) + kGroupEntrySize * index;
    const std::span<const uint8_t> data = r.Sub(at, r.U32(entry + 8));
    const xsflib::Reader head(data, true,
                              {.bounds_message = xsflib::BoundsMessage::kFixed, .fixed_message = cut_short_});
    head.Sub(0, kGroupHeadSize);
    const auto part = [&head, &data](std::size_t word)
    {
        const uint32_t offset = head.U32(word);
        head.Sub(offset, 0);
        return data.subspan(offset);
    };

    return {part(0), part(4), part(8), r.Sub(r.U32(kGroupSamples) + std::size_t{r.U32(entry + 12)}, r.U32(entry + 16))};
}

std::size_t MsmFile::GroupData(std::size_t index) const
{
    if (index >= GroupCount())
    {
        throw FormatError(path_ + " has no group at " + std::to_string(index) + " in its table");
    }

    const xsflib::Reader r = Reader();
    return r.U32(kGroupData) + std::size_t{r.U32(r.U32(kGroupTable) + kGroupEntrySize * index + 4)};
}

std::vector<SoundTestSong> ReadSoundTest(std::span<const uint8_t> module, const std::string& module_path,
                                         uint32_t offset, uint32_t count, std::span<const uint8_t> messages,
                                         const std::string& messages_path)
{
    const std::string past_end = module_path + "'s sound test runs past the file's end";
    const xsflib::Reader r(module, true, {.bounds_message = xsflib::BoundsMessage::kFixed, .fixed_message = past_end});
    std::vector<SoundTestSong> songs;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t entry = offset + kTestEntrySize * i;
        const uint16_t kind = r.U16(entry + 8);
        if (r.U32(entry) != 0 && kind != kStreamKind && kind != kEffectKind)
        {
            songs.push_back({r.U32(entry + 12), MessageText(messages, r.U32(entry + 4), messages_path)});
        }
    }

    return songs;
}

std::map<uint32_t, std::string> ReadSoundTestEffects(std::span<const uint8_t> module, const std::string& module_path,
                                                     uint32_t offset, uint32_t count, std::span<const uint8_t> messages,
                                                     const std::string& messages_path)
{
    const std::string past_end = module_path + "'s list of effects runs past the file's end";
    const xsflib::Reader r(module, true, {.bounds_message = xsflib::BoundsMessage::kFixed, .fixed_message = past_end});
    std::map<uint32_t, std::string> names;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t entry = offset + kTestEntrySize * i;
        if (r.U32(entry) != 0 && r.U16(entry + 8) == kEffectKind)
        {
            names[r.U32(entry + 12)] = MessageText(messages, r.U32(entry + 4), messages_path);
        }
    }

    return names;
}

} // namespace dolphinsf2::musyx
