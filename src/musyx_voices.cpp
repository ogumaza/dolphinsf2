// SPDX-License-Identifier: MIT

// MusyX's voices (see musyx_voices.h).
//
// A voice runs its macro's steps until a step waits. A wait's flags say when the wait ends: after a time, when the note
// is released, or when the sample ends. A trap sends the voice to another step when the note is released. FollowMacro
// runs the steps three times on a clock in milliseconds: for a note that the player holds, for a note that the player
// releases after every wait of a fixed time, and for a note that the player releases 1 ms after its start. The first
// run gives the voice's settings and any stop of its own. The second gives what the release does. The third checks that
// an early release does the same.

#include "musyx_voices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "xsflib/binary.h"

#include "musyx_game.h"

namespace dolphinsf2::musyx
{
namespace
{

using ntdlib::musyx::DlsAdsr;
using ntdlib::musyx::KindOf;
using ntdlib::musyx::LinearAdsr;
using ntdlib::musyx::MacroOp;
using ntdlib::musyx::MacroStep;
using ntdlib::musyx::ObjectKind;
using ntdlib::musyx::Pool;

constexpr double kNever = std::numeric_limits<double>::infinity();

// The steps that a run follows at most. A loop that never ends would run for ever.
constexpr int kMostSteps = 4096;

// MusyX runs the voices' macros once a millisecond, five times in each frame of 160 samples (0x80162318 in Pokémon
// Colosseum). Each run follows at most 32 steps of a voice (0x80155a68). Steps that don't wait therefore take a
// millisecond for each 32 of them.
constexpr int kStepsPerRun = 32;
constexpr double kRunMs = 1.0;

// The count of a Loop step that never runs out (0x801529a4).
constexpr uint16_t kLoopForever = 0xffff;

// The bits of a step's byte that name one of the 32 variables.
constexpr uint8_t kVariableMask = 0x1f;

// The depth of the layers and keymaps that a note goes through at most. A layer can start a layer.
constexpr int kMostLevels = 8;

// The voices that MusyX has, and so the most that an effect can play.
constexpr std::size_t kMostEffectVoices = 64;

// MIDI's volume controller, the one source of a voice's volume that dolphinsf2 follows (VolumeSelect).
constexpr uint8_t kVolumeController = 7;

// MIDI's controllers of the sends to aux A and aux B. A macro can set them (SetVariable).
constexpr uint8_t kReverbController = 91;
constexpr uint8_t kChorusController = 93;

// A controller's full value in a macro's steps. The steps hold 14 bits.
constexpr uint16_t kFullController = 0x3fff;

// The centre of a signed source of a voice's inputs, from 0 to 1: 0x2000 of 0x3fff. A variable is such a source.
constexpr double kSignedCentre = 0x2000 / static_cast<double>(kFullController);

// The controllers that MusyX takes as signed sources: the mod wheel and the pan.
constexpr uint8_t kModWheel = 1;
constexpr uint8_t kPan = 10;

// The controllers of a select step that stand for the channel's pitch wheel and the voice's two LFOs. MusyX turns 0x82
// and 0x83 into 0xA0 and 0xA1 (inpTranslateExCtrl, 0x80161D20 in Pokémon Colosseum).
constexpr uint8_t kPitchWheelControl = 0x80;
constexpr uint8_t kLfo0Control = 0x82;
constexpr uint8_t kLfo1Control = 0x83;

// The most sources that an input of a voice holds (inpAddCtrl, 0x80160F10 in Pokémon Colosseum).
constexpr std::size_t kMostSources = 4;

// The share that a select step `s` (VolumeSelect and the like) gives its source. Bytes 2 and 3 hold the share in
// percent, a signed number. Byte 6 holds a finer part in 256ths of a percent, with the share's sign. MusyX keeps the
// share in 65536ths. Its rounding moves the share by less than 1/65536.
double SelectShare(const MacroStep& s)
{
    const double percent = static_cast<int16_t>(s.Half(2));
    const double fine = static_cast<int8_t>(s.Byte(6)) / 256.0;

    return (percent < 0.0 ? percent - fine : percent + fine) / 100.0;
}

// The approximation that a voice's report gives for the controllers that its macro sets and dolphinsf2 leaves out.
constexpr char kControllersLeftOut[] = "controllers other than the volume and the sends that a macro sets left out";

// A message to this macro reaches no voice (SendMessage).
constexpr uint16_t kNoMacro = 0xffff;

// A wait's time that never ends.
constexpr uint16_t kWaitForever = 0xffff;

// The milliseconds of a tick at 120 beats a minute: 1000 / 768. dolphinsf2 takes that tempo for every time in ticks.
constexpr double kTickMs = 1000.0 / 768.0;

// MusyX updates a voice's pitch and pan every 15 ms (0x8014b74c in Pokémon Colosseum). It counts time in 256ths of a
// millisecond. The DSP plays a pitch up to 0x3fff (0x80162944).
constexpr double kUpdateMs = 15.0;
constexpr int64_t kTimeUnitsPerMs = 256;
constexpr double kHighestPitch = 0x3fff;

// The DSP's pitch of a voice with settings `s` that starts with `key` on `sample`, in 4096ths of the output's rate.
double DspPitch(const PitchSettings& s, const Sample& sample, int key)
{
    if (s.rate_hz && *s.rate_hz > 0.0)
    {
        return *s.rate_hz * 4096.0 / kNominalRate;
    }

    const int voice_key = s.fixed_key.value_or(key + s.key_shift);
    return sample.pcm.sample_rate * std::exp2((voice_key - sample.key) / 12.0 + s.detune_cents / 1200.0) * 4096.0 /
           kNominalRate;
}

// The trap of a note's release, among a voice's three traps.
constexpr uint8_t kReleaseTrap = 0;

// The timecents of a DLS envelope's time in its table. The table holds 65536ths of a timecent. The table's lowest
// value stands for no time.
double TableTimecents(int32_t value)
{
    return value == INT32_MIN ? -1.0e9 : value / 65536.0;
}

// The timecents of a DLS envelope's scale in its table, for each unit of the velocity or the key. MusyX multiplies the
// scale by the velocity or the key over 128. The table's lowest value turns the scale off.
double ScaleTimecents(int32_t value)
{
    return value == INT32_MIN ? 0.0 : value / (128.0 * 65536.0);
}

// The steps of 0.5 dB below full level that MusyX gives an amplitude of `amplitude` 1023rds (its table at 0x8036944C in
// Pokémon Colosseum): the nearest step. A step that falls halfway can come out one lower than the table's.
int SustainSteps(uint32_t amplitude)
{
    const uint32_t scaled = std::min<uint32_t>(amplitude, 0x3ff);
    if (scaled == 0)
    {
        return 193;
    }

    return std::clamp(static_cast<int>(std::lround(-40.0 * std::log10(scaled / 1023.0))), 0, 193);
}

// The steps of 0.5 dB below full level of a DLS envelope's sustain, whose table gives `level`. MusyX squares the level
// in 128ths, then finds the nearest step of its amplitude in 1023rds.
int DlsSustainSteps(uint16_t level)
{
    const int x = std::min(level >> 5, 127);
    const auto amplitude = static_cast<uint32_t>(4096.0f * static_cast<float>(x * x) / (127.0f * 127.0f));
    return SustainSteps(amplitude >> 2);
}

// A volume from 0 to 127 through `curve`, a table of the pool. MusyX takes the table's byte at the volume's whole part
// and moves toward the next byte by the fraction. From 127 up, the volume takes the byte at 127. MusyX reads whatever
// table holds the ID, an envelope's included (the envelope set-up of FadeIn, 0x80153c10 in Pokémon Colosseum). Return
// nothing where the table ends before the bytes that MusyX reads.
std::optional<double> CurveVolume(const std::vector<uint8_t>& curve, double volume)
{
    const double v = std::clamp(volume, 0.0, 127.0);
    const auto whole = static_cast<std::size_t>(v);
    if (whole >= 127)
    {
        return curve.size() > 127 ? std::optional<double>(curve[127]) : std::nullopt;
    }
    if (whole + 1 >= curve.size())
    {
        return std::nullopt;
    }

    return curve[whole] + (v - static_cast<double>(whole)) * (curve[whole + 1] - curve[whole]);
}

// Table `id` of `pool` as an envelope, in the DLS form or the linear form. Return nothing if the table is missing or
// short.
std::optional<Envelope> EnvelopeOf(const Pool& pool, uint16_t id, bool dls)
{
    const auto table = pool.tables.find(id);
    if (table == pool.tables.end())
    {
        return std::nullopt;
    }

    Envelope e;
    e.dls = dls;
    if (dls)
    {
        const std::optional<DlsAdsr> a = ntdlib::musyx::ReadDlsAdsr(table->second);
        if (!a)
        {
            return std::nullopt;
        }

        e.attack_timecents = TableTimecents(a->attack);
        e.decay_timecents = TableTimecents(a->decay);
        e.attack_ms = DlsMilliseconds(e.attack_timecents);
        e.decay_ms = DlsMilliseconds(e.decay_timecents);
        e.sustain = std::pow(10.0, -DlsSustainSteps(a->sustain) * 0.5 / 20.0);
        e.release_ms = a->release;
        e.attack_tc_per_velocity = ScaleTimecents(a->velocity_to_attack);
        e.decay_tc_per_key = ScaleTimecents(a->key_to_decay);
    }
    else
    {
        const std::optional<LinearAdsr> a = ntdlib::musyx::ReadLinearAdsr(table->second);
        if (!a)
        {
            return std::nullopt;
        }

        e.attack_ms = a->attack;
        e.decay_ms = a->decay;
        e.sustain = std::min(a->sustain * 8, 0x7fff) / 32767.0;
        e.release_ms = a->release;
    }

    return e;
}

// MusyX's times in milliseconds for a controller's value from 0 to 127 (0x80368A9C in Pokémon Colosseum, 0x8032FA4C in
// Star Fox Adventures). The value 127 gives no time.
constexpr std::array<uint32_t, 128> kControllerTimes{
    0,      10,     20,     30,     40,     50,     60,     70,     80,     90,     100,    110,    110,
    120,    130,    140,    150,    160,    170,    190,    200,    220,    230,    250,    270,    290,
    310,    330,    350,    380,    410,    440,    470,    500,    540,    580,    620,    660,    710,
    760,    820,    880,    940,    1000,   1000,   1100,   1200,   1300,   1400,   1500,   1600,   1700,
    1800,   2000,   2100,   2300,   2400,   2600,   2800,   3000,   3200,   3500,   3700,   4000,   4300,
    4600,   4900,   5300,   5700,   6100,   6500,   7000,   7500,   8100,   8600,   9300,   9900,   10000,
    11000,  12000,  13000,  14000,  15000,  16000,  17000,  18000,  19000,  21000,  22000,  24000,  26000,
    28000,  30000,  32000,  34000,  37000,  39000,  42000,  45000,  49000,  50000,  55000,  60000,  65000,
    70000,  75000,  80000,  85000,  90000,  95000,  100000, 105000, 110000, 115000, 120000, 125000, 130000,
    135000, 140000, 145000, 150000, 155000, 160000, 165000, 170000, 175000, 180000, 0};

// The controller whose value MusyX keeps for controller `controller` of a song's channel. Below 64, the controllers
// come in pairs of 14 bits. A controller below 32 holds the high 7 bits, and the one 32 above it the low 7. MusyX
// reads both through the lower one.
uint8_t KeptController(uint8_t controller)
{
    return controller < 0x40 ? controller & 0x1f : controller;
}

// The value from 0 to 127 that MusyX reads for controller `controller` of a song's channel, whose controllers hold
// `controls`: the high 7 bits of the value that inpGetMidiCtrl returns. A controller that `controls` lacks holds its
// value after a cold reset of the channel. The switches from 64 to 69 read as 0 or 127, and controllers 96 to 101 as 0.
int ControllerValue(const ChannelControls& controls, uint8_t controller)
{
    if (controller >= 0x60 && controller < 0x66)
    {
        return 0;
    }

    const uint8_t kept = KeptController(controller);
    const auto it = std::ranges::find(controls, kept, &std::pair<uint8_t, uint8_t>::first);
    int value = 0;
    if (it != controls.end())
    {
        value = it->second;
    }
    else
    {
        // MusyX's cold reset: CC7 and CC11 at 127, CC10 and the high bits of the pitch wheel and of controller 132
        // at 64, the rest at 0.
        value = kept == 7 || kept == 11 ? 127 : kept == 10 || kept == 0x80 || kept == 0x84 ? 64 : 0;
    }
    if (controller >= 0x40 && controller < 0x46)
    {
        return value < 0x40 ? 0 : 127;
    }

    return value;
}

} // namespace

int StartVolume(int velocity, double share)
{
    return std::clamp(static_cast<int>(velocity * share + 1e-9), 0, 127);
}

ChannelControls VoiceControls(const ChannelControls& controls, double share)
{
    ChannelControls voice = controls;
    for (auto& [controller, value] : voice)
    {
        if (controller == kVelocityControl)
        {
            value = static_cast<uint8_t>(StartVolume(value, share));
        }
    }

    return voice;
}

namespace
{

// The DLS envelope of a SetAdsrFromControllers step whose controllers have the values `attack`, `decay`, `sustain` and
// `release` (0x801534CC in Pokémon Colosseum). The attack, decay and release take their times from kControllerTimes.
// The sustain takes the volume on the DLS curve: the value squared, over 127 squared. The DSP keeps the low 16 bits of
// each time (hwSetADSR, 0x801627DC).
Envelope ControllerEnvelope(int attack, int decay, int sustain, int release)
{
    Envelope e;
    e.dls = true;
    e.times_in_ms = true;
    e.attack_ms = kControllerTimes[attack] & 0xffff;
    e.decay_ms = kControllerTimes[decay] & 0xffff;
    e.release_ms = kControllerTimes[release] & 0xffff;
    const float volume = static_cast<float>(sustain * sustain) / (127.0f * 127.0f);
    e.sustain = std::pow(10.0, -SustainSteps(static_cast<uint32_t>(1023.0f * volume)) * 0.5 / 20.0);

    return e;
}

// The approximation that a voice's report gives for the samples after its first.
constexpr char kLaterSamplesLeftOut[] = "a second sample of a voice left out";

// What a run of a macro's steps found.
struct Run
{
    MacroVoice voice;
    std::optional<double> release_at; // the first KeyOff, in milliseconds after the note's start
    std::optional<double> stop_at;    // the first StopSample
    std::vector<VolumeRamp> ramps;    // the Envelope steps, each from its time after the note's start
    bool sample_started = false;
    bool envelope_before_sample = false;
    std::vector<MacroVoice> parts; // a part for each sample, in order (MacroVoice::parts)
};

// The wait of a step: its time, in ticks or milliseconds, and what else ends it.
struct Wait
{
    uint16_t time = 0;
    bool ms = false;
    bool absolute = false;      // the time counts from the note's start, not from the step
    bool on_release = false;    // the note's release ends the wait
    bool on_sample_end = false; // the sample's end ends the wait
    bool random = false;        // MusyX waits a random time, up to `time`
};

// The wait of WaitTicks or WaitMs step `s`.
Wait WaitOf(const MacroStep& s)
{
    return {.time = s.Half(6),
            .ms = s.Op() == MacroOp::kWaitMs || (s.Byte(5) & 1) != 0,
            .absolute = (s.Byte(4) & 1) != 0,
            .on_release = (s.Byte(1) & 1) != 0,
            .on_sample_end = (s.Byte(3) & 1) != 0,
            .random = (s.Byte(2) & 1) != 0};
}

// The wait of AddKey or SetKey step `s`. The key commands wait without flags.
Wait KeyWaitOf(const MacroStep& s)
{
    return {.time = s.Half(6), .ms = (s.Byte(5) & 1) != 0, .absolute = (s.Byte(4) & 1) != 0};
}

// A run of a macro's steps for a note that the player releases `release_ms` after its start: kNever for a note that it
// holds. ScaleVolume divides by `volume_scale_divisor` (SongGroup).
class MacroRun
{
public:
    MacroRun(const Pool& pool, double release_ms, int volume_scale_divisor, const ChannelControls& controls)
        : pool_(pool),
          release_ms_(release_ms),
          volume_scale_divisor_(volume_scale_divisor),
          controls_(controls),
          released_(release_ms <= 0.0)
    {
    }

