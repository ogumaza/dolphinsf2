// SPDX-License-Identifier: MIT

// Unit tests for dolphinsf2, without game data. The tests build small MusyX song groups, and pieces of the Nintendo
// 64's sound library and of Square's Red driver, in memory. ntdlib's tests cover the conversion of JAudio's sound data,
// the file of Hudson's sound manager, Yay0 data and modules of code.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
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
#include "seqlib/sf2/units.h"
#include "seqlib/sfz/track_file.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "fade_fit.h"
#include "input.h"
#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"
#include "musyx_player.h"
#include "musyx_voices.h"
#include "n64_convert.h"
#include "n64_player.h"
#include "n64_sound.h"
#include "output_units.h"
#include "red_music.h"

namespace
{

using namespace dolphinsf2;
using ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;
using xsflib::testing::failures;

// The events of `kind` on MIDI track `slot` of `perf`, in order. None if `perf` has no such track.
std::vector<TrackEvent> EventsOf(const Performance& perf, std::size_t slot, EventKind kind)
{
    std::vector<TrackEvent> events;
    if (slot >= perf.tracks.size())
    {
        return events;
    }

    for (const TrackEvent& e : perf.tracks[slot])
    {
        if (e.kind == kind)
        {
            events.push_back(e);
        }
    }

    return events;
}

// The number of events of `kind` on MIDI track `slot` of `perf`.
int Count(const Performance& perf, std::size_t slot, EventKind kind)
{
    return static_cast<int>(EventsOf(perf, slot, kind).size());
}

// The values of the events of `kind` on MIDI track `slot` of `perf`, in order: CC values, or a note's velocity.
std::vector<int> ValuesOf(const Performance& perf, std::size_t slot, EventKind kind)
{
    std::vector<int> values;
    for (const TrackEvent& e : EventsOf(perf, slot, kind))
    {
        values.push_back(e.value);
    }

    return values;
}

// The tick and the key of each event of `kind` on MIDI track `slot` of `perf`, in order.
std::vector<std::pair<uint32_t, int>> KeysOf(const Performance& perf, std::size_t slot, EventKind kind)
{
    std::vector<std::pair<uint32_t, int>> keys;
    for (const TrackEvent& e : EventsOf(perf, slot, kind))
    {
        keys.emplace_back(e.tick, e.key);
    }

    return keys;
}

// The bends of channel 0 of `perf` other than 0, in semitones, in order.
std::vector<float> BendsOf(const Performance& perf)
{
    std::vector<float> bends;
    for (const TrackEvent& e : EventsOf(perf, 0, EventKind::kPitch))
    {
        if (e.semitones != 0.0f)
        {
            bends.push_back(e.semitones);
        }
    }

    return bends;
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
    Check(past_the_data == "main.dol has no data at the song list",
          "a list past the DOL's data fails: " + past_the_data);
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
    Check(past_the_data == "main.dol has no data at the sound room",
          "a sound room past the DOL's data fails: " + past_the_data);
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
    if (!Check(shifted.piano_pan && shifted.piano_pan->scale == 100 && shifted.piano_pan->centre_key == 60 &&
                   shifted.piano_pan->centre_pan == 54,
               "PianoPan takes its scale, centre key and centre pan from bytes 1 to 3"))
    {
        return;
    }

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

// A WaitMs step of `ms` milliseconds.
ntdlib::musyx::MacroStep WaitMsStep(uint32_t ms)
{
    return Step(ntdlib::musyx::MacroOp::kWaitMs, {0, 0, 0, 0, 0, ms & 0xff, ms >> 8});
}

// A Loop step back to step `step`, `count` times, with the flags of byte 1.
ntdlib::musyx::MacroStep LoopStep(uint32_t flags, uint32_t step, uint32_t count)
{
    return Step(ntdlib::musyx::MacroOp::kLoop, {flags, 0, 0, step & 0xff, step >> 8, count & 0xff, count >> 8});
}

// Loop goes back to step `step` `count` times. The count starts again once it runs out. A count of 0xffff with bit 0
// of byte 1 goes round until the release. A loop that doesn't wait then waits for the release, and a trap of the
// release takes the voice on from there. MusyX follows 32 steps of a voice in each millisecond.
void TestMacroLoop()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kStartSample, {1}), WaitMsStep(100), LoopStep(0, 1, 2), Step(MacroOp::kStopSample),
                      Step(MacroOp::kEnd)};
    pool.macros[2] = {
        Step(MacroOp::kStartSample, {1}), WaitMsStep(10),     LoopStep(0, 1, 1), WaitMsStep(20), LoopStep(0, 3, 1),
        Step(MacroOp::kStopSample),       Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kStartSample, {1}), LoopStep(0, 1, 70), Step(MacroOp::kStopSample),
                      Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kScaleVolume, {127, 0, 0xff, 0xff, 1}),
                      LoopStep(1, 1, 0xffff), Step(MacroOp::kStopSample), Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kTrapEvent, {0, 5, 0, 4, 0}), Step(MacroOp::kStartSample, {1}),
                      LoopStep(0, 2, 0xffff), Step(MacroOp::kEnd)};
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    pool.macros[5].insert(pool.macros[5].end(), kFade.begin() + 1, kFade.end());

    const musyx::MacroVoice counted = musyx::FollowMacro(pool, 1, 127);
    Check(counted.stop_ms == 300.0 && counted.approximations.empty(),
          "a count of 2 goes back twice, and the loop reports nothing");
    Check(musyx::FollowMacro(pool, 2, 127).stop_ms == 60.0, "a second loop starts the count again");
    Check(musyx::FollowMacro(pool, 3, 127).stop_ms == 2.0,
          "73 steps without a wait take two milliseconds: the stop comes in the third run");

    const musyx::MacroVoice held = musyx::FollowMacro(pool, 4, 127);
    Check(!held.stop_ms && held.key_off == musyx::KeyOff::kCut && held.approximations.empty(),
          "a loop until the release that doesn't wait holds the voice, and the stop after it cuts the voice there");
    const musyx::MacroVoice trapped = musyx::FollowMacro(pool, 5, 127);
    Check(!trapped.stop_ms && trapped.key_off == musyx::KeyOff::kFade && trapped.release_fade.stop_ms == 920.0,
          "a trap of the release takes a loop that doesn't wait on to the fade");
}

// FadeIn at the voice's start becomes the attack, and the volume takes the fade's end: byte 1 scales it in 128ths and
// byte 2 adds 127ths of full volume. The time is in milliseconds with bit 0 of byte 5, else in ticks. A FadeIn after a
// wait ramps from the voice's volume, and the report says so. A table of the pool named in bytes 3 and 4 maps the
// fade's end as a curve.
void TestFadeIn()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kFadeIn, {64, 10, 0xff, 0xff, 1, 0x20, 0x03}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kStartSample, {1}), WaitMsStep(100),
                      Step(MacroOp::kFadeIn, {127, 0, 0xff, 0xff, 1, 50}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kFadeIn, {127, 0, 0xff, 0xff, 0, 10}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};

    const musyx::MacroVoice faded = musyx::FollowMacro(pool, 1, 127);
    Check(faded.fade_in_ms == 800.0 && faded.volume == 0.5 && std::abs(faded.volume_add - 10.0 / 127.0) < 1e-12 &&
              faded.held_ramps.empty() && faded.approximations.empty(),
          "a fade at the start rises for 800 ms to half the volume plus 10/127, and reports nothing");
    const musyx::MacroVoice later = musyx::FollowMacro(pool, 2, 127);
    Check(later.fade_in_ms == 0.0 && later.held_ramps.size() == 1 && later.held_ramps[0].start_ms == 100.0 &&
              later.held_ramps[0].ms == 50.0 && later.held_ramps[0].scale == 127.0 / 128.0,
          "a later fade ramps from the volume that the voice has");
    Check(later.approximations == std::vector<std::string>{"fade-ins after a voice's start ramp from its volume"},
          "a later fade is reported");
    Check(std::abs(musyx::FollowMacro(pool, 3, 127).fade_in_ms - 10.0 * 1000.0 / 768.0) < 1e-9,
          "a time in ticks takes a tempo of 120");

    // Pokémon Colosseum's snd_se-0902 fades in over 2 s to 2/128 of its volume through table 0x20, an envelope. At full
    // volume the fade ends at 1.984 of 127. The curve takes byte 1 (3) and 0.984 of the way to byte 2 (232).
    pool.tables[0x20] = {0xe8, 0x03, 0xe8, 0x03, 0x00, 0x10, 0xe8, 0x03};
    pool.tables[0x21] = {0x00, 0x40};
    pool.macros[4] = {Step(MacroOp::kFadeIn, {2, 0, 0x20, 0x00, 1, 0xd0, 0x07}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kFadeIn, {64, 0, 0x21, 0x00, 1, 10}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    const musyx::MacroVoice curved = musyx::FollowMacro(pool, 4, 127);
    Check(curved.fade_in_ms == 2000.0 && std::abs(curved.volume - (3.0 + 0.984375 * 229.0) / 127.0) < 1e-12 &&
              curved.volume_add == 0.0 && curved.approximations.empty(),
          "a curve that the pool holds maps the fade's end at full volume, and reports nothing");
    const musyx::MacroVoice short_curve = musyx::FollowMacro(pool, 5, 127);
    Check(short_curve.volume == 0.5 && short_curve.approximations == std::vector<std::string>{"volume curves left out"},
          "a curve that ends before the bytes that MusyX reads is left out and reported");
}

// SetVariable, AddImmediate and the other arithmetic steps work on 16 signed bits, and IfEqual and IfLess go on at
// step `Half(6)` when they hold. Byte 5 turns a comparison round. Variables 16 to 31 are MusyX's globals. Each run
// starts them at 0. The game's start leaves them there. A comparison with a controller goes on with the next step and
// is reported. Each macro ends at a ScaleVolume of 100 when it goes on, or of 64 when it jumps.
void TestMacroVariables()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;

    const auto ending = [](std::vector<ntdlib::musyx::MacroStep> steps)
    {
        steps.push_back(Step(MacroOp::kScaleVolume, {100, 0, 0xff, 0xff, 1}));
        steps.push_back(Step(MacroOp::kEnd));
        steps.push_back(Step(MacroOp::kScaleVolume, {64, 0, 0xff, 0xff, 1}));
        steps.push_back(Step(MacroOp::kEnd));

        return steps;
    };

    // The first play of Mario Party 4's se-0768: global 16 counts the plays, and global 17 holds 1.
    pool.macros[1] =
        ending({Step(MacroOp::kSetVariable, {0, 0x11, 0, 1, 0}), Step(MacroOp::kAddImmediate, {0, 0x10, 0, 0x10, 1, 0}),
                Step(MacroOp::kIfEqual, {0, 0x10, 0, 0x11, 0, 5, 0})});
    pool.macros[2] =
        ending({Step(MacroOp::kSetVariable, {0, 0x11, 0, 1, 0}), Step(MacroOp::kAddImmediate, {0, 0x10, 0, 0x10, 1, 0}),
                Step(MacroOp::kIfEqual, {0, 0x10, 0, 0x11, 1, 5, 0})});
    pool.macros[3] =
        ending({Step(MacroOp::kSetVariable, {0, 0, 0, 0xfb, 0xff}), Step(MacroOp::kIfLess, {0, 0, 0, 1, 0, 4, 0})});
    pool.macros[4] =
        ending({Step(MacroOp::kSetVariable, {0, 0, 0, 0xff, 0x7f}), Step(MacroOp::kAddImmediate, {0, 0, 0, 0, 1, 0}),
                Step(MacroOp::kDivideVariables, {0, 2, 0, 0, 0, 3}), Step(MacroOp::kIfEqual, {0, 2, 0, 3, 1, 6, 0}),
                Step(MacroOp::kSetVariable, {0, 1, 0, 0xff, 0x7f}), Step(MacroOp::kIfEqual, {0, 0, 0, 1, 0, 8, 0})});
    pool.macros[5] = ending({Step(MacroOp::kIfEqual, {1, 7, 0, 0, 0, 3, 0})});

    const musyx::MacroVoice first = musyx::FollowMacro(pool, 1, 127);
    Check(first.volume == 64.0 / 127.0 && first.approximations.empty(),
          "a global starts at 0: the first play adds 1, equals 1 and jumps, and reports nothing");
    Check(musyx::FollowMacro(pool, 1, 127).volume == 64.0 / 127.0, "each run starts the globals again");
    Check(musyx::FollowMacro(pool, 2, 127).volume == 100.0 / 127.0, "byte 5 turns the comparison round");
    Check(musyx::FollowMacro(pool, 3, 127).volume == 64.0 / 127.0, "IfLess compares 16 signed bits: -5 < 0");
    Check(musyx::FollowMacro(pool, 4, 127).volume == 64.0 / 127.0,
          "a sum past 0x7fff stays there, and a division by 0 gives 0");
    const musyx::MacroVoice controller = musyx::FollowMacro(pool, 5, 127);
    Check(controller.volume == 100.0 / 127.0 &&
              controller.approximations == std::vector<std::string>{musyx::kCommandsLeftOut},
          "a comparison with a controller goes on and is reported");
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
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    pool.macros[1].insert(pool.macros[1].end(), kFade.begin(), kFade.end());
    pool.macros[2] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0})};
    pool.macros[2].insert(pool.macros[2].end(), kFade.begin() + 1, kFade.end());

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

// A held note whose linear envelope decays to silence ends where the decay does. Macros 1 and 2 set linear envelopes
// with an attack of 20 ms, a decay of 300 ms and a sustain of 0, start sample 1 and release the envelope at the note's
// release. Envelope 3 releases over 100 ms. Envelope 4 releases over 1,800 ms, more slowly than its decay falls. Its
// note ends where the files' decay has fallen silent.
void TestDecayToSilence()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;

    const auto releasing = [](uint32_t table)
    {
        return std::vector<ntdlib::musyx::MacroStep>{
            Step(MacroOp::kSetAdsr, {table, 0, 0}),
            Step(MacroOp::kStartSample, {1}),
            Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
            Step(MacroOp::kKeyOff),
            Step(MacroOp::kEnd),
        };
    };

    pool.macros[1] = releasing(3);
    pool.macros[2] = releasing(4);
    pool.tables[3] = {20, 0, 0x2c, 0x01, 0, 0, 100, 0};
    pool.tables[4] = {20, 0, 0x2c, 0x01, 0, 0, 0x08, 0x07};

    const musyx::MacroVoice fast = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice slow = musyx::FollowMacro(pool, 2, 127);

    Check(fast.key_off == musyx::KeyOff::kRelease && slow.key_off == musyx::KeyOff::kRelease,
          "both voices release their envelopes");
    Check(musyx::DecayedMs(fast) == 320.0 && musyx::HeldSilenceMs(fast) == 320.0,
          "a release faster than the decay: the note ends with the decay, after the attack");
    const double kSilent = 20.0 + 300.0 * kSoundFontFallDb / musyx::kEnergyFitDb;
    Check(musyx::DecayedMs(slow) == kSilent && musyx::HeldSilenceMs(slow) == kSilent,
          "a release slower than the decay: the note ends where the files' decay has fallen silent, at " +
              std::to_string(musyx::DecayedMs(slow).value_or(0.0)) + " ms");
}

// A song group with one program. Program 0 plays macro 1. The macro picks the DLS volume curve without ITD, sets DLS
// envelope 2, starts sample 1, waits for the note's release and releases the envelope. Envelope 2 starts at full level
// at once, holds it and releases over 100 ms. Sample 1 is a PCM16 sine of 3200 samples at 32000 Hz, at key 60, that
// loops from sample 800 to its end. Song 5's MIDI setup gives channel 0 a volume of 100.
musyx::SongGroup MakeSongGroup()
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
musyx::VolumeTable SampleVolumes()
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
ntdlib::musyx::Song OneTrackSong(bool loops, bool volume_change)
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

