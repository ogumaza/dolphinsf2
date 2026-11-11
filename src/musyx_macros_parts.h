// SPDX-License-Identifier: MIT

// The parts of MusyX's macro interpreter that musyx_macros.cpp and musyx_macro_steps.cpp share: the constants, the
// waits and the class MacroRun (see musyx_macros.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "xsflib/binary.h"

#include "musyx_game.h"
#include "musyx_macros.h"
#include "output_units.h"

namespace dolphinsf2::musyx::macros::run
{

using ntdlib::musyx::DlsAdsr;
using ntdlib::musyx::LinearAdsr;
using ntdlib::musyx::MacroOp;
using ntdlib::musyx::MacroStep;
using ntdlib::musyx::Pool;

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

// The centre of a signed source of a voice's inputs, from 0 to 1: 0x2000 of 0x3fff. A variable is such a source.
constexpr double kSignedCentre = kWheelCentre / static_cast<double>(kWheelHighest);

// The most sources that an input of a voice holds (inpAddCtrl, 0x80160F10 in Pokémon Colosseum).
constexpr std::size_t kMostSources = 4;

// The share that a select step `s` (VolumeSelect and the like) gives its source. Bytes 2 and 3 hold the share in
// percent, a signed number. Byte 6 holds a finer part in 256ths of a percent, with the share's sign. MusyX keeps the
// share in 65536ths. Its rounding moves the share by less than 1/65536.
inline double SelectShare(const MacroStep& s)
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

// The trap of a note's release, among a voice's three traps.
constexpr uint8_t kReleaseTrap = 0;

// The steps of 0.5 dB below full level that MusyX gives an amplitude of `amplitude` 1023rds (its table at 0x8036944C in
// Pokémon Colosseum): the nearest step. A step that falls halfway can come out one lower than the table's.
inline int SustainSteps(uint32_t amplitude)
{
    const uint32_t scaled = std::min<uint32_t>(amplitude, 0x3ff);
    if (scaled == 0)
    {
        return 193;
    }

    return std::clamp(static_cast<int>(std::lround(-40.0 * std::log10(scaled / 1023.0))), 0, 193);
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

// Whether MusyX reads controller `controller` of a song's channel as 0: controllers 96 to 101, the data buttons and
// the parameter numbers.
inline bool ReadsAsZero(uint8_t controller)
{
    return controller >= 0x60 && controller < 0x66;
}

// The value from 0 to 127 that MusyX reads for controller `controller` of a song's channel, whose controllers hold
// `controls`: the high 7 bits of the value that inpGetMidiCtrl returns. A controller that `controls` lacks holds its
// value after a cold reset of the channel. The switches from 64 to 69 read as 0 or 127, and controllers 96 to 101 as 0.
inline int ControllerValue(const ChannelControls& controls, uint8_t controller)
{
    if (ReadsAsZero(controller))
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
        value = kept == kVolumeController || kept == kExpressionController                       ? 127
                : kept == kPanController || kept == kPitchWheelControl || kept == kController132 ? 64
                                                                                                 : 0;
    }
    if (controller >= 0x40 && controller < 0x46)
    {
        return value < 0x40 ? 0 : 127;
    }

    return std::min(value, 127);
}

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

    // Run step `s` if its command is in the family of the function. Return nothing for another command. The families
    // are the commands that move through the macros, those of the pitch, those of the volume, those of the sample, the
    // pan and the voices that the voice starts, and those of the variables.
    std::optional<Next> FlowStep(const MacroStep& s);
    std::optional<Next> PitchStep(const MacroStep& s);
    std::optional<Next> VolumeStep(const MacroStep& s);
    std::optional<Next> VoiceStep(const MacroStep& s);
    std::optional<Next> VariableStep(const MacroStep& s);

    // Follow VolumeSelect step `s`, SetAdsrFromControllers step `s`, StartSample step `s` and SetVariable step `s`.
    void SelectVolume(const MacroStep& s);
    void SetControllerEnvelope(const MacroStep& s);
    void StartSample(const MacroStep& s);
    void SetVariable(const MacroStep& s);

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

} // namespace dolphinsf2::musyx::macros::run
