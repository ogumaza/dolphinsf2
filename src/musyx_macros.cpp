// SPDX-License-Identifier: MIT

// The interpreter of MusyX's macros (see musyx_macros.h). A run follows the steps of a voice's macro on a clock in
// milliseconds until the macro ends.

#include "musyx_macros.h"

#include "musyx_macros_parts.h"

namespace dolphinsf2::musyx::macros::run
{

namespace
{

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
    return SustainSteps(amplitude >> 2);
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

} // namespace

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
    for (const auto family : {&MacroRun::FlowStep, &MacroRun::PitchStep, &MacroRun::VolumeStep, &MacroRun::VoiceStep,
                              &MacroRun::VariableStep})
    {
        if (const std::optional<Next> next = (this->*family)(s))
        {
            return *next;
        }
    }

    // A command of none of the families is left out.
    Note(kCommandsLeftOut);

    return Next::kStep;
}

std::optional<MacroRun::Next> MacroRun::FlowStep(const MacroStep& s)
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

    case MacroOp::kSendMessage:
        // Bytes 2 and 3 name the macro whose voices get the message, unless byte 1 makes them name a variable.
        if (s.Byte(1) != 0 || s.Half(2) != kNoMacro)
        {
            Note(kCommandsLeftOut);
        }
        break;

    default:
        return std::nullopt;
    }

    return Next::kStep;
}

std::optional<MacroRun::Next> MacroRun::PitchStep(const MacroStep& s)
{
    MacroVoice& v = run_.voice;
    switch (s.Op())
    {
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

    case MacroOp::kPitchWheelSelect:
        {
            // Byte 1 names the source's controller, or with byte 5 one of the voice's variables. SelectShare gives the
            // share. Byte 4 picks how the source joins those before it: 0 in their place, 1 added, 2 multiplied, 3
            // taken away. The voice's first PitchWheelSelect replaces the pitch wheel of the channel
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

    default:
        return std::nullopt;
    }

    return Next::kStep;
}

std::optional<MacroRun::Next> MacroRun::VolumeStep(const MacroStep& s)
{
    MacroVoice& v = run_.voice;
    switch (s.Op())
    {
    case MacroOp::kFadeIn:
        FadeIn(s);
        break;

    case MacroOp::kVolumeSelect:
        SelectVolume(s);
        break;

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
        SetControllerEnvelope(s);
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

    case MacroOp::kTremoloSelect:
        if (s.Byte(1) == kLfo0Control || s.Byte(1) == kLfo1Control)
        {
            tremolo_lfo_ = static_cast<uint8_t>(s.Byte(1) - kLfo0Control);
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
        tremolo_mod_add_ = s.Half(4) / 4096.0;
        tremolo_set_ = true;
        break;

    default:
        return std::nullopt;
    }

    return Next::kStep;
}

std::optional<MacroRun::Next> MacroRun::VoiceStep(const MacroStep& s)
{
    MacroVoice& v = run_.voice;
    switch (s.Op())
    {
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

    case MacroOp::kStartSample:
        StartSample(s);
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

    default:
        return std::nullopt;
    }

    return Next::kStep;
}

std::optional<MacroRun::Next> MacroRun::VariableStep(const MacroStep& s)
{
    switch (s.Op())
    {
    // SetVariable sets a variable, or with byte 1 a MIDI controller that byte 2 names. Bytes 4 and 5 hold the value. A
    // variable changes the sound only through a step that reads it.
    case MacroOp::kSetVariable:
        SetVariable(s);
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
        return std::nullopt;
    }

    return Next::kStep;
}

} // namespace dolphinsf2::musyx::macros::run

namespace dolphinsf2::musyx::macros
{

using run::MacroRun;
using run::Pool;

// Run `macro`'s steps for a note that the player releases `release_ms` after its start: kNever for a note that it
// holds. ScaleVolume divides by `volume_scale_divisor`.
Run RunMacro(const Pool& pool, uint16_t macro, double release_ms, int volume_scale_divisor,
             const ChannelControls& controls)
{
    return MacroRun(pool, release_ms, volume_scale_divisor, controls).Follow(macro);
}

} // namespace dolphinsf2::musyx::macros