// The game's song table gives each song a volume. MusyX multiplies the channels' volumes by it inside the volume curve.
// On the DLS curve, each CC7 then takes the song's volume in 127ths: the MIDI setup's 100, the track entry's 100 and
// the pattern's 64. A game's master volume multiplies them too.
void TestMusyxSongVolume()
{
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    const ntdlib::musyx::Song song = OneTrackSong(false, true);

    const Performance full = musyx::PlaySong(song, {"full", "G/A/A", 5, 127}, instruments, {});
    const Performance quieter = musyx::PlaySong(song, {"quieter", "G/A/A", 5, 90}, instruments, {});
    const Performance lower = musyx::PlaySong(song, {"lower", "G/A/A", 5, 127, 101}, instruments, {});
    const Performance both = musyx::PlaySong(song, {"both", "G/A/A", 5, 90, 101}, instruments, {});

    Check(ValuesOf(full, 0, EventKind::kVolume) == std::vector<int>{100, 64},
          "a song at 127 keeps its channels' volumes");
    Check(ValuesOf(quieter, 0, EventKind::kVolume) == std::vector<int>{71, 45},
          "a song at 90 scales them by 90/127, rounded");
    Check(ValuesOf(lower, 0, EventKind::kVolume) == std::vector<int>{80, 51},
          "a master volume of 101 scales them by 101/127");
    Check(ValuesOf(both, 0, EventKind::kVolume) == std::vector<int>{56, 36},
          "the song's volume and the master volume multiply");
}

// A game can start a song with a fade-in, through the volume of sndSeqPlayEx or sndSeqVolume. MusyX's fader starts at
// 0 and moves in a line, a step every 5 ms. Over 20 ms, each CC7 of channel 0 then takes 0, 1/4, 1/2 and 3/4 of the
// channel's volume, then all of it. The notes keep their velocities.
void TestMusyxSongFadeIn()
{
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    const ntdlib::musyx::Song song = OneTrackSong(false, true);
    musyx::GameSong faded{"faded", "G/A/A", 5, 127};
    faded.fade_in_ms = 20.0;

    const Performance plain = musyx::PlaySong(song, {"plain", "G/A/A", 5, 127}, instruments, {});
    const Performance fading = musyx::PlaySong(song, faded, instruments, {});

    const std::vector<int> volumes = ValuesOf(fading, 0, EventKind::kVolume);
    std::string listed;
    for (const int v : volumes)
    {
        listed += " " + std::to_string(v);
    }
    Check(volumes == std::vector<int>{0, 25, 50, 75, 100, 64},
          "the song's CC7 rises from 0 to 100 in steps of 5 ms, then the pattern sets 64:" + listed);
    Check(ValuesOf(fading, 0, EventKind::kNoteOn) == ValuesOf(plain, 0, EventKind::kNoteOn),
          "the fade-in leaves the notes' velocities alone");
}

// The song group of the tests of tail cuts: MakeSongGroup with a macro that sets linear envelope 3 instead. The
// envelope has no attack or decay, a full sustain and a release of 200 ms.
musyx::SongGroup TailCutGroup()
{
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[1][1] = Step(ntdlib::musyx::MacroOp::kSetAdsr, {3, 0, 0});
    group.pool.tables[3] = {0, 0, 0, 0, 0x00, 0x10, 200, 0};

    return group;
}

// A SoundFont plays a release to its end. The files fit MusyX's linear release by its energy. Their release therefore
// sounds on after the release of the game has ended. Once the game's voices of a channel have fallen silent, CC11 takes
// the tail of the files down along the game's line, and CC120 stops it. CC11 then goes back to 127. The note of
// OneTrackSong ends after 192 of the song's 768 ticks a second. The game's voice falls silent 200 ms later, about 154
// ticks after the note-off.
void TestMusyxTailCut()
{
    constexpr double kReleaseTicks = 200.0 * 768.0 / 1000.0;
    const musyx::SongGroup group = TailCutGroup();
    musyx::Instruments instruments(group, SampleVolumes());

    const Performance perf = musyx::PlaySong(OneTrackSong(false, false), {"tail", "G/A/A", 5, 127}, instruments, {});

    const std::vector<TrackEvent> offs = EventsOf(perf, 0, EventKind::kNoteOff);
    const std::vector<TrackEvent> cuts = EventsOf(perf, 0, EventKind::kSoundOff);
    if (!Check(offs.size() == 1 && cuts.size() == 1, "one note and one cut"))
    {
        return;
    }

    const double after = static_cast<double>(cuts[0].tick) - offs[0].tick;
    Check(std::abs(after - kReleaseTicks) <= 2.0,
          "the cut comes where the game's release ends, " + std::to_string(after) + " ticks after the note-off");

    // After the note-off, CC11 falls from about 58% of the release on, a step every 5 ms, to 0 at the cut. It is 127
    // after the cut.
    std::vector<int> before;
    bool restored = false;
    uint32_t first = 0;
    for (const TrackEvent& e : EventsOf(perf, 0, EventKind::kExpression))
    {
        if (e.tick <= offs[0].tick)
        {
            continue;
        }
        if (e.tick < cuts[0].tick || (e.tick == cuts[0].tick && e.value == 0))
        {
            if (before.empty())
            {
                first = e.tick;
            }
            before.push_back(e.value);
        }
        else
        {
            restored = e.tick == cuts[0].tick && e.value == 127;
        }
    }
    Check(before.size() >= 10 && std::is_sorted(before.rbegin(), before.rend()) && before.back() == 0 &&
              before.front() < 127,
          "CC11 falls step by step to 0 at the cut, in " + std::to_string(before.size()) + " steps");
    const double share = (static_cast<double>(first) - offs[0].tick) / kReleaseTicks;
    Check(share > 0.55 && share < 0.65, "the fall starts at " + std::to_string(share) + " of the release");
    Check(restored, "CC11 goes back to 127 after the cut");
}

// A note that starts before the game's release ends leaves the tail alone. The second note's tail is cut.
void TestMusyxTailUnderNextNote()
{
    const musyx::SongGroup group = TailCutGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song two = OneTrackSong(false, false);
    two.patterns[0].events.push_back(
        {.tick = 230, .kind = ntdlib::musyx::PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 192});

    const Performance overlapped = musyx::PlaySong(two, {"two", "G/A/A", 5, 127}, instruments, {});

    const std::vector<TrackEvent> cut_once = EventsOf(overlapped, 0, EventKind::kSoundOff);
    const std::vector<TrackEvent> two_offs = EventsOf(overlapped, 0, EventKind::kNoteOff);
    Check(cut_once.size() == 1 && two_offs.size() == 2 && cut_once[0].tick > two_offs[1].tick,
          "a tail under the next note sounds on, and the last note's tail is cut");
}

// A song that loops cuts the tail in each pass. A DLS release isn't cut: the files' release ends with the game's.
void TestMusyxLoopTailCut()
{
    const musyx::SongGroup group = TailCutGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    const musyx::SongGroup dls_group = MakeSongGroup();
    musyx::Instruments dls_instruments(dls_group, SampleVolumes());

    const Performance looped = musyx::PlaySong(OneTrackSong(true, false), {"loop", "G/A/A", 5, 127}, instruments, {});
    const Performance dls = musyx::PlaySong(OneTrackSong(false, false), {"dls", "G/A/A", 5, 127}, dls_instruments, {});

    Check(looped.loop && EventsOf(looped, 0, EventKind::kSoundOff).size() == 1, "the loop's tail is cut");
    Check(EventsOf(dls, 0, EventKind::kSoundOff).empty(), "a DLS release isn't cut");
}

// A voice that fades after its note's release and plays its linear release at a later KeyOff falls silent at the
// end of that release, 250 + 200 ms after the note-off. The files' release sounds on, and the cut ends it there.
void TestMusyxFadeTailCut()
{
    using ntdlib::musyx::MacroOp;
    constexpr double kFadeTicks = (250.0 + 200.0) * 768.0 / 1000.0;
    musyx::SongGroup fading = TailCutGroup();
    fading.pool.macros[1] = {Step(MacroOp::kModeSelect, {1, 1}),
                             Step(MacroOp::kSetAdsr, {3, 0, 0}),
                             Step(MacroOp::kStartSample, {1}),
                             Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
                             Step(MacroOp::kEnvelope, {96, 0, 0xff, 0xff, 1, 0xfa, 0}),
                             Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xfa, 0}),
                             Step(MacroOp::kKeyOff),
                             Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xe8, 0x03}),
                             Step(MacroOp::kEnd)};
    musyx::Instruments fading_instruments(fading, SampleVolumes());

    const Performance faded =
        musyx::PlaySong(OneTrackSong(false, false), {"fade", "G/A/A", 5, 127}, fading_instruments, {});

    const std::vector<TrackEvent> fade_offs = EventsOf(faded, 0, EventKind::kNoteOff);
    const std::vector<TrackEvent> fade_cuts = EventsOf(faded, 0, EventKind::kSoundOff);
    const double fade_after = fade_offs.size() == 1 && fade_cuts.size() == 1
                                  ? static_cast<double>(fade_cuts[0].tick) - fade_offs[0].tick
                                  : 0.0;
    Check(std::abs(fade_after - kFadeTicks) <= 3.0,
          "a fade that ends in a linear release is cut where the release ends, " + std::to_string(fade_after) +
              " ticks after the note-off");
}

// The velocity whose square times the squares of CC7 `volume` and CC11 127 gives the table's level of `velocity` times
// the volume that CC7 stands for, on the table or on the DLS curve.
double TableVelocity(int velocity, int volume, bool on_table)
{
    const musyx::VolumeTable table = SampleVolumes();
    const double written = volume / 127.0;
    const double channel = on_table ? musyx::TableVolume(table, written * written) : written;
    return 127.0 * std::sqrt(musyx::TableLevel(table, velocity / 127.0 * channel)) / written;
}

// The decibels by which velocity `velocity` and CC7 `volume` miss the table's level of `product`.
double TableMiss(int velocity, int volume, double product)
{
    const double level = velocity / 127.0 * volume / 127.0;
    return 20.0 * std::log10(level * level / musyx::TableLevel(SampleVolumes(), product));
}

// MusyX's volume table gives a voice's level at a volume in 127ths. MusyX interpolates between the entries.
void TestMusyxVolumeTable()
{
    const musyx::VolumeTable table = SampleVolumes();
    musyx::SongGroup group = MakeSongGroup();
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
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    group.pool.macros[2][0] = Step(ntdlib::musyx::MacroOp::kModeSelect, {0, 1});
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::SongGroup table_group = group;
    table_group.group.pages[0].object = 2;
    musyx::Instruments table_instruments(table_group, SampleVolumes());
    musyx::Instruments mixed_instruments(group, SampleVolumes());
    const ntdlib::musyx::Song song = OneTrackSong(false, true);
    ntdlib::musyx::Song mixed_song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = mixed_song.patterns[0].events;
    events.push_back({.tick = 240, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 288, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});

    const Performance table = musyx::PlaySong(song, {"table", "G/A/A", 5, 127, 100}, table_instruments, {});
    const Performance mixed = musyx::PlaySong(mixed_song, {"mixed", "G/A/A", 5, 127, 100}, mixed_instruments, {});

    constexpr double kChannel = 100.0 / 127.0 * 100.0 / 127.0;

    const auto written = [](double volume)
    {
        return static_cast<int>(std::lround(127.0 * std::sqrt(musyx::TableLevel(SampleVolumes(), volume))));
    };

    const std::vector<int> volumes = ValuesOf(table, 0, EventKind::kVolume);
    Check(volumes == std::vector<int>{written(kChannel), written(64.0 / 127.0 * 100.0 / 127.0)},
          "CC7 takes the square root of the table's level of the channel's volume times the master volume");
    const std::vector<int> velocities = ValuesOf(table, 0, EventKind::kNoteOn);
    Check(velocities.size() == 1 && !volumes.empty() &&
              std::abs(velocities[0] - TableVelocity(100, volumes[0], true)) <= 0.5,
          "the velocity gives the note the table's level of the velocity times the volume that CC7 stands for");
    Check(velocities.size() == 1 && !volumes.empty() &&
              std::abs(TableMiss(velocities[0], volumes[0], 100.0 / 127.0 * kChannel)) < 0.15,
          "the note plays within 0.15 dB of the table's level of the whole product");
    Check(table.approximations.empty(), "a channel on the table reports no approximation");

    const std::vector<int> mixed_volumes = ValuesOf(mixed, 0, EventKind::kVolume);
    const std::vector<int> mixed_velocities = ValuesOf(mixed, 0, EventKind::kNoteOn);
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
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(true, false), {"loop", "G/A/A", 5, 127}, instruments, {});

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
    musyx::SongGroup startup = MakeSongGroup();
    startup.pool.macros[2] = {Step(MacroOp::kStartSample, {9}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff})};
    startup.samples[0].id = 9;
    musyx::SongGroup bank = MakeSongGroup();
    bank.pool.macros[1] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kEnd)};
    const auto pushed = std::make_shared<const musyx::SongGroup>(startup);

    musyx::AddPushedGroups(bank, {pushed});
    musyx::Instruments instruments(bank, SampleVolumes(), {{9, "s0_9"}});
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
    const musyx::SongGroup first = MakeSongGroup();
    musyx::SongGroup other = MakeSongGroup();
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
    if (!Check(names.size() == 4, "each group has its names"))
    {
        return;
    }

    musyx::Instruments named(*groups[1], SampleVolumes(), names[1]);
    musyx::Instruments plain(*groups[1], SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(true, false), {"loop", "G/A/A", 5, 127}, named, {});
    const Conversion unnamed = musyx::ConvertSong(OneTrackSong(true, false), {"loop", "G/A/A", 5, 127}, plain, {});

    const auto name_of = [&names](std::size_t group)
    {
        const auto found = names[group].find(1);
        return found != names[group].end() ? found->second : std::string("none");
    };

    Check(name_of(0) == "s1" && name_of(1) == "s1_1" && name_of(2) == "s1_1" && name_of(3) == "s1",
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
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    musyx::GameSong listed{"early", "G/A/A", 5, 127};
    listed.tick_step = musyx::TickStep::kInteger;

    const Performance perf = musyx::PlaySong(OneTrackSong(true, false), listed, instruments, {});

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
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[2];
    macro.insert(macro.begin() + 1, Step(MacroOp::kScaleVolume, {scale, 0, 0, 0, 1}));
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 240, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 288, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});

    return musyx::ConvertSong(song, {name, "G/A/A", 5, 127}, instruments, {});
}

// Whether a zone with generators `gens` plays key 60, the key of the test songs' notes. The test sample plays at the
// DSP's highest pitch from key 84 up. Those keys get zones of their own (TestHighestPitch).
bool PlaysKey60(const std::map<uint16_t, uint16_t>& gens)
{
    return !gens.contains(seqlib::sf2::kKeyRange) ||
           ((gens.at(seqlib::sf2::kKeyRange) & 0xff) <= 60 && (gens.at(seqlib::sf2::kKeyRange) >> 8) >= 60);
}

// The attenuation of the zones that play a sample at key 60 in a SoundFont: the number of such zones, the values of
// their initialAttenuation generators, and their modulators to initialAttenuation.
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
        if (!gens.contains(seqlib::sf2::kSampleId) || !PlaysKey60(gens))
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

// The values of `opcode`, such as "volume", in the SFZ files of `c`, in the order of the files and their regions. The
// regions from key 84 up play the test sample at the DSP's highest pitch. Every region whose lowest key is above 60 is
// left out.
std::vector<std::string> OpcodeValues(const Conversion& c, const std::string& opcode)
{
    const std::string key = " " + opcode + "=";
    std::vector<std::string> values;
    for (const seqlib::sfz::File& f : c.sfz.files)
    {
        for (std::size_t at = f.text.find(key); at != std::string::npos; at = f.text.find(key, at + 1))
        {
            const std::size_t region = f.text.rfind("<region>", at);
            const std::size_t lokey = f.text.find(" lokey=", region);
            if (region != std::string::npos && lokey != std::string::npos && lokey < at &&
                std::stoi(f.text.substr(lokey + 7)) > 60)
            {
                continue;
            }

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
    const musyx::VolumeTable table = SampleVolumes();
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
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[2];
    macro[0] = Step(MacroOp::kModeSelect, {0, 1});
    macro.insert(macro.begin() + 1, Step(MacroOp::kScaleVolume, {80, 20, 0xff, 0xff, 1}));
    group.pool.layers[0x8000] = {{.object = 2, .volume = 0}};
    group.group.pages.push_back({.object = 0x8000, .priority = 0, .max_voices = 8, .program = 1});
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 2});
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 200, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 240, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});
    events.push_back({.tick = 400, .kind = PatternEventKind::kProgram, .value = 2});
    events.push_back({.tick = 440, .kind = PatternEventKind::kNote, .key = 64, .velocity = 64, .length = 96});

    const Conversion c = musyx::ConvertSong(song, {"added", "G/A/A", 5, 127}, instruments, {});

    const SampleZoneAttenuation a = AttenuationOf(c.sf2);

    const auto generator = [](double volume)
    {
        return static_cast<int>(std::lround(-500.0 * std::log10(musyx::TableLevel(SampleVolumes(), volume))));
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
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(false, false), {"release", "G/A/A", 5, 127}, instruments, {});

    const std::vector<std::string> releases = OpcodeValues(c, "ampeg_release");
    Check(releases.size() == 1 && std::abs(std::stod(releases[0]) - kSeconds) < 1e-6,
          "the release lasts " + std::to_string(kSeconds) + " s: " + (releases.empty() ? "none" : releases[0]));
}