    // Run macro `macro` from its first step. A macro that the pool lacks starts nothing.
    Run Follow(uint16_t macro);

private:
    // Whether the run goes on after a step.
    enum class Next
    {
        kStep,
        kEnd,
    };

    // A step in a macro.
    struct Place
    {
        const std::vector<MacroStep>* steps = nullptr;
        std::size_t at = 0;
    };

    // The place of a Loop step's jump, and the waits by then.
    struct LoopJump
    {
        const std::vector<MacroStep>* steps = nullptr;
        std::size_t at = 0;
        int waits = 0;

        bool operator==(const LoopJump&) const = default;
    };

    // A sample and its start.
    struct OpenPart
    {
        uint16_t sample = 0;
        double start_ms = 0.0;
    };

    // A source of the voice's pitch wheel and the way it joins those before it.
    struct WheelSource
    {
        InputSource source;
        uint8_t combine = 0;
    };

    // The most Gosub steps whose Return a run keeps.
    static constexpr std::size_t kMostCalls = 4;

    // Run step `s`.
    Next Step(const MacroStep& s);

    // Go on at step `step` of macro `macro`. Return false if the pool lacks the macro.
    bool Jump(uint16_t macro, uint16_t step);

    // Release the note. The run goes on where the trap of the release points.
    void TakeTrap();

    // Follow Loop step `s`.
    Next Loop(const MacroStep& s);

    // Follow FadeIn step `s`.
    void FadeIn(const MacroStep& s);

    // The value of the variable that byte `at` + 1 of step `s` names, or nothing for a controller (byte `at` set).
    std::optional<int32_t> VariableAt(const MacroStep& s, int at) const;

    // Follow the arithmetic step `s` (AddVariables to AddImmediate), and the comparison `s` (IfEqual, IfLess).
    void Calculate(const MacroStep& s);
    void Compare(const MacroStep& s);

    // Let the time of `wait` pass. A release during the wait takes the trap of the release. Return kEnd if the voice
    // waits for ever.
    Next Pass(const Wait& wait);

    // Set up the vibrato of Vibrato step `s`, or turn it off.
    void SetVibrato(const MacroStep& s);

    // Add the voice's settings of its pitch now to its changes (MacroVoice::pitch_changes).
    void ChangePitch();

    // Add `what` to the voice's approximations, once.
    void Note(const std::string& what);

    // Set the voice's pitch wheel from its sources (PitchWheelSelect).
    void SetWheel();

    // Note a voice whose pitch wheel follows the channel's after time has passed and not before, or the other way.
    void CheckWheel();

    // Split step `s`: list the split on `controller` from `value` on (MacroVoice::splits), and go to the step's macro
    // and step if `read`, the controller's value, reaches `value`. A run without controls goes on.
    void Split(const MacroStep& s, uint8_t controller, uint8_t value, int read);

    // Add the part of the latest sample to the run's parts, with the voice's settings now, if it isn't there yet. A
    // part's settings are those when time first passes after its sample starts.
    void SettlePart();

    const Pool& pool_;
    const double release_ms_;
    const int volume_scale_divisor_;
    const ChannelControls& controls_;
    Run run_;
    Place place_;
    double now_ = 0.0;
    bool released_;
    std::optional<Place> trap_; // where the trap of the release goes on
    std::vector<Place> calls_;  // where each Gosub step's Return goes on, the latest last

    // The steps since time last passed in the run, and the waits that have let time pass.
    int steps_since_wait_ = 0;
    int waits_ = 0;

    // MusyX's count for a voice's Loop steps. All of them share it (0x801529a4).
    uint16_t loop_count_ = 0;

    // The voice's 16 variables, then MusyX's 16 global ones. MusyX sets a voice's to 0 when its macro starts
    // (0x801557ec in Pokémon Colosseum), and the globals when it starts (synthInit, 0x8014d000). A run takes the
    // globals as they are after the game's start: the first play of a sound.
    std::array<int32_t, 32> variables_{};

    // The place of the latest jump of a Loop step, and the waits by then.
    std::optional<LoopJump> last_loop_;

    // The latest sample and its start, until its part settles (SettlePart).
    std::optional<OpenPart> open_part_;

    // The tremolo's setup: the LFO that it follows, the two LFOs' periods, and SetupTremolo's scale and addition.
    std::optional<uint8_t> tremolo_lfo_;
    double lfo_period_ms_[2] = {0.0, 0.0};
    double tremolo_scale_ = 0.0;
    double tremolo_mod_add_ = 0.0;
    bool tremolo_set_ = false;

    double mod_vibrato_cents_ = 0.0; // the vibrato depth that the mod wheel adds at its top (ModulationToVibrato)

    // The sources of the voice's pitch wheel, each with the way it joins those before it (PitchWheelSelect). MusyX
    // starts a voice on the channel's pitch wheel in full.
    std::vector<WheelSource> wheel_{{InputSource{.controller = kPitchWheelControl}, 0}};
    bool wheel_selected_ = false;                // a PitchWheelSelect has run
    std::optional<bool> follows_wheel_at_start_; // MacroVoice::follows_wheel when time first passes

