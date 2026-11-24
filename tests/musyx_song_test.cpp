// SPDX-License-Identifier: MIT

// Unit tests of the MusyX songs: the volume of a song, the tails of the notes, the volume table, pushed groups, sample
// names, tempos, loops and the ranges of the pitch wheel.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "xsflib/testing/check.h"

#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"
#include "musyx_player.h"
#include "musyx_test_helpers.h"
#include "musyx_voices.h"
#include "test_helpers.h"
#include "unit_tests.h"

namespace dolphinsf2::testing
{
namespace
{

using xsflib::testing::Check;

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

// A group of effects plays the objects and samples that it doesn't have from the groups pushed before it. The bank
// keeps its macro 1. Its effect of object 2 plays the pushed group's macro 2 and sample 9, under the pushed group's
// name.
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
// Group 2 contains group 1's sample at another offset, and group 3 contains group 0's. Group 1's sample takes the
// group's index in its name. Its song's SFZ files play that file. Its SoundFont keeps the name "s1".
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

} // namespace

void RunMusyxSongTests()
{
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
    TestMusyxTempoTable();
    TestMusyxLoopWaitsForPatterns();
    TestMusyxPitchWheelRange();
    TestMusyxMixedPitchWheelRanges();
    TestMusyxNoteOfNoLength();
}

} // namespace dolphinsf2::testing
