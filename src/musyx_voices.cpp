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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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

// The steps that a run follows at most. A loop that never waits would run forever. The game runs at most 32 steps of a
// voice in each update.
constexpr int kMostSteps = 4096;

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

// The steps of 0.5 dB below full level of a DLS envelope's sustain, whose table gives `level`. MusyX squares the level
// in 128ths, then finds the nearest step of its amplitude in 1023rds.
int DlsSustainSteps(uint16_t level)
{
    const int x = std::min(level >> 5, 127);
    const auto amplitude = static_cast<uint32_t>(4096.0f * static_cast<float>(x * x) / (127.0f * 127.0f));
    const uint32_t scaled = std::min<uint32_t>(amplitude >> 2, 0x3ff);
    if (scaled == 0)
    {
        return 193;
    }

    return std::clamp(static_cast<int>(std::lround(-40.0 * std::log10(scaled / 1023.0))), 0, 193);
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
    MacroRun(const Pool& pool, double release_ms, int volume_scale_divisor)
        : pool_(pool),
          release_ms_(release_ms),
          volume_scale_divisor_(volume_scale_divisor),
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

    // Run step `s`.
    Next Step(const MacroStep& s);

    // Go on at step `step` of macro `macro`. Return false if the pool lacks the macro.
    bool Jump(uint16_t macro, uint16_t step);

    // Release the note. The run goes on where the trap of the release points.
    void TakeTrap();

    // Let the time of `wait` pass. A release during the wait takes the trap of the release. Return kEnd if the voice
    // waits for ever.
    Next Pass(const Wait& wait);

    // Set up the vibrato of Vibrato step `s`, or turn it off.
    void SetVibrato(const MacroStep& s);

    // Add `what` to the voice's approximations, once.
    void Note(const std::string& what);

    // Add the part of the latest sample to the run's parts, with the voice's settings now, if it isn't there yet. A
    // part's settings are those when time first passes after its sample starts.
    void SettlePart();

    const Pool& pool_;
    const double release_ms_;
    const int volume_scale_divisor_;
    Run run_;
    Place place_;
    double now_ = 0.0;
    bool released_;
    std::optional<Place> trap_; // where the trap of the release goes on

    // The latest sample and its start, until its part settles (SettlePart).
    struct OpenPart
    {
        uint16_t sample = 0;
        double start_ms = 0.0;
    };
    std::optional<OpenPart> open_part_;

    // The tremolo's setup: the LFO that it follows, the two LFOs' periods, and SetupTremolo's scale and addition.
    std::optional<uint8_t> tremolo_lfo_;
    double lfo_period_ms_[2] = {0.0, 0.0};
    double tremolo_scale_ = 0.0;
    double tremolo_mod_add_ = 0.0;
    bool tremolo_set_ = false;

    double mod_vibrato_cents_ = 0.0; // the vibrato depth that the mod wheel adds at its top (ModulationToVibrato)
};

Run MacroRun::Follow(uint16_t macro)
{
    if (!Jump(macro, 0))
    {
        return std::move(run_);
    }

    for (int count = 0; count < kMostSteps && place_.at < place_.steps->size(); count++)
    {
        if (Step((*place_.steps)[place_.at++]) == Next::kEnd)
        {
            SettlePart();
            return std::move(run_);
        }

        // A release can come while a step runs: a trap then takes the voice on from the next step.
        if (trap_ && !released_ && release_ms_ <= now_)
        {
            TakeTrap();
        }
    }

    // The tremolo: its LFO swings between 0 and 1, and the volume drops by that times its depth. Without the mod wheel,
    // the depth is 1 minus the tremolo's scale. The mod wheel at its top multiplies the scale by the addition.
    if (tremolo_set_ && tremolo_lfo_ && lfo_period_ms_[*tremolo_lfo_] > 0.0)
    {
        run_.voice.tremolo =
            Tremolo{lfo_period_ms_[*tremolo_lfo_], 1.0 - tremolo_scale_, 1.0 - tremolo_scale_ * tremolo_mod_add_};
    }
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
            return Pass(KeyWaitOf(s));
        }

    case MacroOp::kSetKey:
        v.fixed_key = s.Byte(1) & 0x7f;
        v.detune_cents = static_cast<int8_t>(s.Byte(2));
        v.rate_hz.reset();
        return Pass(KeyWaitOf(s));

    case MacroOp::kSetPitch:
        // Bytes 1 to 3 hold the rate in Hz. The games' steps leave bytes 4 to 7 at 0. A step that sets them is
        // reported.
        v.rate_hz = (s.Byte(1) << 16) | (s.Byte(2) << 8) | s.Byte(3);
        if (s.Byte(4) != 0 || s.Byte(5) != 0 || s.Byte(6) != 0 || s.Byte(7) != 0)
        {
            Note(kCommandsLeftOut);
        }
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
        // Byte 1 names the controller and bytes 2 and 3 hold its share in percent. Byte 4 picks how the source joins
        // the others: 0 replaces them. Byte 5 makes byte 1 name a variable. Byte 6 holds a finer part of the share.
        // MusyX starts a voice on the volume controller in full.
        if (s.Byte(1) == kVolumeController && s.Byte(4) == 0 && s.Byte(5) == 0 && s.Byte(6) == 0)
        {
            v.source_volume = std::max(static_cast<int16_t>(s.Half(2)), int16_t{0}) / 100.0;
        }
        else
        {
            Note("volume sources other than a share of the volume controller left out");
        }
        break;

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
        if (!run_.sample_started)
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
        if (s.Byte(1) < 2)
        {
            lfo_period_ms_[s.Byte(1)] = s.Half(2);
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
    // variable changes the sound only through a step that reads it. Such a step is reported.
    case MacroOp::kSetVariable:
        if (s.Byte(1) != 0)
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
        if (s.Byte(1) != 0)
        {
            Note(kControllersLeftOut);
        }
        break;

    default:
        Note(kCommandsLeftOut);
        break;
    }

    return Next::kStep;
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
    SettlePart();

    const double length = wait.time == kWaitForever ? kNever : (wait.ms ? wait.time : wait.time * kTickMs);
    double end = wait.absolute ? length : now_ + length;
    if (wait.on_release)
    {
        end = std::min(end, std::max(now_, release_ms_));
    }

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

    return Next::kStep;
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
Run RunMacro(const Pool& pool, uint16_t macro, double release_ms, int volume_scale_divisor)
{
    return MacroRun(pool, release_ms, volume_scale_divisor).Follow(macro);
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
            out.push_back({object, key, pan, volume});
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

// What a release `release_ms` after a note's start does in `run`, a run of the macro for that release. `held` is the
// run for a held note. A voice that plays on after the release until a StopSample fades (kFade). A voice that stops at
// the release is cut. A voice that stops by itself at the same time as a held note does ignores the release
// (kAsHeld). A KeyOff after the release releases the envelope.
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
        return KeyOff::kRelease;
    }

    return KeyOff::kIgnored;
}