    bool volume_selected_ = false; // a VolumeSelect has run (MusyX's first one replaces the voice's source)
    bool reverb_selected_ = false; // a ReverbSelect has run
    bool variables_set_ = false;   // a step has set one of the voice's variables
};

Run MacroRun::Follow(uint16_t macro)
{
    if (!Jump(macro, 0))
    {
        return std::move(run_);
    }
    run_.voice.updates.push_back(0.0);

    for (int count = 0; count < kMostSteps && place_.at < place_.steps->size(); count++)
    {
        if (Step((*place_.steps)[place_.at++]) == Next::kEnd)
        {
            CheckWheel();
            SettlePart();
            return std::move(run_);
        }

        // MusyX follows kStepsPerRun steps of a voice in each run. The next step waits for the next run.
        if (++steps_since_wait_ == kStepsPerRun)
        {
            steps_since_wait_ = 0;
            now_ += kRunMs;
        }

        // A release can come while a step runs: a trap then takes the voice on from the next step.
        if (trap_ && !released_ && release_ms_ <= now_)
        {
            TakeTrap();
        }
        released_ = released_ || release_ms_ <= now_;
    }

    // The tremolo: its LFO swings between 0 and 1, and the volume drops by that times its depth. Without the mod wheel,
    // the depth is 1 minus the tremolo's scale. The mod wheel at its top multiplies the scale by the addition.
    if (tremolo_set_ && tremolo_lfo_ && lfo_period_ms_[*tremolo_lfo_] > 0.0)
    {
        run_.voice.tremolo =
            Tremolo{lfo_period_ms_[*tremolo_lfo_], 1.0 - tremolo_scale_, 1.0 - tremolo_scale_ * tremolo_mod_add_};
    }
    CheckWheel();
    SettlePart();

    return std::move(run_);
}

MacroRun::Next MacroRun::Step(const MacroStep& s)
{
    MacroVoice& v = run_.voice;
    switch (s.Op())
    {
    case MacroOp::kEnd:
    case MacroOp::kStop:
        return Next::kEnd;

    case MacroOp::kGoto:
        return Jump(s.Half(2), s.Half(4)) ? Next::kStep : Next::kEnd;

    case MacroOp::kSplitModulation:
        // A song's voice goes to the macro of bytes 2 and 3, at the step of bytes 4 and 5, once the high 7 bits of its
        // mod wheel reach byte 1. An effect's voice has no channel and goes on (macHandleActive, 0x801557EC in Pokémon
        // Colosseum). The files take the mod wheel at the note's start.
        Split(s, kModWheel, s.Byte(1), ControllerValue(controls_, kModWheel));
        break;

    case MacroOp::kSplitVelocity:
        {
            // The voice goes the same way once the whole part of its volume reaches byte 1 (macHandleActive). The
            // volume starts at the voice's start volume (StartVolume), and the steps so far have scaled it. A song's
            // voice holds its start volume in its controls. An effect's voice goes on.
            int from = 0;
            while (from < 128 && std::min(from * v.volume + v.volume_add * 127.0, 127.0) < s.Byte(1))
            {
                from++;
            }
            if (controls_.empty())
            {
                Note(kCommandsLeftOut);
            }
            Split(s, kVelocityControl, static_cast<uint8_t>(from), ControllerValue(controls_, kVelocityControl));
            break;
        }

    case MacroOp::kGosub:
        {
            // MusyX keeps the voice's latest four returns. A fifth call takes the place of the oldest (0x801528D4 in
            // Pokémon Colosseum). A call of a macro that the pool lacks ends the voice's macro.
            const Place back = place_;
            if (!Jump(s.Half(2), s.Half(4)))
            {
                return Next::kEnd;
            }

            if (calls_.size() == kMostCalls)
            {
                calls_.erase(calls_.begin());
            }
            calls_.push_back(back);
            break;
        }

    case MacroOp::kReturn:
        // A return without a call goes on to the next step (0x80156124 in Pokémon Colosseum).
        if (!calls_.empty())
        {
            place_ = calls_.back();
            calls_.pop_back();
        }
        break;

    case MacroOp::kLoop:
        return Loop(s);

    case MacroOp::kFadeIn:
        FadeIn(s);
        break;

    case MacroOp::kTrapEvent:
        if (s.Byte(1) != kReleaseTrap)
        {
            Note("traps of a sample's end or of messages left out");
        }
        else if (const auto target = pool_.macros.find(s.Half(2)); target != pool_.macros.end())
        {
            trap_ = Place{&target->second, s.Half(4)};
        }
        break;

    case MacroOp::kUntrapEvent:
        if (s.Byte(1) == kReleaseTrap)
        {
            trap_.reset();
        }
        break;

    case MacroOp::kWaitTicks:
    case MacroOp::kWaitMs:
        return Pass(WaitOf(s));

    case MacroOp::kAddKey:
        {
            const auto add = static_cast<int8_t>(s.Byte(1));
            v.key_shift = s.Byte(3) != 0 ? add : v.key_shift + add;
            v.detune_cents = static_cast<int8_t>(s.Byte(2));
            if (v.fixed_key)
            {
                v.fixed_key = s.Byte(3) != 0 ? std::nullopt : std::optional<int>(*v.fixed_key + add);
            }
            ChangePitch();
            return Pass(KeyWaitOf(s));
        }

    case MacroOp::kSetKey:
        v.fixed_key = s.Byte(1) & 0x7f;
        v.detune_cents = static_cast<int8_t>(s.Byte(2));
        v.rate_hz.reset();
        ChangePitch();
        return Pass(KeyWaitOf(s));

    case MacroOp::kPitchSweep1:
    case MacroOp::kPitchSweep2:
        {
            // Byte 1 holds the number of steps, and bytes 2 and 3 the step in Hz, a signed number. MusyX turns the
            // step into the DSP's units as it does a rate, rounded toward 0 (0x80162fb0 in Pokémon Colosseum). The
            // sweep waits as AddKey does.
            const auto hz = static_cast<int16_t>(s.Half(2));
            const auto units = static_cast<int>(std::abs(hz) * 4096.0 / kNominalRate);
            v.sweeps.push_back({.ms = now_,
                                .slot = s.Op() == MacroOp::kPitchSweep1 ? 0 : 1,
                                .steps = s.Byte(1),
                                .step = hz < 0 ? -units : units});
            return Pass(KeyWaitOf(s));
        }

    case MacroOp::kSetPitch:
        // Bytes 1 to 3 hold the rate in Hz. The games' steps leave bytes 4 to 7 at 0. A step that sets them is
        // reported.
        v.rate_hz = (s.Byte(1) << 16) | (s.Byte(2) << 8) | s.Byte(3);
        if (s.Byte(4) != 0 || s.Byte(5) != 0 || s.Byte(6) != 0 || s.Byte(7) != 0)
        {
            Note(kCommandsLeftOut);
        }
        ChangePitch();
        break;

    case MacroOp::kPianoPan:
        // Byte 1 holds the scale, byte 2 the centre key and byte 3 the centre pan.
        v.piano_pan = PianoPan{.scale = static_cast<int8_t>(s.Byte(1)),
                               .centre_key = s.Byte(2),
                               .centre_pan = s.Byte(3),
                               .key_shift = v.key_shift,
                               .fixed_key = v.fixed_key};
        v.pan_last = false;
        break;

    case MacroOp::kPanning:
        // Byte 1 holds the pan. MusyX then moves the pan by byte 4, a signed number, over the time in bytes 2 and 3.
        v.pan = s.Byte(1) & 0x7f;
        v.pan_last = true;
        if (s.Byte(4) != 0 && s.Half(2) != 0)
        {
            v.pan_moves = true;
        }
        v.pan_changes.push_back({.ms = now_,
                                 .pan = *v.pan,
                                 .delta = static_cast<int8_t>(s.Byte(4)),
                                 .glide_ms = static_cast<double>(s.Half(2))});
        break;

    case MacroOp::kPlayMacro:
        // Byte 1 adds to the key, a signed number. Bytes 2 and 3 name the macro, and bytes 4 and 5 its first step.
        v.started.push_back({.macro = s.Half(2),
                             .step = s.Half(4),
                             .key_add = static_cast<int8_t>(s.Byte(1)),
                             .key_shift = v.key_shift,
                             .fixed_key = v.fixed_key,
                             .at_ms = now_,
                             .volume_controller = v.volume_controller});
        break;

    case MacroOp::kVolumeSelect:
        {
            // Byte 1 names the controller (SelectShare has the share). Byte 4 picks how the source joins the others:
            // 0 replaces them. Byte 5 makes byte 1 name a variable. MusyX starts a voice on the volume controller in
            // full. The voice's first VolumeSelect replaces that source. Byte 4 changes nothing there (0x80153ee8 in
            // Pokémon Colosseum, 0x8010174c in Mario Party 4).
            const uint8_t combine = volume_selected_ ? s.Byte(4) : 0;
            volume_selected_ = true;
            if (combine != 0 || (s.Byte(5) == 0 && s.Byte(1) != kVolumeController) ||
                (s.Byte(5) != 0 && variables_set_))
            {
                Note("volume sources other than a share of the volume controller left out");
            }
            else if (s.Byte(5) != 0)
            {
                // MusyX takes a variable as a signed source around half of full volume (0x80161134 in Pokémon
                // Colosseum). A macro's variables start at 0. The voice's volume therefore takes half of full volume.
                v.source_volume = kSignedCentre;
                v.source_fixed = true;
            }
            else
            {
                v.source_volume = std::max(SelectShare(s), 0.0);
                v.source_fixed = false;
            }
            break;
        }

    case MacroOp::kReverbSelect:
        {
            // The fields of VolumeSelect. The voice's first ReverbSelect replaces the reverb controller (0x80154500 in
            // Pokémon Colosseum). dolphinsf2 follows the controllers below 0x80 and the variables that no step sets.
            const uint8_t combine = reverb_selected_ ? s.Byte(4) : 0;
            reverb_selected_ = true;
            const bool variable = s.Byte(5) != 0;
            if (combine != 0 || (!variable && s.Byte(1) >= 0x80) || (variable && variables_set_))
            {
                v.reverb_source.reset();
                Note(kReverbSourcesLeftOut);
                break;
            }

            v.reverb_source = InputSource{.controller = s.Byte(1), .variable = variable, .share = SelectShare(s)};
            break;
        }

    case MacroOp::kSendMessage:
        // Bytes 2 and 3 name the macro whose voices get the message, unless byte 1 makes them name a variable.
        if (s.Byte(1) != 0 || s.Half(2) != kNoMacro)
        {
            Note(kCommandsLeftOut);
        }
        break;

    case MacroOp::kStartSample:
        if (s.Byte(3) != 0 || s.word1 != 0)
        {
            Note("samples that start after their first sample play from their start");
        }

        // A sample takes the place of the one that plays. One that starts at the same time never sounds.
        if (open_part_ && open_part_->start_ms == now_)
        {
            open_part_->sample = s.Half(1);
        }
        else
        {
            SettlePart();
            open_part_ = OpenPart{.sample = s.Half(1), .start_ms = now_};
        }
        // The voice's first sample is the first that plays for a moment (hwInitSamplePlayback, 0x801624A8 in Pokémon
        // Colosseum, sets up the DSP's voice for each StartSample).
        if (!run_.sample_started || v.sample_ms == now_)
        {
            v.sample = s.Half(1);
            v.sample_ms = now_;
            run_.sample_started = true;
            run_.envelope_before_sample = v.envelope.has_value();
        }
        else
        {
            Note(kLaterSamplesLeftOut);
        }
        break;

    case MacroOp::kStopSample:
        if (!run_.stop_at)
        {
            run_.stop_at = now_;
        }
        break;

    case MacroOp::kKeyOff:
        if (!run_.release_at)
        {
            run_.release_at = now_;
        }
        break;

    case MacroOp::kEnvelope:
        {
            // Byte 1 scales the volume in 128ths. Byte 2 adds to it in 127ths of full volume. Bytes 3 and 4 name a
            // curve. The time is in milliseconds with bit 0 of byte 5, else in ticks. MusyX ramps for at least 1 ms.
            const uint16_t time = s.Half(6);
            const double ms = (s.Byte(5) & 1) != 0 ? time : time * kTickMs;
            run_.ramps.push_back({now_, std::max(ms, 1.0), s.Byte(1) / 128.0, s.Byte(2) / 127.0});
            if (pool_.tables.contains(s.Half(3)))
            {
                Note("volume curves left out");
            }
            break;
        }

    case MacroOp::kSetAdsr:
        v.envelope = EnvelopeOf(pool_, s.Half(1), s.Byte(3) != 0);
        if (v.envelope && !v.envelope->dls && (v.envelope->decay_ms > 0.0 || v.envelope->release_ms > 0.0))
        {
            Note("linear envelopes' decays and releases fall in decibels");
        }
        break;

    case MacroOp::kSetAdsrFromControllers:
        {
            // Bytes 1, 2, 3 and 4 name the controllers of the attack, the decay, the sustain and the release. A song's
            // voice takes their values from its channel when the step runs. The files take them at the note's start.
            const std::array<uint8_t, 4> numbers{s.Byte(1), s.Byte(2), s.Byte(3), s.Byte(4)};
            for (const uint8_t c : numbers)
            {
                if (c >= 0x60 && c < 0x66)
                {
                    continue;
                }

                const uint8_t kept = KeptController(c);
                if (std::ranges::find(v.controllers, kept) == v.controllers.end())
                {
                    v.controllers.push_back(kept);
                }
            }
            std::ranges::sort(v.controllers);
            if (controls_.empty())
            {
                Note(kCommandsLeftOut);
                break;
            }

            v.envelope =
                ControllerEnvelope(ControllerValue(controls_, numbers[0]), ControllerValue(controls_, numbers[1]),
                                   ControllerValue(controls_, numbers[2]), ControllerValue(controls_, numbers[3]));
            break;
        }

    case MacroOp::kSetPitchAdsr:
        if (const std::optional<Envelope> e = EnvelopeOf(pool_, s.Half(1), true))
        {
            Note("pitch envelopes fall in a line, not in decibels");
            const auto keys = static_cast<int8_t>(s.Byte(4));
            const auto cents = static_cast<int8_t>(s.Byte(5));
            const double range = keys * 100.0 + (keys >= 0 ? cents : -cents);
            v.pitch_envelope = PitchEnvelope{*e, range};
        }
        break;

    case MacroOp::kScaleVolumeDls:
        {
            const double scale = s.Half(1) / 4096.0;
            v.volume = (s.Byte(3) != 0 ? 1.0 : v.volume) * scale;
            v.volume_add = s.Byte(3) != 0 ? 0.0 : v.volume_add * scale;
            break;
        }

    case MacroOp::kScaleVolume:
        {
            // Byte 5 picks the note's volume in place of the voice's. Byte 2 adds to the scaled volume in 127ths of
            // full volume. Bytes 3 and 4 name a table of the pool as a curve. MusyX skips a curve that the pool lacks.
            const double scale = s.Byte(1) / static_cast<double>(volume_scale_divisor_);
            v.volume = (s.Byte(5) != 0 ? 1.0 : v.volume) * scale;
            v.volume_add = (s.Byte(5) != 0 ? 0.0 : v.volume_add) * scale + s.Byte(2) / 127.0;
            if (pool_.tables.contains(s.Half(3)))
            {
                Note("volume curves left out");
            }
            break;
        }

    case MacroOp::kModeSelect:
        v.dls_volume = s.Byte(1) != 0;
        v.itd = s.Byte(2) == 0;
        break;

    case MacroOp::kVibrato:
        SetVibrato(s);
        break;

    case MacroOp::kModulationToVibrato:
        {
            // MusyX keeps the depth in 256ths of a semitone.
            const auto keys = static_cast<int8_t>(s.Byte(1));
            const auto cents = static_cast<int8_t>(s.Byte(2));
            const int scale = keys * 256 + (keys >= 0 ? cents * 256 / 100 : -(cents * 256 / 100));
            mod_vibrato_cents_ = scale * 100.0 / 256.0;
            if (v.vibrato)
            {
                v.vibrato->mod_depth_cents = mod_vibrato_cents_;
            }
            break;
        }

    case MacroOp::kSetupLfo:
        // A pitch wheel that reads the LFO follows it from here (SetWheel).
        if (s.Byte(1) < 2)
        {
            lfo_period_ms_[s.Byte(1)] = s.Half(2);
            if (wheel_selected_)
            {
                SetWheel();
            }
        }
        break;

    case MacroOp::kTremoloSelect:
        // Controllers 0x82 and 0x83 are the voice's two LFOs.
        if (s.Byte(1) == 0x82 || s.Byte(1) == 0x83)
        {
            tremolo_lfo_ = static_cast<uint8_t>(s.Byte(1) - 0x82);
            if (static_cast<int16_t>(s.Half(2)) != 100)
            {
                Note("tremolos at other than 100% of their LFO take 100%");
            }
        }
        else
        {
            Note("tremolos from controllers other than an LFO left out");
        }
        break;

    case MacroOp::kSetupTremolo:
        tremolo_scale_ = s.Half(1) / 4096.0;
        tremolo_mod_add_ = (s.word1 & 0xffff) / 4096.0;
        tremolo_set_ = true;
        break;

    case MacroOp::kPitchWheelSelect:
        {
            // Byte 1 names the source's controller, or with byte 5 one of the voice's variables. SelectShare gives the
            // share. Byte 4 picks how the source joins those before it: 0 in their place, 1 added, 2 multiplied, 3
            // taken away. The voice's first PitchWheelSelect takes the channel's pitch wheel's place
            // (mcmdPitchWheelSelect, 0x801540F0 in Pokémon Colosseum).
            const uint8_t combine = wheel_selected_ ? s.Byte(4) : 0;
            wheel_selected_ = true;
            if (combine == 0)
            {
                wheel_.clear();
            }
            if (wheel_.size() < kMostSources)
            {
                wheel_.push_back(
                    {InputSource{.controller = s.Byte(1), .variable = s.Byte(5) != 0, .share = SelectShare(s)},
                     combine});
            }
            SetWheel();
            break;
        }

    case MacroOp::kPitchWheelRange:
        v.bend_up = s.Byte(1);
        v.bend_down = s.Byte(2);
        break;

    case MacroOp::kSetKeyGroup:
        // A voice that sets a key group stops the group's other voices at once, or releases them. A SoundFont's
        // exclusive class stops them.
        v.key_group = s.Byte(1);
        if (v.key_group != 0 && s.Byte(2) == 0)
        {
            Note("key groups that release their voices stop them");
        }
        break;

    // The voice's priority and age only decide which voice to take when the voices run out. The resampler's filter and
    // the priority of the channel change nothing a SoundFont plays.
    case MacroOp::kSetPriority:
    case MacroOp::kAddPriority:
    case MacroOp::kAddAgeCounter:
    case MacroOp::kSetAgeCounter:
    case MacroOp::kAgeCounterSpeed:
    case MacroOp::kAgeCounterByVolume:
    case MacroOp::kSrcModeSelect:
        break;

    // SetVariable sets a variable, or with byte 1 a MIDI controller that byte 2 names. Bytes 4 and 5 hold the value. A
    // variable changes the sound only through a step that reads it.
    case MacroOp::kSetVariable:
        variables_set_ = variables_set_ || s.Byte(1) == 0;
        if (s.Byte(1) == 0)
        {
            variables_[s.Byte(2) & kVariableMask] = static_cast<int16_t>(s.Half(4));
        }
        else
        {
            const double value = std::min(s.Half(4), kFullController) / static_cast<double>(kFullController);
            switch (s.Byte(2))
            {
            case kVolumeController:
                v.volume_controller = value;
                break;
            case kReverbController:
                v.reverb_controller = value;
                break;
            case kChorusController:
                v.chorus_controller = value;
                break;
            default:
                Note(kControllersLeftOut);
                break;
            }
        }
        break;

    // The arithmetic on variables. Byte 1 makes the result a controller.
    case MacroOp::kAddVariables:
    case MacroOp::kSubtractVariables:
    case MacroOp::kMultiplyVariables:
    case MacroOp::kDivideVariables:
    case MacroOp::kAddImmediate:
        variables_set_ = variables_set_ || s.Byte(1) == 0;
        Calculate(s);
        break;

    case MacroOp::kIfEqual:
    case MacroOp::kIfLess:
        Compare(s);
        break;

    default:
        Note(kCommandsLeftOut);
        break;
    }

    return Next::kStep;
}

void MacroRun::SetWheel()
{
    // MusyX adds up the sources' moves away from the wheel's centre (_GetInputValue, 0x80161134 in Pokémon Colosseum).
    // The channel's pitch wheel moves it by its share. An LFO swings it by its share of the wheel's range either way:
    // MusyX's sine reaches 4095 (sndSin, 0x8016208C), and the input takes twice that. A variable that no step sets
    // stays at the centre.
    MacroVoice& v = run_.voice;
    double wheel = 0.0;
    std::optional<WheelLfo> lfo;
    for (const WheelSource& w : wheel_)
    {
        const InputSource& source = w.source;
        if (w.combine > 1)
        {
            Note("pitch wheels that multiply or take away their sources left out");
        }
        else if (source.variable)
        {
            if (variables_set_)
            {
                Note("pitch wheels from variables left out");
            }
        }
        else if (source.controller == kPitchWheelControl)
        {
            wheel += source.share;
        }
        else if (source.controller == kLfo0Control || source.controller == kLfo1Control)
        {
            const double period_ms = lfo_period_ms_[source.controller - kLfo0Control];
            if (period_ms > 0.0 && source.share != 0.0)
            {
                if (lfo)
                {
                    Note("a second LFO on a pitch wheel left out");
                }
                else
                {
                    lfo = WheelLfo{.delay_ms = now_, .period_ms = period_ms, .share = source.share};
                }
            }
        }
        else
        {
            Note("pitch wheels from controllers other than the pitch wheel and the LFOs left out");
        }
    }

    // A SoundFont's voice follows the channel's wheel in full or not at all, from its start (Pass).
    if (wheel != 0.0 && wheel != 1.0)
    {
        Note("pitch wheels at other than 100% of the channel's take 100%");
    }
    v.follows_wheel = wheel != 0.0;
    v.wheel_lfo = lfo;
}

void MacroRun::Split(const MacroStep& s, uint8_t controller, uint8_t value, int read)
{
    const std::pair<uint8_t, uint8_t> split{controller, value};
    if (std::ranges::find(run_.voice.splits, split) == run_.voice.splits.end())
    {
        run_.voice.splits.push_back(split);
    }

    // A macro that the pool lacks leaves the voice where it is.
    if (!controls_.empty() && read >= value)
    {
        Jump(s.Half(2), s.Half(4));
    }
}

bool MacroRun::Jump(uint16_t macro, uint16_t step)
{
    const auto target = pool_.macros.find(macro);
    if (target == pool_.macros.end())
    {
        return false;
    }

    place_ = {&target->second, step};

    return true;
}

void MacroRun::TakeTrap()
{
    released_ = true;
    place_ = *trap_;
    trap_.reset();
}

void MacroRun::CheckWheel()
{
    // A SoundFont's voice takes the run's last wheel from its start.
    if (!follows_wheel_at_start_)
    {
        follows_wheel_at_start_ = run_.voice.follows_wheel;
    }
    else if (*follows_wheel_at_start_ != run_.voice.follows_wheel)
    {
        Note("pitch wheels that a macro turns later turn at the note's start");
    }
}

MacroRun::Next MacroRun::Pass(const Wait& wait)
{
    if (wait.time == 0)
    {
        return Next::kStep;
    }

    if (wait.random)
    {
        Note("random waits take their longest time");
    }
    if (!wait.ms)
    {
        Note("waits in ticks take a tempo of 120");
    }

    // A wait for the release ends at once after the release, and one for the sample's end at once when the sample isn't
    // playing.
    if ((wait.on_release && released_) || (wait.on_sample_end && !run_.sample_started))
    {
        return Next::kStep;
    }
    CheckWheel();
    SettlePart();

    const double length = wait.time == kWaitForever ? kNever : (wait.ms ? wait.time : wait.time * kTickMs);
    double end = wait.absolute ? length : now_ + length;
    if (wait.on_release)
    {
        end = std::min(end, std::max(now_, release_ms_));
    }

    // The run after a wait starts its count of steps afresh.
    steps_since_wait_ = 0;
    waits_++;

    // A trap of the release jumps when the note is released during the wait.
    if (trap_ && !released_ && release_ms_ <= end)
    {
        now_ = std::max(now_, release_ms_);
        TakeTrap();
        return Next::kStep;
    }
    if (end == kNever)
    {
        // The voice waits for ever, or for its sample's end. Nothing it does later changes its sound.
        return Next::kEnd;
    }

    now_ = std::max(now_, end);
    released_ = released_ || release_ms_ <= now_;
    run_.voice.updates.push_back(now_);

    return Next::kStep;
}

MacroRun::Next MacroRun::Loop(const MacroStep& s)
{
    // Loop goes back to step `Half(4)` of its macro `Half(6)` times, or with bit 0 of byte 2 a random number of times
    // below that. A count of kLoopForever never runs out. Bit 0 of byte 1 ends the loop once the note is released, and
    // bit 0 of byte 3 once the sample has stopped (0x801529a4 in Pokémon Colosseum).
    if (loop_count_ == 0)
    {
        const uint16_t count = s.Half(6);
        if ((s.Byte(2) & 1) != 0)
        {
            Note("random loop counts take their highest");
            loop_count_ = count == 0 ? 0 : count - 1;
        }
        else
        {
            loop_count_ = count;
        }
        if (loop_count_ != kLoopForever)
        {
            loop_count_++;
        }
    }
    if (loop_count_ != kLoopForever && --loop_count_ == 0)
    {
        return Next::kStep;
    }

    const bool on_release = (s.Byte(1) & 1) != 0;
    const bool on_sample_end = (s.Byte(3) & 1) != 0;
    if ((on_release && released_) || (on_sample_end && !run_.sample_started))
    {
        loop_count_ = 0;
        return Next::kStep;
    }

    // A loop that never runs out and comes back without a wait goes round in each run until something ends it: the
    // release, the trap of the release, or the sample's end. It waits for that.
    const LoopJump jump{place_.steps, place_.at, waits_};
    if (loop_count_ == kLoopForever && last_loop_ == jump)
    {
        // A held note's loop goes round for ever. Nothing that the voice does later changes its sound.
        if (release_ms_ == kNever)
        {
            return Next::kEnd;
        }

        const Place at = place_;
        if (Pass({.time = kWaitForever, .ms = true, .on_release = on_release, .on_sample_end = on_sample_end}) ==
            Next::kEnd)
        {
            return Next::kEnd;
        }
        if (place_.steps == at.steps && place_.at == at.at)
        {
            loop_count_ = 0;
        }
        return Next::kStep;
    }

    last_loop_ = jump;
    place_.at = s.Half(4);

    return Next::kStep;
}

std::optional<int32_t> MacroRun::VariableAt(const MacroStep& s, int at) const
{
    if (s.Byte(at) != 0)
    {
        return std::nullopt;
    }

    return variables_[s.Byte(at + 1) & kVariableMask];
}

void MacroRun::Calculate(const MacroStep& s)
{
    // Bytes 3 and 4 name the first operand, and bytes 5 and 6 the second. AddImmediate adds bytes 5 and 6 as a signed
    // number. Bytes 1 and 2 name the result, held to 16 signed bits (0x801550e0 in Pokémon Colosseum). Division by 0
    // gives 0.
    const std::optional<int32_t> a = VariableAt(s, 3);
    const std::optional<int32_t> b = s.Op() == MacroOp::kAddImmediate
                                         ? std::optional<int32_t>(static_cast<int16_t>(s.Byte(5) | s.Byte(6) << 8))
                                         : VariableAt(s, 5);
    if (s.Byte(1) != 0 || !a || !b)
    {
        Note(kControllersLeftOut);
        if (s.Byte(1) != 0)
        {
            return;
        }
    }

    const int32_t x = static_cast<int16_t>(a.value_or(0));
    const int32_t y = static_cast<int16_t>(b.value_or(0));
    int32_t result = 0;
    switch (s.Op())
    {
    case MacroOp::kSubtractVariables:
        result = x - y;
        break;
    case MacroOp::kMultiplyVariables:
        result = x * y;
        break;
    case MacroOp::kDivideVariables:
        result = y != 0 ? x / y : 0;
        break;
    default:
        result = x + y;
        break;
    }
    variables_[s.Byte(2) & kVariableMask] = std::clamp<int32_t>(result, INT16_MIN, INT16_MAX);
}

void MacroRun::Compare(const MacroStep& s)
{
    // IfEqual and IfLess compare the value that bytes 1 and 2 name with the one that bytes 3 and 4 name. Byte 5 turns
    // the result round. When it holds, the run goes on at step `Half(6)` of the macro (0x80155300 in Pokémon
    // Colosseum). A run has no controller values. A comparison with a controller goes on with the next step.
    const std::optional<int32_t> a = VariableAt(s, 1);
    const std::optional<int32_t> b = VariableAt(s, 3);
    if (!a || !b)
    {
        Note(kCommandsLeftOut);
        return;
    }

    bool holds = s.Op() == MacroOp::kIfEqual ? *a == *b : *a < *b;
    if (s.Byte(5) != 0)
    {
        holds = !holds;
    }
    if (holds)
    {
        place_.at = s.Half(6);
    }
}

void MacroRun::FadeIn(const MacroStep& s)
{
    // FadeIn ramps the voice's volume from silence as Envelope does from where it is (0x80153c10 in Pokémon
    // Colosseum). The files play a fade at the voice's start as the attack of its envelope, and the volume takes the
    // ramp's end at once. A later fade ramps from the volume that the voice has.
    const uint16_t time = s.Half(6);
    const double ms = std::max((s.Byte(5) & 1) != 0 ? time : time * kTickMs, 1.0);
    MacroVoice& v = run_.voice;
    const auto curve = pool_.tables.find(s.Half(3));
    if (now_ == 0.0)
    {
        v.fade_in_ms = ms;
        v.volume *= s.Byte(1) / 128.0;
        v.volume_add = v.volume_add * s.Byte(1) / 128.0 + s.Byte(2) / 127.0;

        // A curve that the pool holds maps the fade's end. The files map it for a note at full volume. MusyX holds the
        // volume to full before the curve.
        if (curve != pool_.tables.end())
        {
            if (const std::optional<double> mapped = CurveVolume(curve->second, 127.0 * (v.volume + v.volume_add)))
            {
                v.volume = *mapped / 127.0;
                v.volume_add = 0.0;
                return;
            }
        }
    }
    else
    {
        Note("fade-ins after a voice's start ramp from its volume");
        run_.ramps.push_back({now_, ms, s.Byte(1) / 128.0, s.Byte(2) / 127.0});
    }
    if (curve != pool_.tables.end())
    {
        Note("volume curves left out");
    }
}

void MacroRun::SettlePart()
{
    if (!open_part_)
    {
        return;
    }

    MacroVoice part = run_.voice;
    part.sample = open_part_->sample;
    part.sample_ms = open_part_->start_ms;
    part.started.clear();
    run_.parts.push_back(std::move(part));
    open_part_.reset();
}

void MacroRun::SetVibrato(const MacroStep& s)
{
    MacroVoice& v = run_.voice;
    const uint16_t time = s.Half(6);
    if (time == 0)
    {
        v.vibrato.reset();
        return;
    }

    // A negative key range turns the vibrato's phase. A negative cent range with no keys does too.
    const auto keys = static_cast<int8_t>(s.Byte(1));
    const auto cents = static_cast<int8_t>(s.Byte(2));
    int key_range = keys;
    int cent_range = cents;
    if (keys < 0)
    {
        key_range = -keys;
        cent_range = cents < 0 ? -cents : cents;
    }
    else if (cents < 0)
    {
        key_range = keys == 0 ? 0 : keys - 1;
        cent_range = keys == 0 ? -cents : 100 - cents;
    }
    if (keys < 0 || (keys == 0 && cents < 0))
    {
        Note("vibratos that start at the bottom of their sine start at the top");
    }

    Vibrato vib;
    vib.delay_ms = now_;
    vib.period_ms = (s.Byte(5) & 1) != 0 ? time : time * kTickMs;
    vib.depth_cents = key_range * 100.0 + cent_range;
    vib.mod_depth_cents = mod_vibrato_cents_;
    vib.mod_scales_depth = (s.Byte(3) & 3) != 0;
    v.vibrato = vib;
}

void MacroRun::ChangePitch()
{
    const MacroVoice& v = run_.voice;
    run_.voice.pitch_changes.push_back({.ms = now_,
                                        .key_shift = v.key_shift,
                                        .fixed_key = v.fixed_key,
                                        .detune_cents = v.detune_cents,
                                        .rate_hz = v.rate_hz});
}

void MacroRun::Note(const std::string& what)
{
    std::vector<std::string>& notes = run_.voice.approximations;
    if (std::find(notes.begin(), notes.end(), what) == notes.end())
    {
        notes.push_back(what);
    }
}

// Run `macro`'s steps for a note that the player releases `release_ms` after its start: kNever for a note that it
// holds. ScaleVolume divides by `volume_scale_divisor`.
Run RunMacro(const Pool& pool, uint16_t macro, double release_ms, int volume_scale_divisor,
             const ChannelControls& controls)
{
    return MacroRun(pool, release_ms, volume_scale_divisor, controls).Follow(macro);
}

// The pan of a voice that an entry of a keymap or a layer starts, under the pan `pan` of the objects before it. The
// entry's pan `entry_pan` moves `pan` by its distance from the centre. A pan of 0x80 or more leaves `pan` as it is.
int EntryPan(uint8_t entry_pan, int pan)
{
    return entry_pan < 0x80 ? std::clamp(entry_pan - 64 + pan, 0, 127) : pan;
}

// Add the voices that `key` starts on `object` to `out`, with `pan` and `volume` from the layers and keymaps before it.
void AddVoices(const Pool& pool, uint16_t object, int key, int pan, double volume, int level,
               std::vector<KeyVoice>& out)
{
    if (level >= kMostLevels)
    {
        return;
    }

    switch (KindOf(object))
    {
    case ObjectKind::kMacro:
        if (pool.macros.contains(object))
        {
            out.push_back({.macro = object, .key = key, .pan = pan, .volume = volume});
        }
        break;

    case ObjectKind::kKeymap:
        {
            // A key of a keymap that plays a keymap plays nothing.
            const auto keymap = pool.keymaps.find(object);
            if (keymap == pool.keymaps.end())
            {
                break;
            }

            const ntdlib::musyx::KeymapEntry& e = keymap->second[static_cast<std::size_t>(key)];
            if (e.object == 0xffff || KindOf(e.object) == ObjectKind::kKeymap)
            {
                break;
            }

            AddVoices(pool, e.object, std::clamp(key + e.transpose, 0, 127), EntryPan(e.pan, pan), volume, level + 1,
                      out);
            break;
        }

    case ObjectKind::kLayer:
        {
            const auto layer = pool.layers.find(object);
            if (layer == pool.layers.end())
            {
                break;
            }

            for (const ntdlib::musyx::LayerEntry& e : layer->second)
            {
                if (e.object == 0xffff || key < e.low_key || key > e.high_key)
                {
                    continue;
                }

                AddVoices(pool, e.object, std::clamp(key + e.transpose, 0, 127), EntryPan(e.pan, pan),
                          volume * e.volume / 127.0, level + 1, out);
            }
            break;
        }
    }
}

// The fade of a voice whose macro runs as `run` for a release `release_ms` after the note's start: the ramps from the
// release to the first StopSample or KeyOff after it, where the fade stops (KeyOff::kFade). A voice that still sounds
// at a KeyOff releases there.
ReleaseFade FadeOf(const Run& run, double release_ms)
{
    const double stop_at = std::min(run.stop_at.value_or(kNever), run.release_at.value_or(kNever));
    ReleaseFade fade;
    fade.stop_ms = stop_at - release_ms;
    for (const VolumeRamp& r : run.ramps)
    {
        if (r.start_ms >= release_ms && r.start_ms < stop_at)
        {
            fade.ramps.push_back({r.start_ms - release_ms, r.ms, r.scale, r.add});
        }
    }
    fade.releases = run.release_at && *run.release_at == stop_at && RampedVolume(fade.ramps, 1.0, fade.stop_ms) > 0.0;

    return fade;
}

// Whether the voice whose macro runs as `run` for a release and as `held` for a held note releases itself: its KeyOff
// comes at the same time whether the note is released or not.
bool ReleasesItself(const Run& run, const Run& held)
{
    return run.release_at && held.release_at && *run.release_at == *held.release_at;
}

// What a release `release_ms` after a note's start does in `run`, a run of the macro for that release. `held` is the
// run for a held note. A voice that plays on after the release until a StopSample fades (kFade). A voice that stops at
// the release is cut. A voice that stops by itself at the same time as a held note does ignores the release
// (kAsHeld). A KeyOff at the release releases the envelope. So does a KeyOff that comes at the same time whether the
// note is released or not. A voice with a later KeyOff plays on until it, and follows any fade on the way (kFade).
KeyOff KeyOffOf(const Run& run, double release_ms, const Run& held)
{
    const bool stops =
        run.stop_at && *run.stop_at >= release_ms && (!run.release_at || *run.stop_at <= *run.release_at);
    if (stops)
    {
        if (*run.stop_at == release_ms)
        {
            return KeyOff::kCut;
        }

        return run.stop_at != held.stop_at ? KeyOff::kFade : KeyOff::kAsHeld;
    }
    if (run.release_at && *run.release_at >= release_ms)
    {
        return *run.release_at > release_ms && !ReleasesItself(run, held) ? KeyOff::kFade : KeyOff::kRelease;
    }

    return KeyOff::kIgnored;
}

} // namespace

double SourceValue(const InputSource& source, double controller)
{
    constexpr double kCentre = 0x2000;
    if (source.variable || source.controller == kModWheel || source.controller == kPan)
    {
        const double from_centre = source.variable ? 0.0 : controller - kCentre;
        return kCentre + std::clamp(std::floor(from_centre * source.share), -kCentre, kCentre - 1.0);
    }

    return std::clamp(std::floor(controller * source.share), 0.0, kFullInput);
}

double DlsMilliseconds(double timecents)
{
    // MusyX works the time out in single precision, and keeps the low 16 bits of its whole milliseconds. A time too
    // long for 31 bits saturates.
    const float ms = 1000.0f * std::pow(2.0f, static_cast<float>(timecents / 1200.0));
    const uint32_t whole = ms >= 2147483647.0f ? 0x7fffffffu : static_cast<uint32_t>(ms);
    return static_cast<double>(whole & 0xffff);
}

double PianoPanAt(const PianoPan& pan, int key)
{
    // MusyX works the pan out in 16.16 fixed point. The division by 128 there loses nothing.
    const int voice_key = pan.fixed_key ? *pan.fixed_key : key + pan.key_shift;
    return std::clamp(pan.centre_pan + (voice_key - pan.centre_key) * pan.scale / 128.0, 0.0, 127.0);
}

double RampedVolume(const std::vector<VolumeRamp>& ramps, double volume, double ms)
{
    double from = volume;
    double to = volume;
    double start = 0.0;
    double length = 0.0;

    const auto at = [&](double t)
    {
        return t >= start + length ? to : from + (to - from) * (t - start) / length;
    };

    for (const VolumeRamp& r : ramps)
    {
        if (r.start_ms > ms)
        {
            break;
        }

        from = at(r.start_ms);
        to = std::min(from * r.scale + r.add, 1.0);
        start = r.start_ms;
        length = r.ms;
    }

    return at(ms);
}

std::optional<double> FadeVolume(const ReleaseFade& fade, double volume, double ms)
{
    if (ms >= fade.stop_ms)
    {
        return std::nullopt;
    }

    return RampedVolume(fade.ramps, volume, ms);
}

MacroVoice FollowMacro(const Pool& pool, uint16_t macro, int volume_scale_divisor, const ChannelControls& controls)
{
    // The held note gives the settings, the fades of its volume, and any stop that the voice makes by itself.
    Run held = RunMacro(pool, macro, kNever, volume_scale_divisor, controls);
    MacroVoice voice = std::move(held.voice);
    if (held.stop_at)
    {
        voice.stop_ms = *held.stop_at;
    }
    else if (held.release_at)
    {
        voice.stop_ms = *held.release_at;
    }
    for (const VolumeRamp& r : held.ramps)
    {
        if (!voice.stop_ms || r.start_ms < *voice.stop_ms)
        {
            voice.held_ramps.push_back(r);
        }
    }
    if (voice.envelope && !held.envelope_before_sample)
    {
        voice.approximations.push_back("envelopes set after their sample starts apply from its start");
    }
    if (voice.envelope && voice.fade_in_ms > 1.0)
    {
        voice.approximations.push_back("fade-ins over an envelope take the longer of the two attacks");
    }

    // A release late in the note: after every wait of a fixed time, or halfway to the voice's stop of its own.
    const double late_ms = voice.stop_ms ? *voice.stop_ms / 2.0 : 60000.0;
    const Run late = RunMacro(pool, macro, late_ms, volume_scale_divisor, controls);
    voice.key_off = KeyOffOf(late, late_ms, held);
    voice.releases_itself = voice.key_off == KeyOff::kRelease && ReleasesItself(late, held);

    // A split after the release picks the release's way. The voice reads its controls for that split too.
    for (const std::pair<uint8_t, uint8_t>& split : late.voice.splits)
    {
        if (std::ranges::find(voice.splits, split) == voice.splits.end())
        {
            voice.splits.push_back(split);
        }
    }
    if (voice.key_off == KeyOff::kFade)
    {
        voice.release_fade = FadeOf(late, late_ms);
    }

    // A KeyOff step releases the voice's envelopes. It takes no trap (0x801557ec in Pokémon Colosseum). The note ends
    // there and plays the release of a released note. A voice that does something else on its note's release stops
    // instead.
    if (!held.stop_at && held.release_at && voice.key_off != KeyOff::kRelease)
    {
        voice.approximations.push_back("voices that release themselves stop instead");
    }

    // A release early in the note should do the same.
    constexpr double kEarly = 1.0;
    const Run early = RunMacro(pool, macro, kEarly, volume_scale_divisor, controls);
    if (KeyOffOf(early, kEarly, held) != voice.key_off)
    {
        voice.approximations.push_back("releases early in a note act as late ones do");
    }

    // A part of a voice that starts one sample after another stops where the next part starts. The last stops where
    // the voice does. Each part follows the voice's ramps, tremolo, release and changes of its pitch.
    if (held.parts.size() > 1)
    {
        for (std::size_t i = 0; i < held.parts.size(); i++)
        {
            MacroVoice& part = held.parts[i];
            part.stop_ms = i + 1 < held.parts.size() ? held.parts[i + 1].sample_ms : voice.stop_ms;
            part.held_ramps = voice.held_ramps;
            part.pitch_changes = voice.pitch_changes;
            part.pan_changes = voice.pan_changes;
            part.sweeps = voice.sweeps;
            part.updates = voice.updates;
            part.tremolo = voice.tremolo;
            part.key_off = voice.key_off;
            part.release_fade = voice.release_fade;
            part.releases_itself = voice.releases_itself;
            part.approximations = voice.approximations;
            std::erase(part.approximations, std::string(kLaterSamplesLeftOut));
        }
        voice.parts = std::move(held.parts);
    }

    return voice;
}

MacroVoice VoiceFrom(const MacroVoice& voice, double ms)
{
    MacroVoice later = voice;
    later.sample_ms = std::max(voice.sample_ms - ms, 0.0);
    later.fade_in_ms = std::max(voice.fade_in_ms - ms, 0.0);
    if (voice.stop_ms)
    {
        if (*voice.stop_ms < ms)
        {
            later.sample.reset();
        }
        later.stop_ms = std::max(*voice.stop_ms - ms, 0.0);
    }

    // A ramp that starts before the note leaves the volume where it is at the note's start. The volume there depends
    // on the note's volume.
    later.held_ramps.clear();
    for (VolumeRamp r : voice.held_ramps)
    {
        if (r.start_ms >= ms)
        {
            r.start_ms -= ms;
            later.held_ramps.push_back(r);
        }
    }
    if (later.held_ramps.size() < voice.held_ramps.size())
    {
        later.approximations.push_back("volume ramps before a voice's sample starts left out");
    }
    if (later.vibrato)
    {
        later.vibrato->delay_ms = std::max(later.vibrato->delay_ms - ms, 0.0);
    }
    later.started.clear();

    // The changes of the pitch keep their times before the note's start. A sweep goes on from there.
    for (PitchSettings& c : later.pitch_changes)
    {
        c.ms -= ms;
    }
    for (PitchSweep& sweep : later.sweeps)
    {
        sweep.ms -= ms;
    }
    for (PanSetting& pan : later.pan_changes)
    {
        pan.ms -= ms;
    }
    for (double& update : later.updates)
    {
        update -= ms;
    }

    return later;
}

std::vector<PitchPoint> PitchCurve(const MacroVoice& voice, const Sample& sample, int key, double wheel, double ms)
{
    std::vector<PitchPoint> curve{{0.0, wheel}};
    const PitchSettings settings{.key_shift = voice.key_shift,
                                 .fixed_key = voice.fixed_key,
                                 .detune_cents = voice.detune_cents,
                                 .rate_hz = voice.rate_hz};
    const double reference = DspPitch(settings, sample, key);
    if (reference <= 0.0 || voice.updates.empty())
    {
        return curve;
    }

    // A sweep's count falls by 16 for each unit of time. Its offset rises by 16 times the step for each unit of time,
    // in 65536ths of the DSP's units. Once the count reaches 0, the count starts again and the offset goes back to 0.
    // Each update adds the time since the update before (0x8014b044 in Pokémon Colosseum).
    struct Slot
    {
        int64_t steps = 0;
        int64_t step = 0;
        int64_t count = 0;
        int64_t offset = 0;
    };
    std::array<Slot, 2> slots;
    PitchSettings now;
    std::size_t next_change = 0;
    std::size_t next_sweep = 0;
    std::size_t next_update = 1;
    double t = voice.updates.front();
    double last = t;
    for (;;)
    {
        // The macro's steps run before the update.
        for (; next_sweep < voice.sweeps.size() && voice.sweeps[next_sweep].ms <= t; next_sweep++)
        {
            const PitchSweep& sweep = voice.sweeps[next_sweep];
            slots[sweep.slot] = {.steps = sweep.steps, .step = sweep.step, .count = sweep.steps << 16, .offset = 0};
        }
        for (; next_change < voice.pitch_changes.size() && voice.pitch_changes[next_change].ms <= t; next_change++)
        {
            now = voice.pitch_changes[next_change];
        }

        const auto delta = static_cast<int64_t>(std::llround((t - last) * kTimeUnitsPerMs));
        last = t;
        for (Slot& slot : slots)
        {
            if (slot.steps == 0 || delta == 0)
            {
                continue;
            }
            slot.count -= delta * 16;
            if (slot.count <= 0)
            {
                slot.count = slot.steps << 16;
                slot.offset = 0;
            }
            else
            {
                slot.offset += slot.step * 16 * delta;
            }
        }

        double pitch = DspPitch(now, sample, key) * std::exp2(wheel / 12.0) +
                       static_cast<double>(slots[0].offset + slots[1].offset) / 65536.0;
        if (pitch < 0.0 || pitch > kHighestPitch)
        {
            pitch = kHighestPitch;
        }
        const double semitones = 12.0 * std::log2(std::max(pitch, 1.0) / reference);
        if (t <= 0.0)
        {
            curve.front().semitones = semitones;
        }
        else if (semitones != curve.back().semitones)
        {
            curve.push_back({t, semitones});
        }

        // The pitch stays where it is once the voice's last change has passed and no sweep runs.
        const bool sweeping = std::any_of(slots.begin(), slots.end(), [](const Slot& slot) { return slot.steps != 0; });
        if (!sweeping && next_change == voice.pitch_changes.size() && next_sweep == voice.sweeps.size())
        {
            return curve;
        }

        // The next update comes 15 ms after this one, or at the end of a wait before then.
        for (; next_update < voice.updates.size() && voice.updates[next_update] <= t; next_update++)
        {
        }
        double next = t + kUpdateMs;
        if (next_update < voice.updates.size())
        {
            next = std::min(next, voice.updates[next_update]);
        }
        if (next > ms || (voice.stop_ms && next >= *voice.stop_ms))
        {
            return curve;
        }
        t = next;
    }
}

std::vector<PanPoint> PanCurve(const MacroVoice& voice, double ms)
{
    std::vector<PanPoint> curve;
    if (!voice.pan_last || !voice.pan || voice.pan_changes.empty() || voice.updates.empty())
    {
        return curve;
    }

    // The pan moves from its start towards its target. The glide's time left falls at each update.
    double pan = voice.pan_changes.front().pan;
    double target = pan;
    double left_ms = 0.0;
    double per_ms = 0.0;
    bool gliding = false;
    std::size_t next_change = 0;
    std::size_t next_update = 1;
    double t = voice.updates.front();
    double last = t;
    curve.push_back({0.0, 0.0});
    for (;;)
    {
        // The macro's steps run before the update. A Panning step sets the pan at once. The update then moves it by the
        // time since the update before.
        for (; next_change < voice.pan_changes.size() && voice.pan_changes[next_change].ms <= t; next_change++)
        {
            const PanSetting& p = voice.pan_changes[next_change];
            pan = p.pan;
            target = p.pan + p.delta;
            left_ms = p.glide_ms;
            per_ms = p.glide_ms > 0.0 ? p.delta / p.glide_ms : 0.0;
            gliding = p.delta != 0;
        }
        if (gliding)
        {
            left_ms -= t - last;
            gliding = left_ms > 0.0;
            pan = std::clamp(gliding ? target - left_ms * per_ms : target, 0.0, 127.0);
        }
        last = t;

        const double offset = pan - *voice.pan;
        if (t <= 0.0)
        {
            curve.front().offset = offset;
        }
        else if (offset != curve.back().offset)
        {
            curve.push_back({t, offset});
        }

        if (!gliding && next_change == voice.pan_changes.size())
        {
            break;
        }
        for (; next_update < voice.updates.size() && voice.updates[next_update] <= t; next_update++)
        {
        }
        double next = t + kUpdateMs;
        if (next_update < voice.updates.size())
        {
            next = std::min(next, voice.updates[next_update]);
        }
        if (next > ms || (voice.stop_ms && next >= *voice.stop_ms))
        {
            break;
        }
        t = next;
    }
    if (curve.size() == 1 && curve.front().offset == 0.0)
    {
        curve.clear();
    }

    return curve;
}

bool PitchMoves(const std::vector<PitchPoint>& curve, double wheel)
{
    constexpr double kSame = 1e-6;
    return curve.size() > 1 || std::abs(curve.front().semitones - wheel) > kSame;
}

bool AtHighestPitch(const MacroVoice& voice, const Sample& sample, int key)
{
    const PitchSettings settings{.key_shift = voice.key_shift,
                                 .fixed_key = voice.fixed_key,
                                 .detune_cents = voice.detune_cents,
                                 .rate_hz = voice.rate_hz};
    return DspPitch(settings, sample, key) > kHighestPitch;
}

double HighestPitchHz()
{
    return kHighestPitch / 4096.0 * kOutputRate;
}

std::optional<double> CurveSampleMs(const MacroVoice& voice, const Sample& sample, int key)
{
    // The DSP plays the pitch's 4096ths of a frame of the sample for each frame of the output, at kOutputRate.
    const PitchSettings settings{.key_shift = voice.key_shift,
                                 .fixed_key = voice.fixed_key,
                                 .detune_cents = voice.detune_cents,
                                 .rate_hz = voice.rate_hz};
    const double frames_per_ms = DspPitch(settings, sample, key) / 4096.0 * kOutputRate / 1000.0;
    double left = sample.pcm.Length();
    if (sample.pcm.loop || frames_per_ms <= 0.0)
    {
        return std::nullopt;
    }

    // The curve runs to four times the sample's length at the voice's settings, and a second more. The pitch stays at
    // its last point after that.
    const std::vector<PitchPoint> curve = PitchCurve(voice, sample, key, 0.0, 4.0 * left / frames_per_ms + 1000.0);
    if (!PitchMoves(curve, 0.0))
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < curve.size(); i++)
    {
        const double rate = frames_per_ms * std::exp2(curve[i].semitones / 12.0);
        const double span = i + 1 < curve.size() ? curve[i + 1].ms - curve[i].ms : kNever;
        if (rate * span >= left)
        {
            return curve[i].ms + left / rate;
        }
        left -= rate * span;
    }