// The note-off ticks of channel 0 of the song that plays the voice of macro `macro` with one note of `length` ticks.
// The song's clock runs 384 ticks to 500 ms.
std::vector<uint32_t> NoteEnds(const musyx::SongGroup& group, uint16_t macro, uint16_t length)
{
    musyx::SongGroup played = group;
    played.pool.macros[1] = played.pool.macros[macro];
    musyx::Instruments instruments(played, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    song.patterns[0].events[0].length = length;

    const Performance perf = musyx::PlaySong(song, {"attack", "G/A/A", 5, 127}, instruments, {});

    std::vector<uint32_t> ends;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOff)
        {
            ends.push_back(e.tick);
        }
    }

    return ends;
}

// A voice that plays on after its note's release goes on with its attack. A SoundFont's note that ends during its
// attack releases from the level that it has reached. The file's note therefore lasts until the voice's highest level.
// Each macro here plays on the DLS curve, where the level is the square of the volume, with an attack of 200 ms.
// Macro 3 waits for the release, fades its volume to 0 in 960 ms, then releases its envelope. Resident Evil's macros do
// that. A fade to silence leaves the KeyOff nothing to do: the voice fades (KeyOff::kFade), and the fade ends at the
// KeyOff. A note released after 14 ticks, 18.23 ms, reaches its highest level at the attack's end, with the volume at
// 1 - (200 - 18.23) / 960. That level is 0.6573, and the file's attack reaches it after 131.5 ms, tick 101.
// Macro 4 waits 100 ms, then follows FadeSteps' fade and stops by itself (KeyOff::kAsHeld). The file's note lasts for
// the whole attack. The fade is the file's decay. Macro 5 releases its envelope at the note's release. MusyX releases
// from the level reached too. Its note ends with the release.
void TestMusyxAttackAfterRelease()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();
    const ntdlib::musyx::MacroStep select = Step(MacroOp::kModeSelect, {1, 1});
    const ntdlib::musyx::MacroStep adsr = Step(MacroOp::kSetAdsr, {7, 0, 0});
    const ntdlib::musyx::MacroStep start = Step(MacroOp::kStartSample, {1});
    group.pool.tables[7] = {200, 0, 0, 0, 0x00, 0x10, 0x90, 0x01};
    group.pool.macros[3] = {select,
                            adsr,
                            start,
                            Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
                            Step(MacroOp::kEnvelope, {0, 0, 0xff, 0xff, 1, 0xc0, 0x03}),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xc0, 0x03}),
                            Step(MacroOp::kKeyOff),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xe8, 0x03}),
                            Step(MacroOp::kEnd)};
    group.pool.macros[4] = {select, adsr, start, Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0})};
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    group.pool.macros[4].insert(group.pool.macros[4].end(), kFade.begin() + 1, kFade.end());
    group.pool.macros[5] = {select,
                            adsr,
                            start,
                            Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
                            Step(MacroOp::kKeyOff),
                            Step(MacroOp::kEnd)};

    const musyx::MacroVoice late = musyx::FollowMacro(group.pool, 3, 127);
    Check(late.key_off == musyx::KeyOff::kFade && late.release_fade.stop_ms == 960.0 &&
              late.release_fade.ramps.size() == 1,
          "a KeyOff after a fade to silence leaves a voice that fades until the KeyOff");
    Check(musyx::FollowMacro(group.pool, 4, 127).key_off == musyx::KeyOff::kAsHeld, "macro 4 stops by itself");
    Check(musyx::FollowMacro(group.pool, 5, 127).key_off == musyx::KeyOff::kRelease, "macro 5 releases its envelope");

    const std::vector<uint32_t> faded = NoteEnds(group, 3, 14);
    Check(faded.size() == 1 && faded[0] >= 100 && faded[0] <= 102,
          "a note that fades during its attack ends at its highest level, about tick 101: " +
              (faded.empty() ? std::string("none") : std::to_string(faded[0])));
    Check(NoteEnds(group, 3, 192) == std::vector<uint32_t>{192},
          "a note released after its attack ends at the release");
    const std::vector<uint32_t> held = NoteEnds(group, 4, 14);
    Check(held.size() == 1 && held[0] >= 153 && held[0] <= 155,
          "a voice that stops by itself sounds for its whole attack, about tick 154: " +
              (held.empty() ? std::string("none") : std::to_string(held[0])));
    Check(NoteEnds(group, 5, 14) == std::vector<uint32_t>{14}, "a voice that releases at the release ends there");
}

// A voice can play on after its note's release until a later KeyOff. The macros here play on the DLS curve. They use
// the envelope of MakeSongGroup: full level at once, and a release of 100 ms. A note of 192 ticks lasts 250 ms.
// Macro 3 waits for the release, ramps the volume to 96/128 over 250 ms and then releases its envelope. The voice
// fades until the KeyOff (KeyOff::kFade) and releases there. The file's note ends at the release.
// Macro 4 waits 500 ms, then releases its envelope, whatever the note does. Its note lasts until the KeyOff, 384 ticks.
// Macro 5 waits 300 ms and stops, whatever the note does (KeyOff::kAsHeld). Its note lasts until the stop, 230 ticks.
// A note longer than the KeyOff or the stop ends there.
void TestMusyxLaterKeyOff()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();
    const ntdlib::musyx::MacroStep select = Step(MacroOp::kModeSelect, {1, 1});
    const ntdlib::musyx::MacroStep adsr = Step(MacroOp::kSetAdsr, {2, 0, 1});
    const ntdlib::musyx::MacroStep start = Step(MacroOp::kStartSample, {1});
    group.pool.macros[3] = {select,
                            adsr,
                            start,
                            Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
                            Step(MacroOp::kEnvelope, {96, 0, 0xff, 0xff, 1, 0xfa, 0}),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xfa, 0}),
                            Step(MacroOp::kKeyOff),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xe8, 0x03}),
                            Step(MacroOp::kEnd)};
    group.pool.macros[4] = {select,
                            adsr,
                            start,
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xf4, 0x01}),
                            Step(MacroOp::kKeyOff),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xe8, 0x03}),
                            Step(MacroOp::kEnd)};
    group.pool.macros[5] = {select,
                            adsr,
                            start,
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0x2c, 0x01}),
                            Step(MacroOp::kStopSample),
                            Step(MacroOp::kEnd)};

    const musyx::MacroVoice fades = musyx::FollowMacro(group.pool, 3, 127);
    Check(fades.key_off == musyx::KeyOff::kFade && fades.release_fade.releases && fades.release_fade.stop_ms == 250.0 &&
              !fades.releases_itself,
          "a voice with a KeyOff 250 ms after its release fades until it, then releases");
    const musyx::MacroVoice itself = musyx::FollowMacro(group.pool, 4, 127);
    Check(itself.key_off == musyx::KeyOff::kRelease && itself.releases_itself && itself.stop_ms == 500.0,
          "a voice whose KeyOff comes at 500 ms whatever the release does releases itself");
    const musyx::MacroVoice stops = musyx::FollowMacro(group.pool, 5, 127);
    Check(stops.key_off == musyx::KeyOff::kAsHeld && stops.held_ramps.empty() && stops.stop_ms == 300.0,
          "a voice that stops at 300 ms whatever the release does ignores the release");

    Check(NoteEnds(group, 3, 192) == std::vector<uint32_t>{192}, "the fading voice's note ends at its release");
    const std::vector<uint32_t> released = NoteEnds(group, 4, 14);
    Check(released.size() == 1 && released[0] >= 383 && released[0] <= 385,
          "a short note of the voice that releases itself lasts until the KeyOff: " +
              (released.empty() ? std::string("none") : std::to_string(released[0])));
    const std::vector<uint32_t> long_note = NoteEnds(group, 4, 576);
    Check(long_note.size() == 1 && long_note[0] >= 383 && long_note[0] <= 385, "a longer note ends at the KeyOff");
    const std::vector<uint32_t> stopped = NoteEnds(group, 5, 14);
    Check(stopped.size() == 1 && stopped[0] >= 229 && stopped[0] <= 231,
          "a short note of the voice that stops by itself lasts until the stop: " +
              (stopped.empty() ? std::string("none") : std::to_string(stopped[0])));
}

// The values of generator `oper` in the zones of `sf2` that play a sample at key 60 (PlaysKey60), in the order of the
// zones.
std::vector<int> SampleZoneValues(const std::vector<uint8_t>& sf2, uint16_t oper)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<int> values;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (gens.contains(seqlib::sf2::kSampleId) && gens.contains(oper) && PlaysKey60(gens))
        {
            values.push_back(static_cast<int16_t>(gens.at(oper)));
        }
    }

    return values;
}

// A zone whose voice fades after its release gets a release that reaches 20 dB down when the fade does. The test's
// macro plays on the DLS curve, where the level is the square of the volume. With FadeSteps, the volume reaches
// sqrt(0.1) at 120 + 800 × (1 - sqrt(0.1) / 0.625) ms of MusyX's clock, about 515.2 ms. A SoundFont release falls
// 100 dB in its time, and an SFZ release 8 × 20 / ln 10 dB.
void TestMusyxReleaseFade()
{
    namespace sf2 = seqlib::sf2;
    musyx::SongGroup group = MakeSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.resize(3);
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    macro.insert(macro.end(), kFade.begin(), kFade.end());
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(false, false), {"fade", "G/A/A", 5, 127}, instruments, {});

    const double kFitMs = (120.0 + 800.0 * (1.0 - std::sqrt(0.1) / 0.625)) * musyx::kNominalRate / musyx::kOutputRate;
    const std::vector<int> releases = SampleZoneValues(c.sf2, sf2::kReleaseVolEnv);
    const int kExpected = sf2::ToTimecents(5.0 * kFitMs);
    Check(
        releases.size() == 1 && std::abs(releases[0] - kExpected) <= 1,
        "the SoundFont release lasts 5 times the fade's time 20 dB down: " + std::to_string(kExpected) + " timecents");
    const std::vector<std::string> sfz = OpcodeValues(c, "ampeg_release");
    const double kSeconds = 8.0 / std::numbers::ln10 * kFitMs / 1000.0;
    Check(sfz.size() == 1 && std::abs(std::stod(sfz[0]) - kSeconds) < 1e-3,
          "the SFZ release lasts " + std::to_string(kSeconds) + " s: " + (sfz.empty() ? "none" : sfz[0]));
    Check(c.performance.approximations.empty(), "the conversion reports no approximation");
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
    musyx::SongGroup group = MakeSongGroup();
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    group.pool.macros[1] = {Step(MacroOp::kModeSelect, {1, 1}), Step(MacroOp::kStartSample, {1}),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0})};
    group.pool.macros[1].insert(group.pool.macros[1].end(), kFade.begin() + 1, kFade.end());
    group.pool.macros[2] = {Step(MacroOp::kModeSelect, {1, 1}), Step(MacroOp::kStartSample, {1}),
                            Step(MacroOp::kEnvelope, {64, 0, 0xff, 0xff, 1, 0x90, 0x01}),
                            Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}), Step(MacroOp::kEnd)};
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 360, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 400, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 96});

    const Conversion c = musyx::ConvertSong(song, {"held", "G/A/A", 5, 127}, instruments, {});

    constexpr double kToReal = musyx::kNominalRate / musyx::kOutputRate;
    const double kSilentFitMs = (120.0 + 800.0 * (1.0 - std::sqrt(0.1) / 0.625)) * kToReal;
    const double kHalfFitMs = 400.0 * (1.0 - std::sqrt(0.5)) / 0.5 * kToReal;
    const double kHalfDb = -20.0 * std::log10(0.25);
    const std::vector<int> holds = SampleZoneValues(c.sf2, sf2::kHoldVolEnv);
    const std::vector<int> decays = SampleZoneValues(c.sf2, sf2::kDecayVolEnv);
    const std::vector<int> sustains = SampleZoneValues(c.sf2, sf2::kSustainVolEnv);
    const std::vector<int> releases = SampleZoneValues(c.sf2, sf2::kReleaseVolEnv);
    Check(holds.size() == 2 && holds[0] == sf2::ToTimecents(100.0 * kToReal) && holds[1] == -12000,
          "the first zone holds until its fade starts, 100 ms in, and the second has no hold");
    Check(decays.size() == 2 && std::abs(decays[0] - sf2::ToTimecents(5.0 * kSilentFitMs)) <= 1 &&
              std::abs(decays[1] - sf2::ToTimecents(100.0 / kHalfDb * 2.0 * kHalfFitMs)) <= 1,
          "each decay reaches its fit's level when the fade does");
    Check(sustains == std::vector<int>{1440, static_cast<int>(std::lround(10.0 * kHalfDb))},
          "the first sustain is silent, and the second 12.04 dB down");
    Check(releases.size() == 2 && decays.size() == 2 && releases[0] == decays[0] &&
              releases[1] == sf2::ToTimecents(4.5 * 10.0 * kToReal),
          "the first release goes on at the decay's rate, and the second is the cut of a stopped voice");

    const std::vector<std::string> sfz_holds = OpcodeValues(c, "ampeg_hold");
    const std::vector<std::string> sfz_decays = OpcodeValues(c, "ampeg_decay");
    const std::vector<std::string> sfz_releases = OpcodeValues(c, "ampeg_release");
    Check(sfz_holds.size() == 1 && std::abs(std::stod(sfz_holds[0]) - 0.1 * kToReal) < 1e-6,
          "the first SFZ region holds for 100 ms");
    Check(sfz_decays.size() == 2 && sfz_releases.size() == 2 &&
              std::abs(std::stod(sfz_decays[0]) - 8.0 / std::numbers::ln10 * kSilentFitMs / 1000.0) < 1e-3 &&
              sfz_releases[0] == sfz_decays[0],
          "the first region's decay and release fall at the fit's rate");
    Check(std::abs(c.performance.seconds - 1.02) < 1e-9,
          "the song lasts until the first voice falls silent: " + std::to_string(c.performance.seconds));
    Check(c.performance.approximations ==
              std::map<std::string, uint32_t>{{"voices that loop and ignore the release stop at their note's end", 1}},
          "only the voice that keeps sounding is reported, for stopping at its note's end");
    Check(ntdlib::testing::CheckConversion(c, "held") == 0, "independent readers accept the files");
}

// A fade-in at a voice's start becomes the zone's delay and attack, and the hold fills the time from the attack's end
// to the fade's start. The voice fades in over 50 ms, waits 100 ms from its start and follows FadeSteps' fade. Its
// zone holds for the other 50 ms. The game runs the fade-in and the wait at once (Mario Party 4's se-2067 fades in
// over 1 s and fades out 4.5 s after its start). The voice plays on the DLS curve, whose level is the square of the
// volume. It reaches 20 dB down at sqrt(0.1) of the fade. The zone's attack, a line in amplitude, reaches 20 dB down
// then too after a delay of (sqrt(0.1) - 0.1) / 0.9 of the fade, about 12.0 ms.
void TestFadeInHold()
{
    namespace sf2 = seqlib::sf2;
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    group.pool.macros[1] = {Step(MacroOp::kModeSelect, {1, 1}), Step(MacroOp::kFadeIn, {127, 0, 0xff, 0xff, 1, 50, 0}),
                            Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0})};
    group.pool.macros[1].insert(group.pool.macros[1].end(), kFade.begin() + 1, kFade.end());
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(false, false), {"fade-in", "G/A/A", 5, 127}, instruments, {});

    constexpr double kToReal = musyx::kNominalRate / musyx::kOutputRate;
    const double kDelayMs = (std::sqrt(0.1) - 0.1) / 0.9 * 50.0;
    const std::vector<int> delays = SampleZoneValues(c.sf2, sf2::kDelayVolEnv);
    const std::vector<int> attacks = SampleZoneValues(c.sf2, sf2::kAttackVolEnv);
    Check(delays.size() == 1 && std::abs(delays[0] - sf2::ToTimecents(kDelayMs * kToReal)) <= 1,
          "the zone waits about 12.0 ms before its attack");
    Check(attacks.size() == 1 && std::abs(attacks[0] - sf2::ToTimecents((50.0 - kDelayMs) * kToReal)) <= 1,
          "the zone attacks for the rest of the fade-in's 50 ms");
    Check(SampleZoneValues(c.sf2, sf2::kHoldVolEnv) == std::vector<int>{sf2::ToTimecents(50.0 * kToReal)},
          "the zone holds for the 50 ms from the attack's end to the fade's start");
    const std::vector<std::string> sfz_holds = OpcodeValues(c, "ampeg_hold");
    Check(sfz_holds.size() == 1 && std::abs(std::stod(sfz_holds[0]) - 0.05 * kToReal) < 1e-6,
          "the SFZ region holds for 50 ms");
    const std::vector<std::string> sfz_delays = OpcodeValues(c, "ampeg_delay");
    Check(sfz_delays.size() == 1 && std::abs(std::stod(sfz_delays[0]) - kDelayMs * kToReal / 1000.0) < 1e-5,
          "the SFZ region waits about 12.0 ms before its attack");
}

