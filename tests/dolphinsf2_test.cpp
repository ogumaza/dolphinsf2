// SPDX-License-Identifier: MIT

// Unit tests for dolphinsf2, without game data. The tests build small MusyX song groups in memory. ntdlib's tests cover
// the conversion of JAudio's sound data.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/dol.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "disc.h"
#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"
#include "musyx_player.h"
#include "musyx_voices.h"

namespace
{

using namespace dolphinsf2;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;
using xsflib::testing::failures;

namespace files = ntdlib::testing::jaudio_files;
using files::Bytes;

// Count `perf`'s events of `kind` on MIDI track `slot`.
int Count(const Performance& perf, std::size_t slot, EventKind kind)
{
    int n = 0;
    for (const TrackEvent& e : perf.tracks[slot])
    {
        n += e.kind == kind ? 1 : 0;
    }

    return n;
}

std::vector<uint8_t> DolWithData(uint32_t address, const std::vector<uint8_t>& data)
{
    constexpr std::size_t kHeader = 0x100;
    std::vector<uint8_t> dol(kHeader);
    xsflib::StoreBe32(&dol[0x1c], static_cast<uint32_t>(kHeader));
    xsflib::StoreBe32(&dol[0x64], address);
    xsflib::StoreBe32(&dol[0xac], static_cast<uint32_t>(data.size()));
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
    Bytes b;
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
    Check(past_the_data == "main.dol has no data at the song list's address",
          "a list past the DOL's data fails: " + past_the_data);
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

// A song group with one program. Program 0 plays macro 1. The macro picks the DLS volume curve without ITD, sets DLS
// envelope 2, starts sample 1, waits for the note's release and releases the envelope. Envelope 2 starts at full level
// at once, holds it and releases over 100 ms. Sample 1 is a PCM16 sine of 3200 samples at 32000 Hz, at key 60, that
// loops from sample 800 to its end. Song 5's MIDI setup gives channel 0 a volume of 100.
musyx::SongGroup TestSongGroup()
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
    Bytes data;
    for (uint32_t i = 0; i < sample.length; i++)
    {
        data.U16(static_cast<uint16_t>(std::lround(8000.0 * std::sin(i * 2.0 * std::numbers::pi / 80.0))));
    }
    group.sample_data = data.Data();

    return group;
}

// A song of one track on channel 0. The track plays pattern 0 at tick 0 with a volume of 100, then loops back to it or
// ends at tick 768. The pattern plays key 60 for half a beat. With `volume_change`, it then sets the channel's volume
// to 64.
ntdlib::musyx::Song TestSong(bool loops, bool volume_change)
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

// The values of the CC7 events of channel 0.
std::vector<int> VolumesOf(const Performance& perf)
{
    std::vector<int> volumes;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volumes.push_back(e.value);
        }
    }

    return volumes;
}

// The game's song table gives each song a volume. MusyX multiplies the channels' volumes by it inside the volume curve.
// Each CC7 then takes the song's volume in 127ths: the MIDI setup's 100, the track entry's 100 and the pattern's 64. A
// game's level multiplies them too.
void TestMusyxSongVolume()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group);
    const ntdlib::musyx::Song song = TestSong(false, true);

    const Performance full = musyx::PlaySong(song, {"full", "G/A/A", 5, 127}, instruments, {});
    const Performance quieter = musyx::PlaySong(song, {"quieter", "G/A/A", 5, 90}, instruments, {});
    const Performance lower = musyx::PlaySong(song, {"lower", "G/A/A", 5, 127, 0.8064}, instruments, {});
    const Performance both = musyx::PlaySong(song, {"both", "G/A/A", 5, 90, 0.8064}, instruments, {});

    Check(VolumesOf(full) == std::vector<int>{100, 64}, "a song at 127 keeps its channels' volumes");
    Check(VolumesOf(quieter) == std::vector<int>{71, 45}, "a song at 90 scales them by 90/127, rounded");
    Check(VolumesOf(lower) == std::vector<int>{81, 52}, "a game's level of 0.8064 scales them by that factor");
    Check(VolumesOf(both) == std::vector<int>{57, 37}, "the song's volume and the game's level multiply");
}

// A song that loops plays at the output's rate, marks its loop and converts to files that independent readers accept.
void TestMusyxSong()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group);

    const Conversion c = musyx::ConvertSong(TestSong(true, false), {"loop", "G/A/A", 5, 127}, instruments, {});

    const Performance& perf = c.performance;
    const double bpm = 120.0 * musyx::kOutputRate / musyx::kNominalRate;
    Check(!perf.tempo.empty() && std::abs(perf.tempo[0].bpm - bpm) < 0.01,
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

// A tempo table starts again in each pass. The run goes on past the end to find the loop, but the performance keeps
// no tempo change after the end. The table here slows the song to 60 beats a minute halfway through each pass.
void TestMusyxTempoTable()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group);
    ntdlib::musyx::Song song = TestSong(true, false);
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

// Convert every sequence of the game at `path`, a start-up archive or a disc image, and validate each file.
void TestGameConvertsEverySequence(const char* path)
{
    std::optional<Game> game;
    try
    {
        const std::filesystem::path p(path);
        const std::string ext = p.extension().string();
        game.emplace(ext == ".baa" || ext == ".aaf" ? Game::FromFolder(p, kProgram) : OpenDiscGame(p));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: %s\n", path, e.what());
        failures++;
        return;
    }

    BankSet banks(game->Archive(), [&](const std::string& name) { return game->ReadWaveArchive(name); });
    int looping = 0;
    for (const SequenceSound& sound : game->Sequences())
    {
        Conversion c;
        try
        {
            c = ConvertSequence(sound, banks, {}, kProgram);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", sound.name.c_str(), e.what());
            failures++;
            continue;
        }

        failures += ntdlib::testing::CheckConversion(c, sound.name);
        looping += c.performance.loop ? 1 : 0;
    }

    std::printf("%s: %zu sequences converted and checked, %d loop\n", path, game->Sequences().size(), looping);
}

} // namespace

int main(int argc, char** argv)
{
    TestMusyxPaths();
    TestMusyxSongLists();
    TestDlsMilliseconds();
    TestMusyxSongVolume();
    TestMusyxSong();
    TestMusyxTempoTable();
    if (argc > 1)
    {
        TestGameConvertsEverySequence(argv[1]);
    }
    if (failures == 0)
    {
        std::puts("all tests passed");
    }

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