    return curve.back().ms;
}

double VoiceVolume(const MacroVoice& voice, double volume, double controller)
{
    const double source = voice.source_fixed ? voice.source_volume : std::min(voice.source_volume * controller, 1.0);
    return std::min(voice.volume * volume + voice.volume_add, 1.0) * source;
}

std::optional<double> DecayedMs(const MacroVoice& voice)
{
    if (!voice.envelope || voice.envelope->dls || voice.envelope->sustain >= kSilentSustain)
    {
        return std::nullopt;
    }

    // The files' decay and release fall kEnergyFitDb in their times. A release longer than the decay falls more slowly.
    const Envelope& e = *voice.envelope;
    const bool slow_release = voice.key_off == KeyOff::kRelease && e.release_ms > e.decay_ms;

    return e.attack_ms + (slow_release ? e.decay_ms * kSoundFontFallDb / kEnergyFitDb : e.decay_ms);
}

std::optional<double> HeldSilenceMs(const MacroVoice& voice)
{
    // A ramp that leaves the volume at 0 does so whatever the volume was before it.
    std::optional<double> silent;
    if (!voice.held_ramps.empty() && RampedVolume(voice.held_ramps, 1.0, kNever) <= 0.0)
    {
        const VolumeRamp& last = voice.held_ramps.back();
        silent = last.start_ms + last.ms;
    }
    if (voice.stop_ms)
    {
        silent = std::min(silent.value_or(kNever), *voice.stop_ms + kCutMs);
    }
    if (const std::optional<double> decayed = DecayedMs(voice))
    {
        silent = std::min(silent.value_or(kNever), *decayed);
    }

    return silent;
}