// Each instrument's global zone holds seqlib's kGlobalZoneModulators and a modulator from CC1 to the vibrato's depth.
// The set's CC10 modulator has an amount of 500 and replaces the default one of 1000. The CC1 modulator has an amount
// of 0 and replaces the default one of 50 cents.
void TestMusyxGlobalZone()
{
    namespace sf2 = seqlib::sf2;
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(false, false), {"global", "G/A/A", 5, 127}, instruments, {});

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
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(true, false);
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

// MusyX sends a section's clock back once the section has no events left. Here track 1 plays its pattern on past its
// loop entry at tick 768, to a volume change at tick 1000. The loop comes there for both tracks, and the second pass
// starts at tick 1000.
void TestMusyxLoopWaitsForPatterns()
{
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(true, false);
    ntdlib::musyx::Track track;
    track.number = 1;
    track.channel = 1;
    track.entries.push_back({.tick = 0, .pattern = 1, .volume = 100});
    track.entries.push_back({.tick = 768, .pattern = ntdlib::musyx::kLoopBack, .loop_target = 0});
    song.tracks.push_back(track);
    ntdlib::musyx::Pattern pattern;
    pattern.events.push_back(
        {.tick = 1000, .kind = ntdlib::musyx::PatternEventKind::kController, .controller = 7, .value = 64});
    pattern.end_tick = 1152;
    song.patterns.push_back(pattern);

    const Performance perf = musyx::PlaySong(song, {"wait", "G/A/A", 5, 127}, instruments, {});

    Check(perf.loop && perf.loop->start == 0 && perf.loop->end == 1000 && perf.end_tick == 1000,
          "the loop runs from tick 0 to 1000: " +
              (perf.loop ? std::to_string(perf.loop->start) + " to " + std::to_string(perf.loop->end) : "none"));
}

// A macro's PitchWheelRange gives its voice the pitch wheel's range. This one sets 12 semitones up and 6 down. The
// channel takes its latest note's range. The pattern moves the wheel halfway up from its centre during the note. It
// then moves the wheel halfway down.
void TestMusyxPitchWheelRange()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(), Step(MacroOp::kPitchWheelRange, {12, 6}));
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(true, false);
    song.patterns[0].pitch_wheel = {{.tick = 96, .value = 0x3000}, {.tick = 144, .value = 0x1000}};

    const Performance perf = musyx::PlaySong(song, {"bend", "G/A/A", 5, 127}, instruments, {});

    Check(BendsOf(perf) == std::vector<float>{6.0f, -3.0f},
          "the wheel bends 6 semitones up and 3 down over the voice's range");
}

// A MIDI channel bends its notes over its latest note's range. The first note has a range of 12. At tick 144, a note
// of program 1 gives the channel a range of 2 while the first note is still held. The wheel already stands at 0x3000.
// The report counts the held note once.
void TestMusyxMixedPitchWheelRanges()
{
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(), Step(MacroOp::kPitchWheelRange, {12, 6}));
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 120, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 144, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 192});
    song.patterns[0].pitch_wheel = {{.tick = 96, .value = 0x3000}, {.tick = 168, .value = 0x2800}};

    const Performance perf = musyx::PlaySong(song, {"bends", "G/A/A", 5, 127}, instruments, {});

    Check(BendsOf(perf) == std::vector<float>{6.0f, 1.0f, 0.5f},
          "the second note's range bends the channel from its note on");
    const auto mixed =
        perf.approximations.find("pitch wheel ranges that differ among a channel's notes take the latest note's");
    Check(mixed != perf.approximations.end() && mixed->second == 1, "the held note of another range counts once");
}

// A note of no length ends on the tick it starts. Its note-off comes after its note-on. The MIDI note then ends, and a
// later note of the same key plays on the same channel.
void TestMusyxNoteOfNoLength()
{
    using ntdlib::musyx::PatternEventKind;
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
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
    const std::vector<std::pair<EventKind, uint32_t>> kExpected = {
        {EventKind::kNoteOn, 0}, {EventKind::kNoteOff, 0}, {EventKind::kNoteOn, 384}, {EventKind::kNoteOff, 576}};
    Check(notes == kExpected, "the note of no length ends after its note-on, at its tick");
    Check(c.channels == 1, "both notes play on one MIDI channel");
    Check(ntdlib::testing::CheckConversion(c, "empty") == 0, "independent readers accept the files");
}

// VolumeSelect picks the sources of a voice's volume. Byte 1 names the controller, and bytes 2 and 3 hold its share in
// percent. Byte 6 adds 256ths of a percent. Byte 4 picks how the source joins the others: 0 replaces them. The voice's
// first VolumeSelect replaces the source that MusyX starts it on. Byte 4 changes nothing there. A share of the volume
// controller (CC7) that replaces the others scales the voice's volume. MusyX holds the controller's value times the
// share at full volume. Byte 5 makes byte 1 name a variable. A variable that no step sets gives half of full volume.
// The controller doesn't change it. Other sources are reported.
void TestVolumeSelect()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kVolumeSelect, {7, 40}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kVolumeSelect, {7, 127}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kVolumeSelect, {1, 50}), Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kVolumeSelect, {7, 60, 0, 1}), Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kEnd)};
    pool.macros[6] = {Step(MacroOp::kVolumeSelect, {7, 60}), Step(MacroOp::kVolumeSelect, {7, 20, 0, 1}),
                      Step(MacroOp::kEnd)};
    pool.macros[7] = {Step(MacroOp::kVolumeSelect, {7, 120, 0, 0, 1}), Step(MacroOp::kEnd)};
    pool.macros[8] = {Step(MacroOp::kSetVariable, {0, 7, 0, 100}), Step(MacroOp::kVolumeSelect, {7, 100, 0, 0, 1}),
                      Step(MacroOp::kEnd)};
    pool.macros[9] = {Step(MacroOp::kVolumeSelect, {7, 50, 0, 0, 0, 64}), Step(MacroOp::kEnd)};

    const musyx::MacroVoice part = musyx::FollowMacro(pool, 1, 127);
    Check(part.source_volume == 0.4 && musyx::VoiceVolume(part, 0.5, 1.0) == 0.2 && part.approximations.empty(),
          "a share of 40% of the volume controller scales the voice's volume by 0.4");
    Check(musyx::VoiceVolume(musyx::FollowMacro(pool, 2, 127), 1.0, 1.0) == 1.0,
          "a share above 100% plays at full volume with the controller at full");
    const std::vector<std::string> kOther = {"volume sources other than a share of the volume controller left out"};
    Check(musyx::FollowMacro(pool, 3, 127).approximations == kOther, "another controller is reported");
    const musyx::MacroVoice first = musyx::FollowMacro(pool, 4, 127);
    Check(first.source_volume == 0.6 && first.approximations.empty(),
          "the first VolumeSelect replaces the voice's source, even one that asks to join it");
    Check(musyx::FollowMacro(pool, 5, 127).source_volume == 1.0, "a voice follows the volume controller in full");
    const musyx::MacroVoice joined = musyx::FollowMacro(pool, 6, 127);
    Check(joined.source_volume == 0.6 && joined.approximations == kOther,
          "a later source that joins the others is reported");
    const musyx::MacroVoice variable = musyx::FollowMacro(pool, 7, 127);
    Check(variable.source_fixed && variable.source_volume == 0x2000 / 16383.0 &&
              musyx::VoiceVolume(variable, 1.0, 0.25) == variable.source_volume && variable.approximations.empty(),
          "a variable that no step sets gives half of full volume at any controller and share");
    Check(musyx::FollowMacro(pool, 8, 127).approximations == kOther,
          "a variable source after a step that sets a variable is reported");
    Check(musyx::FollowMacro(pool, 9, 127).source_volume == 0.5025, "byte 6 adds 256ths of a percent to the share");
}

// ReverbSelect picks the source of a voice's send to aux A, with VolumeSelect's fields. The first one replaces the
// reverb controller (CC91). MusyX takes a variable and the mod wheel (CC1) as signed sources around half of full. An
// effect's mod wheel stays at 0, and a macro's variables start at 0. The effect's note sends what the source gives.
void TestReverbSelect()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();

    const auto played = [](uint8_t source, uint8_t percent, uint8_t variable)
    {
        return std::vector<ntdlib::musyx::MacroStep>{Step(MacroOp::kReverbSelect, {source, percent, 0, 0, variable}),
                                                     Step(MacroOp::kStartSample, {1}),
                                                     Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff})};
    };

    group.pool.macros[3] = played(1, 20, 0);
    group.pool.macros[4] = played(1, 50, 1);
    group.pool.macros[5] = played(91, 50, 0);
    group.pool.macros[6] = {Step(MacroOp::kReverbSelect, {1, 20}), Step(MacroOp::kReverbSelect, {91, 50, 0, 1}),
                            Step(MacroOp::kEnd)};
    musyx::Instruments instruments(group, SampleVolumes());

    const std::optional<musyx::InputSource>& wheel = instruments.Macro(3).reverb_source;
    const std::optional<musyx::InputSource>& variable = instruments.Macro(4).reverb_source;
    const std::optional<musyx::InputSource>& reverb = instruments.Macro(5).reverb_source;
    if (!Check(wheel && variable && reverb, "each ReverbSelect gives a source"))
    {
        return;
    }

    Check(wheel->controller == 1 && !wheel->variable && wheel->share == 0.2 &&
              instruments.Macro(3).approximations.empty(),
          "ReverbSelect names the mod wheel at 20%");
    Check(musyx::SourceValue(*wheel, 0.0) == 0x2000 - 1639, "the mod wheel at 0 gives 80% of half of full");
    Check(musyx::SourceValue(*variable, 0.0) == 0x2000, "a variable that no step sets gives half of full at any share");
    Check(musyx::SourceValue(*reverb, 127 * 128.0) == 127 * 64,
          "the reverb controller gives its value times the share");
    Check(instruments.Macro(6).approximations == std::vector<std::string>{musyx::kReverbSourcesLeftOut},
          "a later source that joins the others is reported");

    const auto send = [&](uint16_t object, uint8_t reverb)
    {
        musyx::GameEffect effect;
        effect.name = "reverb";
        effect.row = {.id = 1, .object = object, .max_voices = 1, .key = 60};
        effect.reverb = reverb;
        int value = -1;
        for (const std::vector<TrackEvent>& track : musyx::ConvertEffect(effect, instruments).performance.tracks)
        {
            for (const TrackEvent& e : track)
            {
                value = e.kind == EventKind::kReverb ? e.value : value;
            }
        }

        return value;
    };

    Check(send(3, 0) == 51 && send(4, 0) == 64 && send(5, 100) == 50,
          "an effect's note sends what its voice's source gives");
}

// MusyX updates a voice's pitch every 15 ms and at the end of each wait. A pitch sweep adds its step in Hz, in the
// DSP's units of 4096ths of 32000 Hz, every 16 ms. It starts again after its number of steps. The DSP plays a pitch of
// 4 times its rate or more at 0x3fff. The curve counts from the voice's settings. An effect's voice whose pitch moves
// gets a channel of its own. The curve bends that channel.
void TestPitchCurve()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    const ntdlib::musyx::MacroStep held = Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff});
    pool.macros[1] = {Step(MacroOp::kSetKey, {60}), Step(MacroOp::kPitchSweep1, {4, 0x20, 0x03}),
                      Step(MacroOp::kStartSample, {1}), held};
    pool.macros[2] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 20, 0}),
                      Step(MacroOp::kAddKey, {12}), held};
    pool.macros[3] = {Step(MacroOp::kSetKey, {84}), Step(MacroOp::kStartSample, {1}), held};
    pool.macros[4] = {Step(MacroOp::kPitchSweep1, {2, 0x64, 0, 0, 1, 50, 0}), Step(MacroOp::kStartSample, {1}), held};
    musyx::Sample sample;
    sample.pcm.sample_rate = 32000.0f;
    sample.pcm.channels = {std::vector<int16_t>(32000)};

    // 800 Hz is 102 units. A step of 15 ms adds 15/16 of it.
    const musyx::MacroVoice sweep = musyx::FollowMacro(pool, 1, 127);
    const std::vector<musyx::PitchPoint> curve = musyx::PitchCurve(sweep, sample, 60, 0.0, 80.0);

    const auto at = [](double units)
    {
        return 12.0 * std::log2(units / 4096.0);
    };

    Check(sweep.sweeps.size() == 1 && sweep.sweeps[0].steps == 4 && sweep.sweeps[0].step == 102 &&
              sweep.approximations.empty(),
          "PitchSweep1 sweeps 4 steps of 800 Hz");
    Check(curve.size() == 6 && curve[0] == musyx::PitchPoint{0.0, 0.0} && curve[1].ms == 15.0 &&
              std::abs(curve[1].semitones - at(4096 + 102 * 15 / 16.0)) < 1e-9 && curve[4].ms == 60.0 &&
              std::abs(curve[4].semitones - at(4096 + 102 * 60 / 16.0)) < 1e-9 &&
              curve[5] == musyx::PitchPoint{75.0, 0.0},
          "the sweep rises at each update of 15 ms and starts again once its 64 ms have passed");

    const musyx::MacroVoice later = musyx::FollowMacro(pool, 2, 127);
    Check(musyx::PitchCurve(later, sample, 60, 0.0, 100.0) == std::vector<musyx::PitchPoint>{{0.0, -12.0}, {20.0, 0.0}},
          "a key that the macro adds while the sample plays moves the pitch at the end of the wait");
    const double kRate = musyx::kOutputRate / 1000.0;
    const std::optional<double> ms = musyx::CurveSampleMs(later, sample, 60);
    Check(ms && std::abs(*ms - (20.0 + (32000.0 - 20.0 * kRate) / (2.0 * kRate))) < 1e-9,
          "the sample plays at its rate for 20 ms, then twice as fast");

    const std::vector<musyx::PitchPoint> high =
        musyx::PitchCurve(musyx::FollowMacro(pool, 3, 127), sample, 60, 0.0, 100.0);
    Check(high.size() == 1 && std::abs(high[0].semitones - at(0x3fff / 4.0)) < 1e-9 && musyx::PitchMoves(high, 0.0),
          "the DSP plays 4 times the rate at 0x3fff");
    Check(musyx::FollowMacro(pool, 4, 127).sample_ms == 50.0, "the sweep waits as AddKey does");

    // An effect of a voice that starts a voice whose pitch sweeps.
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[3] = {Step(MacroOp::kPlayMacro, {0, 4, 0}), Step(MacroOp::kStartSample, {1}), held};
    group.pool.macros[4] = {Step(MacroOp::kPitchSweep1, {100, 0x64}), Step(MacroOp::kStartSample, {1}), held};
    musyx::Instruments instruments(group, SampleVolumes());
    musyx::GameEffect effect;
    effect.name = "sweep";
    effect.row = {.id = 1, .object = 3, .max_voices = 2, .key = 60};
    std::vector<std::size_t> bends;
    for (const std::vector<TrackEvent>& track : musyx::ConvertEffect(effect, instruments).performance.tracks)
    {
        if (std::any_of(track.begin(), track.end(), [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; }))
        {
            bends.push_back(std::count_if(track.begin(), track.end(),
                                          [](const TrackEvent& e) { return e.kind == EventKind::kPitch; }));
        }
    }
    Check(bends.size() == 2 && bends[0] == 0 && bends[1] > 100,
          "the voice whose pitch sweeps plays on a channel of its own, and the channel bends");
}

