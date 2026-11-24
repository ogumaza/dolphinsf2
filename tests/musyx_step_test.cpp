// SPDX-License-Identifier: MIT

// Unit tests of the SoundMacro steps that select volumes, reverbs, pitches and pans, of splits, and of the steps that
// play other macros.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/testing/check.h"

#include "jaudio.h"
#include "musyx_convert.h"
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

// VolumeSelect picks the sources of a voice's volume. Byte 1 names the controller, and bytes 2 and 3 give its share in
// percent. Byte 6 adds 256ths of a percent. Byte 4 picks how the source joins the others: 0 replaces them. The voice's
// first VolumeSelect replaces the source that MusyX starts it on. Byte 4 changes nothing there. A share of the volume
// controller (CC7) that replaces the others scales the voice's volume. MusyX keeps the controller's value times the
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

// A song's voice takes a macro's Panning: its region pans hard left. A song's channel contains the volume controller
// that a macro sets. A song's voice leaves that out and reports it. The rest of the song's files stays as it is without
// the two steps. An effect's voice takes both.
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
// pattern's events in their order. A note's preset comes after the notes before it on the tick.
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

} // namespace

void RunMusyxStepTests()
{
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
}

} // namespace dolphinsf2::testing