bool FadesThroughRelease(const MacroVoice& voice)
{
    if (voice.held_ramps.empty())
    {
        return false;
    }

    // A voice that ignores the release fades to silence only if its ramps take it there.
    return voice.key_off == KeyOff::kAsHeld ||
           (voice.key_off == KeyOff::kIgnored && RampedVolume(voice.held_ramps, 1.0, kNever) <= 0.0);
}

std::vector<KeyVoice> VoicesOf(const Pool& pool, uint16_t object, int key)
{
    std::vector<KeyVoice> voices;
    AddVoices(pool, object, key, 64, 1.0, 0, voices);

    return voices;
}

double TableLevel(const VolumeTable& volumes, double volume)
{
    const double x = std::clamp(volume, 0.0, 1.0) * 127.0;
    const auto i = static_cast<std::size_t>(x);
    const double fraction = x - static_cast<double>(i);
    const double next = volumes[std::min(i + 1, volumes.size() - 1)];
    return volumes[i] + fraction * (next - volumes[i]);
}

double TableVolume(const VolumeTable& volumes, double level)
{
    // The entries up to 127 rise. The first above `level` ends the step that holds it.
    const auto end = volumes.begin() + 128;
    const auto above = std::upper_bound(volumes.begin(), end, level);
    if (above == volumes.begin())
    {
        return 0.0;
    }
    if (above == end)
    {
        return 1.0;
    }

    const auto i = static_cast<std::size_t>(above - volumes.begin()) - 1;
    return (static_cast<double>(i) + (level - volumes[i]) / (volumes[i + 1] - volumes[i])) / 127.0;
}

