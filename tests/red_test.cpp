// SPDX-License-Identifier: MIT

// Unit tests of the conversion of Square's Red driver, with pieces and banks built in memory.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "fade_fit.h"
#include "jaudio.h"
#include "red_music.h"
#include "test_helpers.h"
#include "unit_tests.h"

namespace dolphinsf2::testing
{
namespace
{

using ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// A region of a bank of Red, as the bank stores it.
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

// A disc that contains these files.
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

// Convert `piece` from a disc with the driver's folders. The piece plays bank 1, with the data `bank`.
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
// to its highest. A region whose highest key is below them plays nothing. The last region takes the keys above.
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
// zone contains the CC10 modulator at 0, in place of the global zone's. A region of another pan goes by the driver's
// table on a sine law.
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

void RunRedTests()
{
    TestRedOpen();
    TestRedLevels();
    TestRedKeyOffs();
    TestRedLoop();
    TestRedSplits();
    TestRedPans();
    TestRedEnvelope();
    TestRedWaveEnd();
    TestRedSharedChannels();
}

} // namespace dolphinsf2::testing
