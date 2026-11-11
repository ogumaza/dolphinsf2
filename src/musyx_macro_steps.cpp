// SPDX-License-Identifier: MIT

// The work of MusyX's macro steps (see musyx_macros.cpp): the volume selects, the controllers' envelopes, the samples,
// the variables, the pitch wheel, jumps and traps, waits and loops, the fade-in and the vibrato.

#include "musyx_macros_parts.h"

namespace dolphinsf2::musyx::macros::run
{

namespace
{

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

} // namespace

void MacroRun::SelectVolume(const MacroStep& s)
{
    MacroVoice& v = run_.voice;

    // Byte 1 names the controller (SelectShare has the share). Byte 4 picks how the source joins the others: 0 replaces
    // them. Byte 5 makes byte 1 name a variable. MusyX starts a voice on the volume controller in full. The voice's
    // first VolumeSelect replaces that source. Byte 4 changes nothing there (0x80153ee8 in Pokémon Colosseum,
    // 0x8010174c in Mario Party 4).
    const uint8_t combine = volume_selected_ ? s.Byte(4) : 0;
    volume_selected_ = true;
    if (combine != 0 || (s.Byte(5) == 0 && s.Byte(1) != kVolumeController) || (s.Byte(5) != 0 && variables_set_))
    {
        Note("volume sources other than a share of the volume controller left out");
    }
    else if (s.Byte(5) != 0)
    {
        // MusyX takes a variable as a signed source around half of full volume (0x80161134 in Pokémon Colosseum). A
        // macro's variables start at 0. The voice's volume therefore takes half of full volume.
        v.source_volume = kSignedCentre;
        v.source_fixed = true;
    }
    else
    {
        v.source_volume = std::max(SelectShare(s), 0.0);
        v.source_fixed = false;
    }
}

void MacroRun::SetControllerEnvelope(const MacroStep& s)
{
    MacroVoice& v = run_.voice;

    // Bytes 1, 2, 3 and 4 name the controllers of the attack, the decay, the sustain and the release. A song's voice
    // takes their values from its channel when the step runs. The files take them at the note's start.
    const std::array<uint8_t, 4> numbers{s.Byte(1), s.Byte(2), s.Byte(3), s.Byte(4)};
    for (const uint8_t c : numbers)
    {
        if (ReadsAsZero(c))
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
        return;
    }

    v.envelope = ControllerEnvelope(ControllerValue(controls_, numbers[0]), ControllerValue(controls_, numbers[1]),
                                    ControllerValue(controls_, numbers[2]), ControllerValue(controls_, numbers[3]));
}

void MacroRun::StartSample(const MacroStep& s)
{
    MacroVoice& v = run_.voice;
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
}

void MacroRun::SetVariable(const MacroStep& s)
{
    MacroVoice& v = run_.voice;
    variables_set_ = variables_set_ || s.Byte(1) == 0;
    if (s.Byte(1) == 0)
    {
        variables_[s.Byte(2) & kVariableMask] = static_cast<int16_t>(s.Half(4));
    }
    else
    {
        const double value = std::min<int>(s.Half(4), kWheelHighest) / static_cast<double>(kWheelHighest);
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
    const std::optional<int32_t> b =
        s.Op() == MacroOp::kAddImmediate ? std::optional<int32_t>(static_cast<int16_t>(s.Half(5))) : VariableAt(s, 5);
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
    run_.voice.pitch_changes.push_back(PitchSettingsOf(run_.voice, now_));
}

void MacroRun::Note(const std::string& what)
{
    std::vector<std::string>& notes = run_.voice.approximations;
    if (std::find(notes.begin(), notes.end(), what) == notes.end())
    {
        notes.push_back(what);
    }
}

} // namespace dolphinsf2::musyx::macros::run