Instruments::Instruments(const SongGroup& group, const VolumeTable& volumes, std::map<uint16_t, std::string> sfz_names)
    : group_(group), volumes_(volumes), sfz_names_(std::move(sfz_names))
{
}

const MacroVoice& Instruments::Macro(uint16_t macro)
{
    auto it = macros_.find(macro);
    if (it == macros_.end())
    {
        it = macros_.emplace(macro, FollowMacro(group_.pool, macro, group_.volume_scale_divisor)).first;
    }

    return it->second;
}

const MacroVoice& Instruments::Macro(uint16_t macro, std::size_t part, double from_ms, const ChannelControls& controls)
{
    const MacroVoice& plain = Macro(macro);
    if (!controls.empty() && (!plain.controllers.empty() || !plain.splits.empty()))
    {
        // The values of the controllers that the macro reads give the voice.
        ChannelControls read;
        for (const auto& value : controls)
        {
            if (std::ranges::find(plain.controllers, value.first) != plain.controllers.end() ||
                std::ranges::find(plain.splits, value.first, &std::pair<uint8_t, uint8_t>::first) != plain.splits.end())
            {
                read.push_back(value);
            }
        }
        const std::tuple<uint16_t, std::size_t, double, ChannelControls> key{macro, part, from_ms, read};
        auto it = controlled_macros_.find(key);
        if (it == controlled_macros_.end())
        {
            const std::tuple<uint16_t, std::size_t, double, ChannelControls> whole_key{macro, 0, 0.0, read};
            auto whole = controlled_macros_.find(whole_key);
            if (whole == controlled_macros_.end())
            {
                whole = controlled_macros_
                            .emplace(whole_key, FollowMacro(group_.pool, macro, group_.volume_scale_divisor, read))
                            .first;
            }
            if (part == 0 && from_ms == 0.0)
            {
                return whole->second;
            }
            const MacroVoice& voice = whole->second;
            it =
                controlled_macros_.emplace(key, VoiceFrom(part == 0 ? voice : voice.parts.at(part - 1), from_ms)).first;
        }

        return it->second;
    }

    if (part == 0 && from_ms == 0.0)
    {
        return Macro(macro);
    }

    const std::tuple<uint16_t, std::size_t, double> key{macro, part, from_ms};
    auto it = later_macros_.find(key);
    if (it == later_macros_.end())
    {
        const MacroVoice& whole = Macro(macro);
        it = later_macros_.emplace(key, VoiceFrom(part == 0 ? whole : whole.parts.at(part - 1), from_ms)).first;
    }

    return it->second;
}