// A Panning step sets the pan at once and moves it in a line over its time. MusyX moves it at each update by the time
// since the update before. The curve counts from the pan of the last Panning step, the zone's pan. An effect's note
// follows the curve with CC10.
void TestPanCurve()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[3] = {Step(MacroOp::kPanning, {48, 0xa0, 0, 0x20}), Step(MacroOp::kStartSample, {1}),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 160, 0}),
                            Step(MacroOp::kPanning, {80, 0xa0, 0, 0xe0}),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff})};

    const musyx::MacroVoice voice = musyx::FollowMacro(group.pool, 3, 127);
    const std::vector<musyx::PanPoint> curve = musyx::PanCurve(voice, 400.0);

    const auto at = [&](double ms)
    {
        const auto p = std::find_if(curve.begin(), curve.end(), [ms](const musyx::PanPoint& q) { return q.ms == ms; });
        return p != curve.end() ? p->offset : 1000.0;
    };

    Check(voice.pan == 80 && std::abs(at(0.0) + 32.0) < 1e-9 && std::abs(at(15.0) + 29.0) < 1e-9 &&
              std::abs(at(150.0) + 2.0) < 1e-9 && std::abs(at(175.0) + 5.0) < 1e-9 && at(160.0) == 1000.0,
          "the pan glides from 48 to 80 and back, counted from the last pan");

    musyx::Instruments instruments(group, SampleVolumes());
    std::set<std::string> left_out;
    musyx::GameEffect effect;
    effect.name = "pan";
    effect.row = {.id = 1, .object = 3, .max_voices = 1, .pan = 64, .key = 60};
    // The zone starts at 48, the pan at the note's start: the voice's pan less 32. CC10 starts at the channel's pan and
    // adds the 32 back as the pan glides to 80.
    std::vector<std::pair<uint32_t, int>> pans;
    for (const std::vector<TrackEvent>& track : musyx::ConvertEffect(effect, instruments).performance.tracks)
    {
        for (const TrackEvent& e : track)
        {
            if (e.kind == EventKind::kPan)
            {
                pans.emplace_back(e.tick, e.value);
            }
        }
    }

    const std::vector<musyx::EffectNote> notes = musyx::EffectNotes(effect, instruments, left_out);
    Check(notes.size() == 1 && notes[0].pan_shift == -32.0 && pans.size() == 21 &&
              pans[0] == std::pair<uint32_t, int>(0, 64) && pans[1] == std::pair<uint32_t, int>(16, 67) &&
              pans[10].second == 94 && pans.back().second == 64,
          "the effect's CC10 follows the voice's pan");

    // A glide across the whole width requires the shift. The pan falls to 127/16 at 150 ms. CC10 reaches 0 there, and
    // the zone sits at 64 plus 127/16.
    group.pool.macros[3][0] = Step(MacroOp::kPanning, {127, 0xa0, 0, 0x81});
    group.pool.macros[3][3] = Step(MacroOp::kPanning, {0, 0xa0, 0, 0x7f});
    musyx::Instruments wide(group, SampleVolumes());
    const std::vector<musyx::EffectNote> wide_notes = musyx::EffectNotes(effect, wide, left_out);
    Check(wide_notes.size() == 1 && std::abs(wide_notes[0].pan_shift - (64.0 + 127.0 / 16.0)) < 1e-9,
          "a glide from 127 to 0 and back keeps CC10 within 0 to 127");
}

// The DSP plays a voice at most just under 4 times its sample's rate. The test's sample plays at 32 kHz at key 60. From
// key 84 up, a song's voice asks for 4 times that rate or more. Its zone then plays at the limit at every key: 2401
// cents above the sample, with the output's rate.
void TestHighestPitch()
{
    namespace sf2 = seqlib::sf2;
    const musyx::SongGroup group = MakeSongGroup();
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(true, false), {"loop", "G/A/A", 5, 127}, instruments, {});

    const sf2::Records records = sf2::Read(c.sf2);
    std::vector<std::array<int, 4>> zones; // the low key, the high key, the scale tuning and the tuning in cents
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (!gens.contains(sf2::kSampleId))
        {
            continue;
        }

        const auto get = [&](uint16_t g, int fallback)
        {
            return gens.contains(g) ? static_cast<int>(static_cast<int16_t>(gens.at(g))) : fallback;
        };

        const int keys = gens.contains(sf2::kKeyRange) ? gens.at(sf2::kKeyRange) : 0x7f00;
        zones.push_back({keys & 0xff, keys >> 8, get(sf2::kScaleTuning, 100),
                         get(sf2::kCoarseTune, 0) * 100 + get(sf2::kFineTune, 0)});
    }
    const double kLimit = 1200.0 * std::log2(musyx::HighestPitchHz() / 32000.0);
    Check(zones.size() == 2 && zones[0][0] == 0 && zones[0][1] == 83 && zones[0][2] == 100 && zones[1][0] == 84 &&
              zones[1][1] == 127 && zones[1][2] == 0 && zones[1][3] == std::lround(kLimit) && zones[1][3] == 2401,
          "keys from 84 up play at the DSP's highest pitch");
}

// A macro's KeyOff releases its voice's envelope. The note ends there and plays its release. The report names no
// approximation.
void TestSelfRelease()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[3] = {Step(MacroOp::kSetAdsr, {2, 0, 1}), Step(MacroOp::kStartSample, {1}),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0}), Step(MacroOp::kKeyOff),
                            Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff})};

    const musyx::MacroVoice voice = musyx::FollowMacro(group.pool, 3, 127);

    Check(voice.stop_ms == 100.0 && voice.key_off == musyx::KeyOff::kRelease && voice.approximations.empty(),
          "a voice that releases itself ends its note at the KeyOff and plays its envelope's release");
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
    const musyx::SongGroup plain = MakeSongGroup();
    musyx::SongGroup group = MakeSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin() + 1, Step(MacroOp::kPanning, {0}));
    macro.insert(macro.begin() + 1, Step(MacroOp::kSetVariable, {1, 7, 0, 0x00, 0x20}));
    musyx::Instruments plain_instruments(plain, SampleVolumes());
    musyx::Instruments instruments(group, SampleVolumes());
    const ntdlib::musyx::Song song = OneTrackSong(false, false);

    const Conversion before = musyx::ConvertSong(song, {"pans", "G/A/A", 5, 127}, plain_instruments, {});
    const Conversion after = musyx::ConvertSong(song, {"pans", "G/A/A", 5, 127}, instruments, {});

    Check(OpcodeValues(before, "pan").empty() && OpcodeValues(after, "pan") == std::vector<std::string>{"-100"},
          "the song's region pans hard left");
    bool same = after.midi == before.midi && after.sfz.files.size() == before.sfz.files.size();
    for (std::size_t i = 0; same && i < after.sfz.files.size(); i++)
    {
        std::string text = after.sfz.files[i].text;
        for (std::size_t at = text.find(" pan=-100"); at != std::string::npos; at = text.find(" pan=-100", at))
        {
            text.erase(at, std::string(" pan=-100").size());
        }
        same = text == before.sfz.files[i].text;
    }
    Check(same, "the rest of the song's files stays the same");
    const auto& reported = after.performance.approximations;
    Check(reported.count(musyx::kCommandsLeftOut) == 0 &&
              reported.count("volumes and sends that a macro sets left out") == 1,
          "the song reports the controller alone");

    musyx::GameEffect effect;
    effect.name = "left";
    effect.row = {.id = 1, .object = 1, .max_voices = 1, .key = 60};

    const Conversion c = musyx::ConvertEffect(effect, instruments);

    Check(OpcodeValues(c, "pan") == std::vector<std::string>{"-100"}, "the effect's region pans hard left");
    Check(ntdlib::testing::CheckConversion(c, "left") == 0, "independent readers accept the effect's files");
}

// A song's note whose voice sweeps its pitch bends along its voice's curve on a part of its channel. The channel's
// other note stays on the first part, unbent. The part takes the channel's settings.
void TestMusyxSongSweep()
{
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[2] = group.pool.macros[1];
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(), Step(MacroOp::kPitchSweep1, {4, 0x20, 0x03}));
    group.group.pages.push_back({.object = 2, .priority = 0, .max_voices = 8, .program = 1});
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 96, .kind = PatternEventKind::kProgram, .value = 1});
    events.push_back({.tick = 96, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 192});

    const Performance perf = musyx::PlaySong(song, {"sweep", "G/A/A", 5, 127}, instruments, {});

    std::map<uint8_t, uint8_t> part_of_key;
    std::map<uint8_t, std::vector<float>> bends;
    std::map<uint8_t, int> settings;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            part_of_key[e.key] = e.part;
        }
        else if (e.kind == EventKind::kPitch)
        {
            bends[e.part].push_back(e.semitones);
        }
        else if (e.kind == EventKind::kPreset || e.kind == EventKind::kVolume)
        {
            settings[e.part]++;
        }
    }
    Check(part_of_key == std::map<uint8_t, uint8_t>{{60, 1}, {62, 0}}, "the sweeping note plays on part 1");
    Check(bends[1].size() > 4 && *std::max_element(bends[1].begin(), bends[1].end()) > 0.3f && bends[1].back() == 0.0f,
          "part 1 bends along the sweep and comes back to the wheel at the end");
    Check(std::all_of(bends[0].begin(), bends[0].end(), [](float s) { return s == 0.0f; }), "part 0 doesn't bend");
    Check(settings[1] == settings[0] && settings[1] >= 3, "part 1 takes the channel's presets and volumes");
    Check(!perf.approximations.contains(musyx::kCommandsLeftOut), "the report leaves out the followed sweep");
}

// SetAdsrFromControllers takes a DLS envelope from four controllers of the song's channel. Bytes 1 to 4 name the
// controllers of the attack, the decay, the sustain and the release. MusyX looks the times up in its table (0x80368A9C
// in Pokémon Colosseum), and the DSP keeps their low 16 bits. A controller from 32 to 63 reads through the one 32 below
// it. A voice without a channel leaves the step out. In a song, each note takes the channel's values at its start, and
// the channel plays a preset of its page for each set of values.
void TestEnvelopeFromControllers()
{
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kSetAdsrFromControllers, {0x14, 0x16, 0x37, 0x18}), Step(MacroOp::kEnd)};

    const musyx::MacroVoice none = musyx::FollowMacro(pool, 1, 127);
    Check(!none.envelope && none.approximations == std::vector<std::string>{musyx::kCommandsLeftOut},
          "a voice without a channel leaves the step out");
    Check(none.controllers == std::vector<uint8_t>{0x14, 0x16, 0x17, 0x18}, "controller 0x37 reads through 0x17");

    const musyx::MacroVoice set = musyx::FollowMacro(pool, 1, 127, {{0x14, 22}, {0x16, 52}, {0x17, 127}, {0x18, 110}});
    Check(set.envelope && set.envelope->dls && set.envelope->times_in_ms && set.envelope->attack_ms == 230.0 &&
              set.envelope->decay_ms == 1800.0 && set.envelope->sustain == 1.0 && set.envelope->release_ms == 34464.0,
          "the times come from the table, and the release of 100,000 ms keeps its low 16 bits");
    const musyx::MacroVoice low = musyx::FollowMacro(pool, 1, 127, {{0x14, 127}, {0x17, 64}});
    Check(low.envelope && low.envelope->attack_ms == 0.0 && low.envelope->decay_ms == 0.0 &&
              low.envelope->release_ms == 0.0,
          "a value of 127 and the cold reset's 0 take no time");
    Check(low.envelope && std::abs(low.envelope->sustain - std::pow(10.0, -0.6)) < 1e-12,
          "a sustain of 64 is 24 steps of 0.5 dB down");

    musyx::SongGroup group = MakeSongGroup();
    group.pool.macros[1][1] = Step(MacroOp::kSetAdsrFromControllers, {0x14, 0x16, 0x17, 0x18});
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;

    const auto controller = [](uint32_t tick, uint8_t number, uint8_t value)
    {
        return ntdlib::musyx::PatternEvent{
            .tick = tick, .kind = PatternEventKind::kController, .controller = number, .value = value};
    };

    events.insert(events.begin(), {controller(0, 0x14, 22), controller(0, 0x17, 127)});
    events.push_back(controller(96, 0x14, 33));
    events.push_back({.tick = 96, .kind = PatternEventKind::kNote, .key = 62, .velocity = 100, .length = 192});
    events.push_back({.tick = 384, .kind = PatternEventKind::kNote, .key = 64, .velocity = 100, .length = 192});

    const Conversion c = musyx::ConvertSong(song, {"controls", "G/A/A", 5, 127}, instruments, {});

    const auto& presets = c.performance.presets;
    if (!Check(presets.size() == 2 && presets[0].bank == musyx::Instruments::kVariantBank &&
                   presets[1].bank == musyx::Instruments::kVariantBank + 1,
               "the channel plays a preset for each set of controller values, and the preset of the page goes unused"))
    {
        return;
    }

    const musyx::Instruments::Variant* first = instruments.VariantOf(presets[0].bank);
    Check(first && first->controls == musyx::ChannelControls{{0x14, 22}, {0x16, 0}, {0x17, 127}, {0x18, 0}},
          "the first preset holds the values at the first note's start");
    Check(Count(c.performance, 0, EventKind::kPreset) == 2, "the third note keeps the second preset");
    const std::vector<std::string> attacks = OpcodeValues(c, "ampeg_attack");
    Check(std::set<std::string>(attacks.begin(), attacks.end()).size() == 2,
          "the two presets' regions attack at different times");
    Check(!c.performance.approximations.contains(musyx::kCommandsLeftOut), "the report leaves out the followed step");
    Check(ntdlib::testing::CheckConversion(c, "controls") == 0, "independent readers accept the files");
}

// SplitModulation takes a song's voice to the macro and step of bytes 2 to 5 once the high 7 bits of its mod wheel
// reach byte 1. SplitVelocity does the same once the voice's volume reaches byte 1: its start volume, as the steps
// before it scale it. A voice without controls, such as an effect's, goes on. A song's channel plays a preset for each
// side of a split that its notes start on.
void TestSplits()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kSplitModulation, {64, 1, 0, 3}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd), Step(MacroOp::kStartSample, {2}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kScaleVolume, {64}), Step(MacroOp::kSplitVelocity, {50, 2, 0, 4}),
                      Step(MacroOp::kStartSample, {1}),  Step(MacroOp::kEnd),
                      Step(MacroOp::kStartSample, {2}),  Step(MacroOp::kEnd)};

    const musyx::MacroVoice plain = musyx::FollowMacro(pool, 1, 127);
    Check(plain.sample == 1 && plain.approximations.empty(), "a voice without controls goes on at the split");
    Check(plain.splits == std::vector<std::pair<uint8_t, uint8_t>>{{1, 64}}, "the split on the mod wheel is listed");
    Check(musyx::FollowMacro(pool, 1, 127, {{1, 64}}).sample == 2, "a mod wheel of 64 takes the voice to step 3");
    Check(musyx::FollowMacro(pool, 1, 127, {{1, 63}}).sample == 1, "a mod wheel of 63 goes on");
    const musyx::MacroVoice loud = musyx::FollowMacro(pool, 2, 127);
    Check(loud.sample == 1 && loud.approximations == std::vector<std::string>{musyx::kCommandsLeftOut},
          "a voice without a start volume goes on and reports the split");
    Check(loud.splits == std::vector<std::pair<uint8_t, uint8_t>>{{musyx::kVelocityControl, 100}},
          "the scale of 64/127 moves the split on 50 to a start volume of 100");
    Check(musyx::FollowMacro(pool, 2, 127, {{musyx::kVelocityControl, 100}}).sample == 2 &&
              musyx::FollowMacro(pool, 2, 127, {{musyx::kVelocityControl, 99}}).sample == 1,
          "a start volume of 100 takes the voice to step 4, and one of 99 goes on");
    Check(musyx::StartVolume(127, 0.5) == 63 && musyx::VoiceControls({{1, 5}, {musyx::kVelocityControl, 100}}, 0.5) ==
                                                    musyx::ChannelControls{{1, 5}, {musyx::kVelocityControl, 50}},
          "a voice's controls hold its share of the velocity");

    musyx::SongGroup group = MakeSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(), Step(MacroOp::kSplitVelocity, {110, 1, 0, 7}));
    macro.push_back(Step(MacroOp::kAddKey, {12}));
    macro.push_back(Step(MacroOp::kGoto, {0, 1, 0, 1}));
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back(
        {.tick = 192, .kind = ntdlib::musyx::PatternEventKind::kNote, .key = 60, .velocity = 120, .length = 96});
    events.push_back(
        {.tick = 384, .kind = ntdlib::musyx::PatternEventKind::kNote, .key = 60, .velocity = 127, .length = 96});

    const Conversion c = musyx::ConvertSong(song, {"splits", "G/A/A", 5, 127}, instruments, {});

    const auto& presets = c.performance.presets;
    Check(presets.size() == 2, "the channel plays a preset for each side of the split");
    const musyx::Instruments::Variant* soft = presets.empty() ? nullptr : instruments.VariantOf(presets[0].bank);
    const musyx::Instruments::Variant* hard = presets.size() < 2 ? nullptr : instruments.VariantOf(presets[1].bank);
    Check(soft && soft->controls == musyx::ChannelControls{{musyx::kVelocityControl, 0}} && hard &&
              hard->controls == musyx::ChannelControls{{musyx::kVelocityControl, 110}},
          "velocities 120 and 127 share the preset of the split's side");
    Check(Count(c.performance, 0, EventKind::kPreset) == 2, "the third note keeps the second preset");
    Check(OpcodeValues(c, "transpose") == std::vector<std::string>{"12"}, "the loud side plays an octave up");
    Check(ntdlib::testing::CheckConversion(c, "splits") == 0, "independent readers accept the files");
}

