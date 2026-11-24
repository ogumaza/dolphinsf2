// SPDX-License-Identifier: MIT

// Unit tests of the MusyX zones: their attenuation, the volume that a voice adds, releases, fades and the global zone.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
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

// A zone's initialAttenuation generator contains 2.5 times the zone's attenuation. FluidSynth, BASSMIDI and the E-mu
// chips apply 0.4 of it. Program 1 plays a copy of the macro with a ScaleVolume of 64/127. On the DLS curve, the zone
// of the copy then plays at (64/127)²: 119.05 cB down. Its generator contains 298 cB. The zone of program 0 plays at
// full level and gets no attenuation.
void TestMusyxZoneAttenuation()
{
    const Conversion c = ConvertScaledCopy(64, "quiet");

    const SampleZoneAttenuation a = AttenuationOf(c.sf2);

    Check(a.zones == 2, "each program has a zone: " + std::to_string(a.zones));
    Check(a.generators == std::vector<int>{298}, "only the zone of the quieter macro gets an attenuation: 298 cB");
    Check(a.modulators.empty(), "no zone has a modulator to initialAttenuation");
    Check(ntdlib::testing::CheckConversion(c, "quiet") == 0, "independent readers accept the files");
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
// voice then plays at the table's level of 20/127 at any velocity. Its zone has that attenuation, and a velocity
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

// Each instrument's global zone contains seqlib's kGlobalZoneModulators and a modulator from CC1 to the vibrato's
// depth. The set's CC10 modulator has an amount of 500 and replaces the default one of 1000. The CC1 modulator has an
// amount of 0 and replaces the default one of 50 cents.
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
        expected.push_back(ModulatorFields(m));
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

} // namespace

void RunMusyxZoneTests()
{
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
}

} // namespace dolphinsf2::testing