const MacroVoice& Instruments::Voice(const KeyVoice& voice)
{
    return Macro(voice.macro, voice.part, voice.from_ms, voice.controls);
}

const Instruments::ObjectReads& Instruments::Reads(uint16_t object)
{
    auto it = object_reads_.find(object);
    if (it == object_reads_.end())
    {
        ObjectReads reads;
        const auto add = [&reads](uint8_t c)
        {
            if (std::ranges::find(reads.controllers, c) == reads.controllers.end())
            {
                reads.controllers.push_back(c);
            }
        };

        for (int key = 0; key < 128; key++)
        {
            for (const KeyVoice& v : VoicesOf(group_.pool, object, key))
            {
                const MacroVoice& voice = Macro(v.macro);
                for (const uint8_t c : voice.controllers)
                {
                    add(c);
                    reads.whole.insert(c);
                }
                for (const auto& [c, from] : voice.splits)
                {
                    add(c);
                    if (c != kVelocityControl)
                    {
                        reads.turns[c].insert(from);
                        continue;
                    }

                    // The lowest velocity whose start volume reaches the split's.
                    int velocity = 0;
                    while (velocity < 128 && StartVolume(velocity, v.volume) < from)
                    {
                        velocity++;
                    }
                    if (velocity < 128)
                    {
                        reads.turns[c].insert(static_cast<uint8_t>(velocity));
                    }
                }
            }
        }
        std::ranges::sort(reads.controllers);
        it = object_reads_.emplace(object, std::move(reads)).first;
    }

    return it->second;
}