// The notes of one tick that start on different sides of a split each play the preset of their side. MusyX plays a
// pattern's events in their order, so a note's preset comes after the notes before it on the tick.
void TestSplitsInAChord()
{
    using ntdlib::musyx::MacroOp;
    using ntdlib::musyx::PatternEventKind;
    musyx::SongGroup group = MakeSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(), Step(MacroOp::kSplitVelocity, {110, 1, 0, 7}));
    macro.push_back(Step(MacroOp::kAddKey, {12}));
    macro.push_back(Step(MacroOp::kGoto, {0, 1, 0, 1}));
    musyx::Instruments instruments(group, SampleVolumes());
    ntdlib::musyx::Song song = OneTrackSong(false, false);
    std::vector<ntdlib::musyx::PatternEvent>& events = song.patterns[0].events;
    events.push_back({.tick = 0, .kind = PatternEventKind::kNote, .key = 64, .velocity = 127, .length = 96});
    events.push_back({.tick = 0, .kind = PatternEventKind::kNote, .key = 67, .velocity = 100, .length = 96});

    const Conversion c = musyx::ConvertSong(song, {"chord", "G/A/A", 5, 127}, instruments, {});

    std::map<uint8_t, int> side_of_key;
    std::optional<uint32_t> preset;
    for (const TrackEvent& e : c.performance.tracks[0])
    {
        if (e.kind == EventKind::kPreset)
        {
            preset = e.preset;
        }
        else if (e.kind == EventKind::kNoteOn && preset && *preset < c.performance.presets.size())
        {
            const musyx::Instruments::Variant* v = instruments.VariantOf(c.performance.presets[*preset].bank);
            side_of_key[e.key] = v && !v->controls.empty() ? v->controls[0].second : -1;
        }
    }
    Check(side_of_key == std::map<uint8_t, int>{{60, 0}, {64, 110}, {67, 0}},
          "each note of the chord plays the preset of its side of the split");
    Check(ntdlib::testing::CheckConversion(c, "chord") == 0, "independent readers accept the files");
}

// PitchWheelSelect sets the sources of a voice's pitch wheel. The voice's first one takes the channel's wheel's place.
// An LFO swings the wheel by its share of the wheel's range either way. A voice whose wheel reads neither the channel's
// wheel nor an LFO stays at the centre. A song's voice whose wheel doesn't read the channel's ignores its bends.
void TestPitchWheelSelect()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kSetupLfo, {0, 0x10, 0x27}), Step(MacroOp::kPitchWheelSelect, {0x82, 10}),
                      Step(MacroOp::kStartSample, {1}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kSetupLfo, {0, 0x04, 0x01}),
                      Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0x2c, 0x01}),
                      Step(MacroOp::kPitchWheelSelect, {0x82, 3}),
                      Step(MacroOp::kPitchWheelSelect, {0x80, 100, 0, 1}),
                      Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kPitchWheelSelect, {0x80, 100, 0, 0, 1}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0x2c, 0x01}),
                      Step(MacroOp::kSetupLfo, {0, 0x10, 0x27}), Step(MacroOp::kPitchWheelSelect, {0x82, 10}),
                      Step(MacroOp::kEnd)};

    const musyx::MacroVoice lfo = musyx::FollowMacro(pool, 1, 127);
    Check(!lfo.follows_wheel && lfo.wheel_lfo && lfo.wheel_lfo->delay_ms == 0.0 &&
              lfo.wheel_lfo->period_ms == 10000.0 && std::abs(lfo.wheel_lfo->share - 0.1) < 1e-12,
          "the first select puts LFO 0 at 10% in the channel's wheel's place");
    const musyx::MacroVoice added = musyx::FollowMacro(pool, 2, 127);
    Check(added.follows_wheel && added.wheel_lfo && added.wheel_lfo->delay_ms == 300.0 &&
              added.wheel_lfo->period_ms == 260.0 && std::abs(added.wheel_lfo->share - 0.03) < 1e-12 &&
              added.approximations.empty(),
          "a second select adds the channel's wheel, from 300 ms on");
    const musyx::MacroVoice variable = musyx::FollowMacro(pool, 3, 127);
    Check(!variable.follows_wheel && !variable.wheel_lfo, "a variable that no step sets holds the wheel at its centre");
    Check(musyx::FollowMacro(pool, 4, 127).approximations ==
              std::vector<std::string>{"pitch wheels that a macro turns later turn at the note's start"},
          "a wheel that turns after 300 ms is reported");

    musyx::SongGroup group = MakeSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin(),
                 {Step(MacroOp::kSetupLfo, {0, 0x10, 0x27}), Step(MacroOp::kPitchWheelSelect, {0x82, 10})});
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(false, false), {"wheel", "G/A/A", 5, 127}, instruments, {});

    Check(OpcodeValues(c, "lfo3_pitch") == std::vector<std::string>{"20"} &&
              OpcodeValues(c, "bend_up") == std::vector<std::string>{"0"},
          "the region swings 20 cents either way on LFO 3 and ignores the channel's bends");
    Check(ntdlib::testing::CheckConversion(c, "wheel") == 0, "independent readers accept the files");
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
    Check(musyx::FollowMacro(pool, 3, 127).approximations == std::vector<std::string>{musyx::kCommandsLeftOut},
          "a step with more bytes is reported");

    musyx::SongGroup group = MakeSongGroup();
    std::vector<ntdlib::musyx::MacroStep>& macro = group.pool.macros[1];
    macro.insert(macro.begin() + 1, Step(MacroOp::kSetPitch, {0x00, 0x3e, 0x80}));
    musyx::Instruments instruments(group, SampleVolumes());

    const Conversion c = musyx::ConvertSong(OneTrackSong(false, false), {"rate", "G/A/A", 5, 127}, instruments, {});

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
    musyx::SongGroup group = MakeSongGroup();

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
    musyx::Instruments instruments(group, SampleVolumes());

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
    musyx::SongGroup group = MakeSongGroup();
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
    musyx::Instruments instruments(group, SampleVolumes());

    constexpr double kShare = 0x2000 / 16383.0;
    const musyx::MacroVoice& voice = instruments.Macro(3);
    Check(voice.volume_controller == kShare && musyx::VoiceVolume(voice, 1.0, kShare) == kShare &&
              voice.approximations.empty(),
          "the macro sets its volume controller to 0x2000 of 0x3fff, and its volume follows");
    std::set<std::string> left_out;
    const std::vector<musyx::EffectVoice> voices = musyx::EffectVoicesOf(instruments, 3, 60, left_out);
    Check(voices.size() == 2 && voices[1].voice.macro == 4 && voices[1].voice.volume == kShare,
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
// replaces at once never sounds. A song's note plays the first sample that sounds. An effect plays each part as a note.
void TestVoiceParts()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group = MakeSongGroup();

    // Sample 1 at -20 cents, then sample 2 at 0 cents at the same moment.
    group.pool.macros[4] = {
        Step(MacroOp::kAddKey, {0, 0xec}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kAddKey, {0, 0}),
        Step(MacroOp::kStartSample, {2}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xff, 0xff}),
        Step(MacroOp::kEnd),
    };

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
    musyx::Instruments instruments(group, SampleVolumes());

    const musyx::MacroVoice& voice = instruments.Macro(3);
    Check(voice.sample == 1 && voice.sample_ms == 0.0 &&
              voice.approximations == std::vector<std::string>{"a second sample of a voice left out"},
          "a song's note plays the first sample and reports the second");
    const musyx::MacroVoice& replaced = instruments.Macro(4);
    Check(replaced.sample == 2 && replaced.detune_cents == 0.0 && replaced.approximations.empty() &&
              replaced.parts.empty(),
          "a sample that starts at the same moment as the first plays in its place");
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
    if (!Check(notes.size() == 2 && notes[0].start_ms == 0.0 && notes[0].voices.size() == 1 &&
                   notes[0].voices[0].part == 1 && notes[1].start_ms == 200.0 && notes[1].voices.size() == 1 &&
                   notes[1].voices[0].part == 2 && left_out.empty(),
               "the effect plays a note for each part"))
    {
        return;
    }

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
    Check(past_the_data == "main.dol has no data at the table of effects",
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

// A curve that stays at its level has no decay, and a rise is an attack.
void TestFitFadesHolds()
{
    const FadeShape flat = FitFades({{0.0, 0.5}});
    Check(flat.peak == 0.5 && flat.attack_ms == 0.0 && flat.decay_db_per_ms == 0.0 && flat.sustain_db == 0.0,
          "a flat curve only holds");

    const FadeShape rise = FitFades({{0.0, 0.0}, {500.0, 1.0}});
    Check(rise.peak == 1.0 && rise.attack_ms == 500.0 && rise.decay_db_per_ms == 0.0, "a rise is the attack");

    const FadeShape silent = FitFades({{0.0, 0.0}});
    Check(silent.peak == 0.0, "a silent curve has no peak");
}

// A line in amplitude to silence starts slowly in decibels. The fit holds, then falls. It follows the line to within a
// decibel at half the line's time, where the line lies 6 dB down, and within 5 dB at 900 ms, where the line lies 20 dB
// down.
void TestFitFadesLine()
{
    const FadeShape line = FitFades({{0.0, 1.0}, {1000.0, 0.0}});
    Check(line.peak == 1.0 && line.attack_ms == 0.0, "the line falls from its peak");
    Check(line.hold_ms > 0.0, "the fit holds first");

    const auto fitted_db = [&line](double ms)
    {
        return (ms - line.hold_ms) * line.decay_db_per_ms;
    };

    Check(std::abs(fitted_db(500.0) - 6.02) < 1.0, "the fit follows the line's middle");
    Check(std::abs(fitted_db(900.0) - 20.0) < 5.0, "the fit follows the line down to 20 dB");
    Check(line.sustain_db < -100.0, "the line ends in silence");

    Check(FadeAmplitude({{0.0, 1.0}, {1000.0, 0.0}}, 250.0) == 0.75, "the curve's level between its points");
    Check(FadeAmplitude({{0.0, 1.0}, {1000.0, 0.0}}, 2000.0) == 0.0, "the curve holds its last level");
}

// A release in a line in amplitude takes the rate that falls 14 dB when the line does.
void TestFitRelease()
{
    const double kFall = 1.0 - std::pow(10.0, -14.0 / 20.0);
    Check(std::abs(FitRelease(1000.0) - 14.0 / (kFall * 1000.0)) < 1e-12, "the rate of a release");
    Check(FitRelease(0.0) > 1000.0, "a release of no time cuts the note");
}

// A decay slower than SoundFont's slowest (100 dB in 101.6 s) falls at the slowest rate after the hold that comes
// closest to the curve. A curve that falls 1 dB in 143 s holds for the longest that holdVolEnv allows. One that drops
// 2 dB first and then falls 1 dB in 143 s doesn't hold. A faster decay keeps the fitted hold.
void TestSlowDecayHold()
{
    using seqlib::sf2::ToTimecents;
    const double kOneDb = std::pow(10.0, -1.0 / 20.0);
    const double kTwoDb = std::pow(10.0, -2.0 / 20.0);
    const FadeShape slow = FitFades({{0.0, 1.0}, {143000.0, kOneDb}});
    const FadeShape drop = FitFades({{0.0, 1.0}, {50.0, kTwoDb}, {143000.0, kTwoDb * kOneDb}});
    const FadeShape fast = FitFades({{0.0, 1.0}, {1000.0, 0.0}});
    seqlib::sf2::ZoneValues slow_zone;
    seqlib::sf2::ZoneValues drop_zone;
    seqlib::sf2::ZoneValues fast_zone;

    SetEnvelope(slow, slow_zone);
    SetEnvelope(drop, drop_zone);
    SetEnvelope(fast, fast_zone);

    Check(slow_zone.hold == 5000 && slow_zone.decay == 8000 && slow_zone.sustain == 10,
          "a slow fall holds for the longest, then falls at the slowest rate");
    Check(drop.soundfont_hold_ms == 0.0 && drop_zone.hold == -12000 && drop_zone.decay == 8000,
          "a quick drop before a slow fall doesn't hold");
    Check(!fast.soundfont_hold_ms && fast_zone.hold == ToTimecents(fast.hold_ms, -12000, 5000),
          "a faster decay keeps the fitted hold");
}

// The rate at which the library plays a sample at a tuning of 1. The library's updates run at kMaxTempo / 60 (13726 /
// 60) a second, not the 240 that its frames of 60 a second would give. The rate is therefore lower than 48000 samples a
// second's figure of 33476.156 Hz.
void TestN64SampleRate()
{
    Check(n64::kUpdatesPerSecond > 228.0 && n64::kUpdatesPerSecond < 229.0, "the port's updates a second");
    Check(std::abs(n64::kSampleRate - 31910.0) < 10.0, "the library's base rate");
}

// A release counts from the level that the note holds at a usual length. An envelope that holds gives a full release.
// One that fades has less to fall, in the same time for a full level.
void TestN64ShapeRelease()
{
    constexpr uint8_t kRelease = 0x90;
    const double kReleaseMs = n64::ReleaseFrames(kRelease) * 1000.0 / n64::kFramesPerSecond;
    const n64::VolumeShape held = n64::ShapeOf({{1, 32767}}, kRelease);
    Check(held.peak == 1.0, "the envelope reaches full level");
    Check(std::abs(held.release_db_per_ms - FitRelease(kReleaseMs)) < 1e-9,
          "a held note's release takes the full time");

    const n64::VolumeShape fades = n64::ShapeOf({{1, 32767}, {100, 0}}, kRelease);
    Check(fades.release_db_per_ms > held.release_db_per_ms, "a faded note's release takes less time");
    Check(n64::ShapeOf({{1, 32767}}, 0).release_db_per_ms == 0.0, "index 0 has no release");
}

// A frame of VADPCM with an empty book: each sample is its nibble times 2 to the frame's scale.
void TestN64Vadpcm()
{
    std::vector<uint8_t> frame(9, 0x12);
    frame[0] = 0x20;
    const std::array<int16_t, 16> book{};
    const std::vector<int16_t> pcm = n64::DecodeVadpcm(frame, book, 4);
    Check(pcm == std::vector<int16_t>{4, 8, 4, 8}, "the frame's samples");
    Check(!ErrorOf([&] { n64::DecodeVadpcm(frame, book, 17); }).empty(), "a frame that ends early fails");
}

// A small game of the N64 library in memory: a piece with one note of one instrument, and its tables.
struct N64Game
{
    std::vector<uint8_t> dol;
    n64::LibraryTables tables;
    std::vector<uint8_t> rom;
};

// A sample's loop in the game of MakeN64Game, and the book's coefficient of the sample before each half. A sample with
// a loop has frames of rising scale. The decoder's history then changes each frame's first sample. An empty state means
// a sample without a loop.
struct N64Loop
{
    uint32_t start = 0;
    std::vector<int16_t> state;
    int16_t coefficient = 0;
};

// The layer's script in the game of MakeN64Game: one note of semitone 39 for 48 ticks.
const std::vector<uint8_t> kN64OneNote = {0x27, 0x30, 0x7f, 0x00, 0xff};

N64Game MakeN64Game(uint8_t release, uint16_t fade_updates = 0, const N64Loop& loop = {},
                    const std::vector<uint8_t>& layer = kN64OneNote)
{
    constexpr uint32_t kBase = 0x80100000;

    // The sequence: the channels take the bank, then the tempo, the volume and a channel. The channel's script and the
    // layer's follow.
    std::vector<uint8_t> sequence = {
        0xd7, 0x00, 0x01, 0xdd, 120,  0xdb, 127,  0x90, 0x00, 0x0d, 0xfd, 0x60, 0xff, // the group
        0xc4, 0xc1, 0x00, 0x88, 0x00, 0x16, 0xfd, 0x60, 0xff,                         // the channel
    };
    sequence.insert(sequence.end(), layer.begin(), layer.end());

    // The bank: an instrument with an envelope, and a sample of 64 samples with its loop and its book.
    BigEndianBytes bank;
    bank.U32(0).U32(0).U32(0x10).PadTo(0x10);
    bank.U8(0).U8(0).U8(127).U8(release).U32(0x30);
    bank.U32(0).F32(0.0f).U32(0x40).F32(1.0f).U32(0).F32(0.0f).PadTo(0x30);
    bank.U16(1).U16(32767);
    if (fade_updates > 0)
    {
        bank.U16(fade_updates).U16(0);
    }
    bank.U16(0xffff).U16(0).PadTo(0x40);
    bank.U32(36).U32(0).U32(0x50).U32(0x80);
    bank.U32(loop.start).U32(64).U32(loop.state.empty() ? 0 : 0xffffffff).U32(0);
    for (const int16_t value : loop.state)
    {
        bank.U16(static_cast<uint16_t>(value));
    }
    bank.PadTo(0x80);
    bank.U32(2).U32(1).PadTo(0x98);
    bank.U16(static_cast<uint16_t>(loop.coefficient)).PadTo(0xa8);
    std::vector<uint8_t> samples;
    for (int f = 0; f < 4; f++)
    {
        samples.push_back(static_cast<uint8_t>(loop.state.empty() ? 0 : (f + 2) << 4));
        samples.insert(samples.end(), 8, 0x11);
    }

    N64Game game;
    game.rom = sequence;
    const uint32_t bank_start = static_cast<uint32_t>(game.rom.size());
    game.rom.insert(game.rom.end(), bank.Data().begin(), bank.Data().end());
    const uint32_t sample_start = static_cast<uint32_t>(game.rom.size());
    game.rom.insert(game.rom.end(), samples.begin(), samples.end());

    BigEndianBytes dol;

    // A table's header holds its count, and its entries follow.
    const auto table = [&dol](std::initializer_list<std::array<uint32_t, 5>> entries)
    {
        const uint32_t address = kBase + static_cast<uint32_t>(dol.Size());
        dol.U16(static_cast<uint32_t>(entries.size())).PadTo(dol.Size() + 14);
        for (const std::array<uint32_t, 5>& e : entries)
        {
            dol.U32(e[0]).U32(e[1]).U8(0).U8(0).U16(e[2]).U16(e[3]).U16(e[4]);
        }

        return address;
    };

    const auto here = [&dol]
    {
        return kBase + static_cast<uint32_t>(dol.Size());
    };

    game.tables.sequences = table({{0, static_cast<uint32_t>(sequence.size()), 0, 0, 0}});
    game.tables.banks = table({{0, static_cast<uint32_t>(bank.Size()), 0x00ff, 0x0100, 0}});
    game.tables.sample_banks = table({{0, static_cast<uint32_t>(samples.size()), 0, 0, 0}});
    game.tables.segments = table({{0, bank_start, 0, 0, 0},
                                  {bank_start, sample_start - bank_start, 0, 0, 0},
                                  {sample_start, static_cast<uint32_t>(samples.size()), 0, 0, 0}});
    game.tables.bank_map = here();
    dol.U16(2).U8(1).U8(0);
    game.tables.cents = here();
    for (int i = 0; i < 256; i++)
    {
        dol.F32(1.0f);
    }
    game.tables.small_cents = here();
    for (int i = 0; i < 256; i++)
    {
        dol.F32(1.0f);
    }
    game.tables.pitches = here();
    for (int i = 0; i < 128; i++)
    {
        dol.F32(static_cast<float>(std::pow(2.0, (i - 39) / 12.0)));
    }
    game.tables.short_velocities = here();
    dol.PadTo(dol.Size() + 16);
    game.tables.short_gate_times = here();
    dol.PadTo(dol.Size() + 16);
    game.tables.default_envelope = here();
    dol.U16(1).U16(32767).U16(0xffff).U16(0);
    game.dol = DolWithData(kBase, dol.Data());

    return game;
}

// The conversion of the piece of `game`, and the errors of its sound data.
struct N64Conversion
{
    Conversion conversion;
    std::vector<std::string> errors;
};

// Convert the piece of `game`. Nothing if the DOL of `game` doesn't read.
std::optional<N64Conversion> ConvertN64(const N64Game& game)
{
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(game.dol);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return std::nullopt;
    }

    n64::SoundData data(*dol, game.tables, game.rom);
    n64::Piece piece;
    piece.name = "piece";
    Conversion c = n64::ConvertPiece(piece, data, {});

    return N64Conversion{std::move(c), data.Errors()};
}

