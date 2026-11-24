// SPDX-License-Identifier: MIT

// Unit tests of the conversion of the Nintendo 64's sound library: its rates, envelopes and VADPCM, and pieces that
// small games built in memory play.

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/dol.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "fade_fit.h"
#include "jaudio.h"
#include "n64_convert.h"
#include "n64_player.h"
#include "n64_sound.h"
#include "test_helpers.h"
#include "unit_tests.h"

namespace dolphinsf2::testing
{
namespace
{

using ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

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

// The bank of the game of MakeN64Game: an instrument with an envelope and the release `release`, and a sample of 64
// samples with its loop and its book. With `fade_updates`, the envelope fades to silence in that many updates.
std::vector<uint8_t> N64BankBytes(uint8_t release, uint16_t fade_updates, const N64Loop& loop)
{
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

    return bank.Data();
}

// The library's tables of the game of MakeN64Game in a DOL's data at `base`, for a ROM that contains the sequence, then
// the bank from `bank_start` and the samples from `sample_start` to `rom_size`.
std::vector<uint8_t> N64TableData(uint32_t base, uint32_t bank_start, uint32_t sample_start, uint32_t rom_size,
                                  n64::LibraryTables& tables)
{
    BigEndianBytes dol;

    // A table's header contains its count, and its entries follow.
    const auto table = [&](std::initializer_list<std::array<uint32_t, 5>> entries)
    {
        const uint32_t address = base + static_cast<uint32_t>(dol.Size());
        dol.U16(static_cast<uint32_t>(entries.size())).PadTo(dol.Size() + 14);
        for (const std::array<uint32_t, 5>& e : entries)
        {
            dol.U32(e[0]).U32(e[1]).U8(0).U8(0).U16(e[2]).U16(e[3]).U16(e[4]);
        }

        return address;
    };

    const auto here = [&]
    {
        return base + static_cast<uint32_t>(dol.Size());
    };

    tables.sequences = table({{0, bank_start, 0, 0, 0}});
    tables.banks = table({{0, sample_start - bank_start, 0x00ff, 0x0100, 0}});
    tables.sample_banks = table({{0, rom_size - sample_start, 0, 0, 0}});
    tables.segments = table({{0, bank_start, 0, 0, 0},
                             {bank_start, sample_start - bank_start, 0, 0, 0},
                             {sample_start, rom_size - sample_start, 0, 0, 0}});
    tables.bank_map = here();
    dol.U16(2).U8(1).U8(0);
    tables.cents = here();
    for (int i = 0; i < 256; i++)
    {
        dol.F32(1.0f);
    }

    tables.small_cents = here();
    for (int i = 0; i < 256; i++)
    {
        dol.F32(1.0f);
    }

    tables.pitches = here();
    for (int i = 0; i < 128; i++)
    {
        dol.F32(static_cast<float>(std::pow(2.0, (i - 39) / 12.0)));
    }

    tables.short_velocities = here();
    dol.PadTo(dol.Size() + 16);
    tables.short_gate_times = here();
    dol.PadTo(dol.Size() + 16);
    tables.default_envelope = here();
    dol.U16(1).U16(32767).U16(0xffff).U16(0);

    return dol.Data();
}

// A game whose piece plays `layer` on one channel with the instrument of N64BankBytes.
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

    // The samples: four frames, of rising scale when the sample loops.
    std::vector<uint8_t> samples;
    for (int f = 0; f < 4; f++)
    {
        samples.push_back(static_cast<uint8_t>(loop.state.empty() ? 0 : (f + 2) << 4));
        samples.insert(samples.end(), 8, 0x11);
    }

    N64Game game;
    game.rom = sequence;
    const uint32_t bank_start = static_cast<uint32_t>(game.rom.size());
    const std::vector<uint8_t> bank = N64BankBytes(release, fade_updates, loop);
    game.rom.insert(game.rom.end(), bank.begin(), bank.end());
    const uint32_t sample_start = static_cast<uint32_t>(game.rom.size());
    game.rom.insert(game.rom.end(), samples.begin(), samples.end());
    game.dol = DolWithData(
        kBase, N64TableData(kBase, bank_start, sample_start, static_cast<uint32_t>(game.rom.size()), game.tables));

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

// A loop's state contains the samples of the frame that contains its start. The decoder goes on from the next frame,
// with the state's last two samples as its history. A state that the first pass gives keeps the loop where the bank has
// it. Any other state gives other samples, and the loop plays once unrolled after the first pass.
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

} // namespace

void RunN64Tests()
{
    TestN64SampleRate();
    TestN64ShapeRelease();
    TestN64Vadpcm();
    TestN64Piece();
    TestN64NoteLength();
    TestN64LegatoSweep();
    TestN64LoopState();
}

} // namespace dolphinsf2::testing