const std::vector<uint8_t>& Instruments::ObjectControllers(uint16_t object)
{
    return Reads(object).controllers;
}

uint8_t Instruments::ObjectValue(uint16_t object, uint8_t controller, uint8_t value)
{
    const ObjectReads& reads = Reads(object);
    if (reads.whole.contains(controller))
    {
        return value;
    }
    const auto turns = reads.turns.find(controller);
    if (turns == reads.turns.end())
    {
        return 0;
    }
    const auto above = turns->second.upper_bound(value);

    return above == turns->second.begin() ? 0 : *std::prev(above);
}

uint32_t Instruments::VariantBank(uint32_t bank, uint32_t program, const ChannelControls& controls)
{
    for (std::size_t i = 0; i < variants_.size(); i++)
    {
        const Variant& v = variants_[i];
        if (v.bank == bank && v.program == program && v.controls == controls)
        {
            return kVariantBank + static_cast<uint32_t>(i);
        }
    }
    variants_.push_back({bank, program, controls});

    return kVariantBank + static_cast<uint32_t>(variants_.size() - 1);
}

const Instruments::Variant* Instruments::VariantOf(uint32_t bank) const
{
    if (bank < kVariantBank || bank - kVariantBank >= variants_.size())
    {
        return nullptr;
    }

    return &variants_[bank - kVariantBank];
}

std::vector<EffectVoice> EffectVoicesOf(Instruments& instruments, uint16_t object, int key,
                                        std::set<std::string>& approximations)
{
    std::vector<EffectVoice> voices;
    for (const KeyVoice& v : VoicesOf(instruments.Group().pool, object, key))
    {
        voices.push_back({v, 0.0});
    }

    // A voice that a macro starts starts with the key of the voice that starts it, its pan and its share of the
    // volume. It shares that voice's controllers: it takes the volume controller that the voice has set, unless its
    // macro sets the controller. It can start more voices. A macro that starts itself would go on for ever: MusyX runs
    // out of voices.
    for (std::size_t i = 0; i < voices.size(); i++)
    {
        const EffectVoice from = voices[i];
        for (const StartedVoice& s : instruments.Macro(from.voice.macro).started)
        {
            if (s.step != 0)
            {
                approximations.insert("voices that start partway through a macro left out");
                continue;
            }
            if (voices.size() == kMostEffectVoices)
            {
                approximations.insert("voices beyond MusyX's 64 left out");
                return voices;
            }

            const int start_key = s.fixed_key.value_or(from.voice.key + s.key_shift) + s.key_add;
            const double controller =
                instruments.Macro(s.macro).volume_controller ? 1.0 : s.volume_controller.value_or(1.0);
            const KeyVoice started{.macro = s.macro,
                                   .key = std::clamp(start_key, 0, 127),
                                   .pan = from.voice.pan,
                                   .volume = from.voice.volume * controller};
            voices.push_back({started, from.start_ms + s.at_ms});
        }
    }

    // A voice that starts one sample after another plays its parts.
    std::vector<EffectVoice> parts;
    for (const EffectVoice& v : voices)
    {
        const std::size_t count = instruments.Macro(v.voice.macro).parts.size();
        if (count == 0)
        {
            parts.push_back(v);
        }
        for (std::size_t i = 1; i <= count; i++)
        {
            EffectVoice part = v;
            part.voice.part = i;
            parts.push_back(part);
        }
    }
    std::stable_sort(parts.begin(), parts.end(),
                     [](const EffectVoice& a, const EffectVoice& b) { return a.start_ms < b.start_ms; });

    return parts;
}

double Instruments::Level(const MacroVoice& voice, double volume) const
{
    return voice.dls_volume ? volume * volume : TableLevel(volumes_, volume);
}

const Sample* Instruments::GetSample(uint16_t id)
{
    auto it = samples_.find(id);
    if (it != samples_.end())
    {
        return it->second.get();
    }

    // The group's entry for the sample, or that of the first group pushed with it that holds one (SongGroup::pushed).
    const auto entry_in = [id](const SongGroup& group) -> const ntdlib::musyx::SampleInfo*
    {
        const auto found = std::find_if(group.samples.begin(), group.samples.end(),
                                        [id](const ntdlib::musyx::SampleInfo& s) { return s.id == id; });
        return found != group.samples.end() ? &*found : nullptr;
    };
    const SongGroup* holder = &group_;
    const ntdlib::musyx::SampleInfo* info = entry_in(group_);
    for (std::size_t p = 0; info == nullptr && p < group_.pushed.size(); p++)
    {
        holder = group_.pushed[p].get();
        info = entry_in(*holder);
    }

    // A voice whose sample isn't in the directory plays nothing in the game either.
    std::unique_ptr<Sample> sample;
    if (info != nullptr)
    {
        try
        {
            ntdlib::musyx::DecodedSample decoded = ntdlib::musyx::DecodeSample(*info, holder->sample_data);
            sample = std::make_unique<Sample>();
            sample->pcm.channels.push_back(std::move(decoded.pcm));
            sample->pcm.loop = decoded.loop && decoded.loop_start < decoded.loop_end;
            sample->pcm.loop_start = decoded.loop_start;
            sample->pcm.loop_end = decoded.loop_end;
            sample->pcm.sample_rate = static_cast<float>(info->rate);
            sample->key = info->key;
            sample->id = id;
            const auto named = sfz_names_.find(id);
            sample->sfz_name = named != sfz_names_.end() ? named->second : "s" + std::to_string(id);
        }
        catch (const xsflib::FormatError& e)
        {
            errors_.push_back(e.what());
        }
    }

    return samples_.emplace(id, std::move(sample)).first->second.get();
}

const Sample* Instruments::SampleOf(const MacroVoice& voice)
{
    return voice.sample ? GetSample(*voice.sample) : nullptr;
}

std::optional<double> Instruments::SampleSeconds(uint16_t id, double ratio)
{
    const Sample* sample = GetSample(id);
    if (sample == nullptr)
    {
        return 0.0;
    }
    if (sample->pcm.loop)
    {
        return std::nullopt;
    }

    // The DSP plays `ratio` times the sample's rate at the output's 32000 Hz. That output runs at kOutputRate.
    return sample->pcm.Length() / (sample->pcm.sample_rate * ratio * kOutputRate / kNominalRate);
}

} // namespace dolphinsf2::musyx