// The piece plays its note on key 60 from tick 0 to tick 48. Its sample plays at the library's base rate at that key,
// and its zone takes the instrument's release.
void TestN64Piece()
{
    constexpr uint8_t kRelease = 0x90;
    const std::optional<N64Conversion> converted = ConvertN64(MakeN64Game(kRelease));
    if (!converted)
    {
        return;
    }

    const Conversion& c = converted->conversion;
    Check(converted->errors.empty(), "the bank reads");

    std::vector<std::pair<EventKind, uint32_t>> notes;
    for (const TrackEvent& e : c.performance.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.kind, e.tick);
            Check(e.key == 60, "semitone 39 is key 60");
        }
    }
    const std::vector<std::pair<EventKind, uint32_t>> kExpected = {{EventKind::kNoteOn, 0}, {EventKind::kNoteOff, 48}};
    Check(notes == kExpected, "the note's start and end");

    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    const uint32_t rate = xsflib::LoadLe32(records.Record("shdr", 46, 0) + 36);
    Check(rate == static_cast<uint32_t>(std::lround(n64::kSampleRate)), "the sample's rate is the library's");

    const double kReleaseMs = n64::ReleaseFrames(kRelease) * 1000.0 / n64::kFramesPerSecond;
    const std::vector<int> releases = SampleZoneValues(c.sf2, seqlib::sf2::kReleaseVolEnv);
    Check(releases == std::vector<int>{seqlib::sf2::ToTimecents(100.0 / FitRelease(kReleaseMs), -12000, 8000)},
          "the zone's release");
    Check(ntdlib::testing::CheckConversion(c, "piece") == 0, "independent readers accept the files");
}

// The piece's note lasts 48 ticks at 120 beats a minute, 500 ms. A fading envelope's release counts from its level
// then.
void TestN64NoteLength()
{
    constexpr uint8_t kRelease = 0x90;
    constexpr uint16_t kFadeUpdates = 200;
    const std::optional<N64Conversion> converted = ConvertN64(MakeN64Game(kRelease, kFadeUpdates));
    if (!converted)
    {
        return;
    }

    const Conversion& c = converted->conversion;

    const n64::VolumeShape shape = n64::ShapeOf({{1, 32767}, {kFadeUpdates, 0}}, kRelease, 500.0);
    const std::vector<int> releases = SampleZoneValues(c.sf2, seqlib::sf2::kReleaseVolEnv);
    Check(releases == std::vector<int>{seqlib::sf2::ToTimecents(100.0 / shape.release_db_per_ms, -12000, 8000)},
          "the zone's release fits the length of its note");
    const n64::VolumeShape usual = n64::ShapeOf({{1, 32767}, {kFadeUpdates, 0}}, kRelease);
    Check(shape.release_db_per_ms > usual.release_db_per_ms, "a long note's release falls from a lower level");
}

// A legato layer with a sweep of mode 5 plays its two notes as one MIDI note. The special sweep (0x85) starts each
// note at the note before and spans 51/256 of the note's length: about 4.7 of its 24 ticks at 120 beats a minute.
// The first note starts at the command's semitone 51 (key 72) and falls to semitone 39 (key 60). The second rises from
// there to semitone 41.
void TestN64LegatoSweep()
{
    const std::vector<uint8_t> kLayer = {
        0xc4,                   // legato
        0xc7, 0x85, 51,   51,   // the sweep: mode 5, special, from semitone 51, time 51
        0x27, 0x18, 0x7f, 0x00, // semitone 39 for 24 ticks
        0x29, 0x18, 0x7f, 0x00, // semitone 41 for 24 ticks
        0xff,
    };
    const std::optional<N64Conversion> converted = ConvertN64(MakeN64Game(0x90, 0, {}, kLayer));
    if (!converted)
    {
        return;
    }

    const Conversion& c = converted->conversion;

    std::vector<std::tuple<EventKind, uint32_t, uint8_t>> notes;
    std::map<uint32_t, float> pitch;
    for (const TrackEvent& e : c.performance.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.kind, e.tick, e.key);
        }
        else if (e.kind == EventKind::kPitch)
        {
            pitch[e.tick] = e.semitones;
        }
    }
    const std::vector<std::tuple<EventKind, uint32_t, uint8_t>> kExpected = {{EventKind::kNoteOn, 0, 72},
                                                                             {EventKind::kNoteOff, 48, 72}};
    Check(notes == kExpected, "the legato notes play as one note from the sweep's start");

    // The pitch at a tick: the last bend sent by then.
    const auto at = [&pitch](uint32_t tick)
    {
        const auto found = pitch.upper_bound(tick);
        return found == pitch.begin() ? 0.0f : std::prev(found)->second;
    };

    Check(at(2) < -1.0f && at(2) > -11.0f, "the first note sweeps down");
    Check(std::abs(at(10) + 12.0f) < 0.01f, "the first sweep ends at semitone 39");
    Check(at(26) > -12.0f && at(26) < -10.0f, "the second note sweeps up from the first");
    Check(std::abs(at(40) + 10.0f) < 0.01f, "the second sweep ends at semitone 41");
    Check(!c.performance.approximations.contains("legato notes played as separate notes"), "no legato note restarts");
    Check(ntdlib::testing::CheckConversion(c, "piece") == 0, "independent readers accept the files");
}

// The start and the end of the loop of the first sample of a conversion of `game`.
std::pair<uint32_t, uint32_t> N64LoopOf(const N64Game& game)
{
    const std::optional<N64Conversion> converted = ConvertN64(game);
    if (!converted)
    {
        return {};
    }

    const seqlib::sf2::Records records = seqlib::sf2::Read(converted->conversion.sf2);
    const uint8_t* const header = records.Record("shdr", 46, 0);

    return {xsflib::LoadLe32(header + 28) - xsflib::LoadLe32(header + 20),
            xsflib::LoadLe32(header + 32) - xsflib::LoadLe32(header + 20)};
}

// A loop's state holds the samples of the frame that holds its start. The decoder goes on from the next frame, with the
// state's last two samples as its history. A state that the first pass gives keeps the loop where the bank has it. Any
// other state gives other samples, and the loop plays once unrolled after the first pass.
void TestN64LoopState()
{
    constexpr int16_t kHalf = 1024;
    std::vector<uint8_t> frames;
    for (int f = 0; f < 4; f++)
    {
        frames.push_back(static_cast<uint8_t>((f + 2) << 4));
        frames.insert(frames.end(), 8, 0x11);
    }
    std::array<int16_t, 16> book{};
    book[8] = kHalf;
    const std::vector<int16_t> pcm = n64::DecodeVadpcm(frames, book, 64);

    N64Loop loop{.start = 16, .state = {pcm.begin() + 16, pcm.begin() + 32}, .coefficient = kHalf};
    Check(N64LoopOf(MakeN64Game(0x90, 0, loop)) == std::pair<uint32_t, uint32_t>{16, 64},
          "a state from the first pass keeps a loop that starts with a frame");
    loop.start = 20;
    Check(N64LoopOf(MakeN64Game(0x90, 0, loop)) == std::pair<uint32_t, uint32_t>{20, 64},
          "a state from the first pass keeps a loop that starts within a frame");

    loop.state.assign(16, 1000);
    Check(N64LoopOf(MakeN64Game(0x90, 0, loop)) == std::pair<uint32_t, uint32_t>{64, 108},
          "another state unrolls the loop once");
}

// A region of a bank of Red, as the bank holds it.
struct RedRecord
{
    uint32_t flags = 0;
    uint32_t sample = 0;
    int32_t loop = -1;
    uint32_t end = 0x1f;
    int32_t pitch = 0;
    int8_t high = 127;
    uint8_t volume = 127;
    uint8_t pan = 0x40;
    std::array<uint16_t, 4> times = {0, 0, 0, 20};
    std::array<uint8_t, 4> levels = {0, 127, 127, 127};
};

// A bank with the records of each instrument, and `sample_bytes` of silence for its samples.
std::vector<uint8_t> RedBank(const std::vector<std::vector<RedRecord>>& instruments, std::size_t sample_bytes)
{
    std::size_t records = 0;
    for (const std::vector<RedRecord>& instrument : instruments)
    {
        records += instrument.size();
    }

    BigEndianBytes b;
    b.U8('W').U8('D').U16(1).U32(static_cast<uint32_t>(sample_bytes)).U32(static_cast<uint32_t>(instruments.size()));
    b.U32(static_cast<uint32_t>(records)).PadTo(0x20);
    std::size_t at = 0x20 + 4 * instruments.size();
    for (const std::vector<RedRecord>& instrument : instruments)
    {
        b.U32(instrument.empty() ? 0 : static_cast<uint32_t>(at));
        at += instrument.size() * 0x60;
    }
    for (const std::vector<RedRecord>& instrument : instruments)
    {
        for (const RedRecord& r : instrument)
        {
            const std::size_t start = b.Size();
            b.U32(r.flags).U32(r.sample).U32(static_cast<uint32_t>(r.loop)).U32(r.end).U32(0);
            b.U32(static_cast<uint32_t>(r.pitch)).U8(static_cast<uint8_t>(r.high)).U8(127).U8(r.volume).U8(r.pan);
            b.PadTo(start + 0x50);
            for (const uint16_t time : r.times)
            {
                b.U16(time);
            }
            for (const uint8_t level : r.levels)
            {
                b.U8(level);
            }
            b.PadTo(start + 0x60);
        }
    }
    b.PadTo(b.Size() + sample_bytes);

    return b.Data();
}

// A piece of Red with volume `volume` and `tracks`, each a list of delta times and commands.
std::vector<uint8_t> RedPiece(int volume, const std::vector<std::vector<uint8_t>>& tracks)
{
    BigEndianBytes b;
    b.U8('B').U8('G').U8('M').U8(' ').U16(1).U16(1).U8(static_cast<uint32_t>(tracks.size())).U8(0).U16(0);
    b.U16(static_cast<uint32_t>(volume)).U16(48).PadTo(0x20);
    for (const std::vector<uint8_t>& track : tracks)
    {
        b.Le32(static_cast<uint32_t>(track.size())).Append(track);
    }

    return b.Data();
}

// A disc that holds these files.
std::shared_ptr<ntdlib::disc::GameFiles> DiscWith(std::map<std::string, std::vector<uint8_t>> files)
{
    auto disc = std::make_shared<ntdlib::disc::GameFiles>();
    for (const auto& [path, data] : files)
    {
        disc->files.push_back({path, data.size()});
    }
    disc->read_file = [held = std::move(files)](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto found = held.find(path);
        if (found == held.end())
        {
            return false;
        }

        data = found->second;

        return true;
    };

    return disc;
}

// Convert `piece`, which plays bank 1 with `bank`, from a disc with the driver's folders.
Conversion ConvertRed(const std::vector<uint8_t>& piece, const std::vector<uint8_t>& bank,
                      const PerformOptions& options = {})
{
    const std::unique_ptr<red::GameInput> game =
        red::GameInput::Open(DiscWith({{"dvd/sound/Music/music100.bgm", piece}, {"dvd/sound/Wave/wave0001.wd", bank}}));
    if (!Check(game != nullptr, "the disc holds a piece of Red"))
    {
        return {};
    }

    return game->Convert(0, options);
}