// The fade of a voice whose macro runs as `run` for a release `release_ms` after the note's start (KeyOff::kFade): the
// ramps from the release to the stop.
ReleaseFade FadeOf(const Run& run, double release_ms)
{
    ReleaseFade fade;
    fade.stop_ms = *run.stop_at - release_ms;
    for (const VolumeRamp& r : run.ramps)
    {
        if (r.start_ms >= release_ms && r.start_ms < *run.stop_at)
        {
            fade.ramps.push_back({r.start_ms - release_ms, r.ms, r.scale, r.add});
        }
    }

    return fade;
}

} // namespace

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

MacroVoice FollowMacro(const Pool& pool, uint16_t macro, int volume_scale_divisor)
{
    // The held note gives the settings, the fades of its volume, and any stop that the voice makes by itself.
    Run held = RunMacro(pool, macro, kNever, volume_scale_divisor);
    MacroVoice voice = std::move(held.voice);
    if (held.stop_at)
    {
        voice.stop_ms = *held.stop_at;
    }
    else if (held.release_at)
    {
        voice.stop_ms = *held.release_at;
        voice.approximations.push_back("voices that release themselves stop instead");
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

    // A release late in the note: after every wait of a fixed time, or halfway to the voice's stop of its own.
    const double late_ms = voice.stop_ms ? *voice.stop_ms / 2.0 : 60000.0;
    const Run late = RunMacro(pool, macro, late_ms, volume_scale_divisor);
    voice.key_off = KeyOffOf(late, late_ms, held);
    if (voice.key_off == KeyOff::kFade)
    {
        voice.release_fade = FadeOf(late, late_ms);
    }

    // A release early in the note should do the same.
    constexpr double kEarly = 1.0;
    const Run early = RunMacro(pool, macro, kEarly, volume_scale_divisor);
    if (KeyOffOf(early, kEarly, held) != voice.key_off)
    {
        voice.approximations.push_back("releases early in a note act as late ones do");
    }

    // A part of a voice that starts one sample after another stops where the next part starts. The last stops where
    // the voice does. Each part follows the voice's ramps, tremolo and release.
    if (held.parts.size() > 1)
    {
        for (std::size_t i = 0; i < held.parts.size(); i++)
        {
            MacroVoice& part = held.parts[i];
            part.stop_ms = i + 1 < held.parts.size() ? held.parts[i + 1].sample_ms : voice.stop_ms;
            part.held_ramps = voice.held_ramps;
            part.tremolo = voice.tremolo;
            part.key_off = voice.key_off;
            part.release_fade = voice.release_fade;
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

    return later;
}

double VoiceVolume(const MacroVoice& voice, double volume, double controller)
{
    return std::min(voice.volume * volume + voice.volume_add, 1.0) * std::min(voice.source_volume * controller, 1.0);
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

const MacroVoice& Instruments::Macro(uint16_t macro, std::size_t part, double from_ms)
{
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
    return Macro(voice.macro, voice.part, voice.from_ms);
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