// The events of `kind` on track `slot`: each one's tick and its value.
std::vector<std::pair<uint32_t, int>> RedEvents(const Conversion& c, std::size_t slot, EventKind kind)
{
    std::vector<std::pair<uint32_t, int>> events;
    for (const TrackEvent& e : EventsOf(c.performance, slot, kind))
    {
        events.emplace_back(e.tick, e.value);
    }

    return events;
}

// The game finds the pieces by their folder, in any case. A disc without them isn't this driver's.
void TestRedOpen()
{
    Check(red::GameInput::Open(DiscWith({{"audio/x.bgm", {}}})) == nullptr, "no folder of pieces, no game");

    const std::unique_ptr<red::GameInput> game = red::GameInput::Open(DiscWith(
        {{"dvd/sound/Music/Music101.bgm", {}}, {"dvd/sound/music/music100.bgm", {1}}, {"dvd/sound/Music/a.txt", {}}}));
    if (Check(game != nullptr, "the folder of pieces names the driver"))
    {
        const std::vector<std::string> kExpected = {"music100", "music101"};
        Check(game->Pieces() == kExpected, "the pieces by their names");
        Check(!ErrorOf([&] { game->Convert(0, {}); }).empty(), "a file that isn't a piece fails");
    }

    const std::vector<uint8_t> piece = RedPiece(127, {});
    const std::unique_ptr<red::GameInput> no_bank =
        red::GameInput::Open(DiscWith({{"dvd/sound/Music/music100.bgm", piece}}));
    Check(
        no_bank != nullptr && ErrorOf([&] { no_bank->Convert(0, {}); }) == "no file on the disc holds the piece's bank",
        "a missing bank fails");
}

// The levels are lines in amplitude, and MIDI's values give squares. The tempo adds up once for each of AX's frames.
// A track starts its first delta time a tick early. A velocity of 0 leaves its factor out.
void TestRedLevels()
{
    const std::vector<uint8_t> bank = RedBank({{{}}}, 16);

    const Conversion c = ConvertRed(
        RedPiece(63, {{0, 0x08, 125, 96, 0x00}, {0, 0x22, 63, 0,  0x26, 0,    0,  0x0a, 32, 0,  0x60, 0,  0x20, 0,
                                                 0, 0x11, 60, 63, 48,   0x18, 12, 0x13, 0,  12, 0x18, 24, 0x00}}),
        bank);

    Check(!c.performance.tempo.empty() && std::abs(c.performance.tempo[0].bpm - 125.0 * 32028.5 / 160.0 / 200.0) < 1e-6,
          "the tempo");
    Check(c.performance.end_tick == 97, "the piece ends with its last track");

    const std::vector<std::pair<uint32_t, int>> kOn = {{0, 90}, {60, 127}};
    Check(RedEvents(c, 1, EventKind::kNoteOn) == kOn, "the notes' starts and velocities");
    const std::vector<std::pair<uint32_t, int>> kOff = {{48, 0}, {72, 0}};
    Check(RedEvents(c, 1, EventKind::kNoteOff) == kOff, "the notes' ends");

    // The piece's volume and the track's are each 0.5, and the pan's total is 1. The left side alone sounds.
    const std::vector<std::pair<uint32_t, int>> expression = RedEvents(c, 1, EventKind::kExpression);
    Check(!expression.empty() && expression.back().second == 64, "CC11 holds the square root of the levels");
    const std::vector<std::pair<uint32_t, int>> pan = RedEvents(c, 1, EventKind::kPan);
    Check(!pan.empty() && pan.back().second == 1, "the pan's left end is CC10's");
    const std::vector<std::pair<uint32_t, int>> reverb = RedEvents(c, 1, EventKind::kReverb);
    Check(!reverb.empty() && reverb.back().second == 32, "the reverb's depth is CC91's after the command turns it on");
    Check(ntdlib::testing::CheckConversion(c, "music100") == 0, "independent readers accept the files");
}

// A key-off cancels a key-on of the tick that's waiting. A key-on after a key-off starts a note. A stop releases only
// the note of the track's key. The others sound on to the end.
void TestRedKeyOffs()
{
    const std::vector<uint8_t> bank = RedBank({{{}}}, 16);

    const Conversion c =
        ConvertRed(RedPiece(127, {{0, 0x08, 120, 48, 0x00}, {0, 0x20, 0,  0,  0x12, 60, 0, 0x18, 10, 0x12, 61,
                                                             0, 0x12, 62, 10, 0x1a, 61, 0, 0x12, 61, 10,   0x00}}),
                   bank);

    const std::vector<std::pair<uint32_t, int>> kOn = {{10, 61}, {10, 62}, {20, 61}};
    Check(KeysOf(c.performance, 1, EventKind::kNoteOn) == kOn,
          "the cancelled note never starts, and the note after a key-off does");
    const std::vector<std::pair<uint32_t, int>> kOff = {{20, 61}, {30, 61}, {49, 62}};
    Check(KeysOf(c.performance, 1, EventKind::kNoteOff) == kOff,
          "a stop releases the track's key, and the other note ends with the piece");
}

// The loop's start saves each track at the end of its tick. The loop's end restores them at the end of its tick, and
// the next tick plays what followed the start. The loop therefore runs from the tick after each.
void TestRedLoop()
{
    const std::vector<uint8_t> bank = RedBank({{{}}}, 16);
    const std::vector<uint8_t> piece = RedPiece(
        127, {{0, 0x08, 120, 24, 0x02, 96, 0x03, 0, 0x00}, {0, 0x20, 0, 30, 0x12, 60, 12, 0x18, 200, 1, 0x00}});

    const Conversion once = ConvertRed(piece, bank);

    const std::optional<Performance::Loop> loop = once.performance.loop;
    if (Check(loop.has_value(), "the piece loops"))
    {
        Check(loop->start == 25 && loop->end == 121, "the loop runs from the tick after each command");
    }
    Check(once.performance.end_tick == 121, "one pass ends at the loop's end");

    PerformOptions twice_options;
    twice_options.loops = 2;

    const Conversion twice = ConvertRed(piece, bank, twice_options);

    Check(twice.performance.end_tick == 217, "a second pass adds the loop's length");
    std::vector<uint32_t> starts;
    for (const auto& [tick, velocity] : RedEvents(twice, 1, EventKind::kNoteOn))
    {
        starts.push_back(tick);
    }
    const std::vector<uint32_t> kExpected = {30, 126};
    Check(starts == kExpected, "the second pass plays the note again");

    const std::vector<uint8_t> no_start = RedPiece(127, {{0, 0x08, 120, 24, 0x03, 0, 0x00}});
    Check(ErrorOf([&] { ConvertRed(no_start, bank); }) == "a piece's loop ends before it starts",
          "a loop needs a start");

    const std::vector<uint8_t> unknown = RedPiece(127, {{0, 0x7e, 0, 0x00}});
    Check(!ErrorOf([&] { ConvertRed(unknown, bank); }).empty(), "a command that dolphinsf2 doesn't read fails");
}

// The conversion of a piece that plays each instrument of `instruments` on a track, with a bank of those instruments
// and `sample_bytes` bytes of samples.
Conversion RedPlaying(const std::vector<std::vector<RedRecord>>& instruments, std::size_t sample_bytes = 256)
{
    std::vector<std::vector<uint8_t>> tracks = {{0, 0x08, 120, 96, 0x00}};
    for (std::size_t i = 0; i < instruments.size(); i++)
    {
        tracks.push_back({0, 0x20, static_cast<uint8_t>(i), 0, 0x12, 60, 48, 0x18, 48, 0x00});
    }

    return ConvertRed(RedPiece(127, tracks), RedBank(instruments, sample_bytes));
}

// Only the first region's flags say that an instrument has splits. A region takes the keys above the region before, up
// to its highest. A region whose highest key lies below them plays nothing. The last region takes the keys above.
void TestRedSplits()
{
    RedRecord first;
    first.flags = 0x10100;
    first.high = 59;
    RedRecord hidden;
    hidden.high = 59;
    RedRecord middle;
    middle.high = 70;
    middle.sample = 16;
    RedRecord last;
    last.flags = 0x10200;
    last.high = 90;
    last.sample = 32;
    RedRecord alone;
    alone.high = 30;
    alone.sample = 48;

    const Conversion c = RedPlaying({{first, hidden, middle, last}, {alone}});

    std::vector<std::pair<int, int>> ranges;
    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (gens.contains(seqlib::sf2::kSampleId))
        {
            const uint16_t range = gens.at(seqlib::sf2::kKeyRange);
            ranges.emplace_back(range & 0xff, range >> 8);
        }
    }
    const std::vector<std::pair<int, int>> kExpected = {{0, 59}, {60, 70}, {71, 127}, {0, 127}};
    Check(ranges == kExpected, "each region's keys");
}

// A stereo wave's records play at the two sides, whatever the track's pan. A fixed pan leaves the track's out too. Its
// zone holds the CC10 modulator at 0, in place of the global zone's. A region of another pan goes by the driver's table
// on a sine law.
void TestRedPans()
{
    RedRecord left;
    left.flags = 0x1;
    RedRecord right;
    right.sample = 16;
    RedRecord fixed;
    fixed.sample = 32;
    fixed.pan = 0x80 | 32;
    RedRecord centre;
    centre.sample = 48;
    centre.pan = 0x80;

    const Conversion c = RedPlaying({{left, right}, {fixed}, {centre}, {}});

    const double kPlace = std::atan2(32.0 * 182.0 / 63.0, 256.0 - 32.0 * 74.0 / 63.0) * 2.0 / std::numbers::pi;
    const std::vector<int> pans = SampleZoneValues(c.sf2, seqlib::sf2::kPan);

    // The centre's zone leaves its pan of 0 out.
    const std::vector<int> kExpected = {-500, 500, static_cast<int>(std::lround((kPlace * 2.0 - 1.0) * 500.0))};
    Check(pans == kExpected, "the zones' pans");

    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    int fixed_zones = 0;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        if (!records.ZoneGens(zone).contains(seqlib::sf2::kSampleId))
        {
            continue;
        }

        for (const std::array<int, 5>& modulator : ZoneModulators(records, zone))
        {
            if (modulator[1] == seqlib::sf2::kPan && modulator[0] == seqlib::sf2::kCc10ToPan.source)
            {
                Check(modulator[2] == 0, "the zone's CC10 modulator does nothing");
                fixed_zones++;
            }
        }
    }
    Check(fixed_zones == 4, "each zone with a fixed pan holds the modulator");
    Check(c.samples == 4, "each record has its sample");
}

// The envelope starts at its first level, and goes to the first stage with a time. A later stage without a time jumps
// to its level. The release falls in its time from any level.
void TestRedEnvelope()
{
    RedRecord slow;
    slow.times = {0, 0, 640, 20};
    slow.levels = {0, 127, 15, 127};

    const Conversion c = RedPlaying({{slow}});

    constexpr double kFrameMs = 160.0 * 1000.0 / 32028.5;
    const std::vector<int> attacks = SampleZoneValues(c.sf2, seqlib::sf2::kAttackVolEnv);
    Check(attacks == std::vector<int>{seqlib::sf2::ToTimecents(640.0 * kFrameMs, -12000, 8000)},
          "the third stage's rise");
    const std::vector<int> releases = SampleZoneValues(c.sf2, seqlib::sf2::kReleaseVolEnv);
    Check(releases == std::vector<int>{seqlib::sf2::ToTimecents(100.0 / FitRelease(20.0 * kFrameMs), -12000, 8000)},
          "the release");
}

// A wave that runs past the end of the bank ends with the bank. A wave that starts past it plays nothing.
void TestRedWaveEnd()
{
    RedRecord past;
    past.end = 0x1000;
    RedRecord outside;
    outside.sample = 0x1000;

    const Conversion c = RedPlaying({{past}, {outside}}, 16);

    Check(c.samples == 1, "the wave that starts inside the bank plays");
    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    const uint8_t* header = records.Record("shdr", 46, 0);
    Check(xsflib::LoadLe32(header + 24) - xsflib::LoadLe32(header + 20) == 28, "the two frames of the bank's 16 bytes");
}

// More tracks than channels: tracks that don't sound together share a channel with no fault. Tracks with other levels
// that sound together do, and the conversion says so.
void TestRedSharedChannels()
{
    const std::vector<uint8_t> bank = RedBank({{{}}}, 16);
    std::vector<std::vector<uint8_t>> apart = {{0, 0x08, 120, 96, 0x00}};
    std::vector<std::vector<uint8_t>> together = {{0, 0x08, 120, 96, 0x00}};
    for (uint8_t t = 0; t < 18; t++)
    {
        // Each track waits 100 ticks more than the last, as a delta time in two bytes.
        const int wait = t * 100;
        apart.push_back({0, 0x20, 0, 0, 0x22, static_cast<uint8_t>(100 + t), static_cast<uint8_t>(0x80 | wait >> 7),
                         static_cast<uint8_t>(wait & 0x7f), 0x12, 60, 12, 0x18, 12, 0x00});
        together.push_back({0, 0x20, 0, 0, 0x22, static_cast<uint8_t>(100 + t), 0, 0x12, static_cast<uint8_t>(40 + t),
                            90, 0x18, 12, 0x00});
    }

    const Conversion separate = ConvertRed(RedPiece(127, apart), bank);

    Check(separate.channels <= 16, "the tracks fit in MIDI's channels");
    const std::string kFault = "tracks with different controllers that share a channel";
    Check(!separate.performance.approximations.contains(kFault), "tracks that sound apart share with no fault");
    int notes = 0;
    for (const std::vector<TrackEvent>& track : separate.performance.tracks)
    {
        notes += static_cast<int>(std::count_if(track.begin(), track.end(),
                                                [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; }));
    }
    Check(notes == 18, "every track's note plays");

    const Conversion crowded = ConvertRed(RedPiece(127, together), bank);

    Check(crowded.channels <= 16, "the tracks fit in MIDI's channels");
    Check(crowded.performance.approximations.contains(kFault),
          "tracks that sound together with other levels are a fault");
    Check(ntdlib::testing::CheckConversion(crowded, "music100") == 0, "independent readers accept the files");
}

} // namespace

int main(int argc, char** argv)
{
    TestMusyxPaths();
    TestMusyxSongLists();
    TestSoundRoom();
    TestSongsInOneFile();
    TestDlsMilliseconds();
    TestPianoPan();
    TestScaleVolume();
    TestMacroLoop();
    TestFadeIn();
    TestMacroVariables();
    TestReleaseFade();
    TestHeldFade();
    TestDecayToSilence();
    TestMusyxSongVolume();
    TestMusyxSongFadeIn();
    TestMusyxTailCut();
    TestMusyxTailUnderNextNote();
    TestMusyxLoopTailCut();
    TestMusyxFadeTailCut();
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
    TestMusyxAttackAfterRelease();
    TestMusyxLaterKeyOff();
    TestMusyxHeldFade();
    TestFadeInHold();
    TestMusyxGlobalZone();
    TestMusyxTempoTable();
    TestMusyxLoopWaitsForPatterns();
    TestMusyxPitchWheelRange();
    TestMusyxMixedPitchWheelRanges();
    TestMusyxNoteOfNoLength();
    TestVolumeSelect();
    TestReverbSelect();
    TestPitchCurve();
    TestSelfRelease();
    TestPanCurve();
    TestHighestPitch();
    TestPanning();
    TestMusyxSongPanning();
    TestMusyxSongSweep();
    TestEnvelopeFromControllers();
    TestSplits();
    TestSplitsInAChord();
    TestPitchWheelSelect();
    TestSetPitch();
    TestSendMessage();
    TestVoiceFrom();
    TestPlayMacro();
    TestVoiceParts();
    TestSetController();
    TestPlayedEffects();
    TestFitFadesHolds();
    TestFitFadesLine();
    TestSlowDecayHold();
    TestFitRelease();
    TestN64SampleRate();
    TestN64ShapeRelease();
    TestN64Vadpcm();
    TestN64Piece();
    TestN64NoteLength();
    TestN64LegatoSweep();
    TestN64LoopState();
    TestRedOpen();
    TestRedLevels();
    TestRedKeyOffs();
    TestRedLoop();
    TestRedSplits();
    TestRedPans();
    TestRedEnvelope();
    TestRedWaveEnd();
    TestRedSharedChannels();

    if (argc > 1)
    {
        TestGameConvertsEverySequence(argv[1]);
    }

    return xsflib::testing::Summary("dolphinsf2");
}
