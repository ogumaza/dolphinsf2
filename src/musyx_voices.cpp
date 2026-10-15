// SPDX-License-Identifier: MIT

// MusyX's voices (see musyx_voices.h).
//
// A voice runs its macro's steps until a step waits. A wait's flags say when the wait ends: after a time, when the note
// is released, or when the sample ends. A trap sends the voice to another step when the note is released. FollowMacro
// runs the steps twice on a clock in milliseconds: for a note that the player holds, and for a note that the player
// releases after every wait of a fixed time. The first run gives the voice's settings and any stop of its own. The
// second gives what the release does.

#include "musyx_voices.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "xsflib/binary.h"

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

// A wait's time that never ends.
constexpr uint16_t kWaitForever = 0xffff;

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

// What a run of a macro's steps found.
struct Run
{
    MacroVoice voice;
    std::optional<double> release_at; // the first KeyOff, in milliseconds after the note's start
    std::optional<double> stop_at;    // the first StopSample
    bool sample_started = false;
    bool envelope_before_sample = false;
};

// Run `macro`'s steps for a note that the player releases `release_ms` after its start: kNever for a note that it
// holds.
Run RunMacro(const Pool& pool, uint16_t macro, double release_ms)
{
    Run run;
    MacroVoice& v = run.voice;
    const auto note = [&v](const std::string& what)
    {
        if (std::find(v.approximations.begin(), v.approximations.end(), what) == v.approximations.end())
        {
            v.approximations.push_back(what);
        }
    };

    auto macro_it = pool.macros.find(macro);
    if (macro_it == pool.macros.end())
    {
        return run;
    }

    const std::vector<MacroStep>* steps = &macro_it->second;
    std::size_t at = 0;
    double now = 0.0;
    bool released = release_ms <= 0.0;
    std::optional<std::pair<uint16_t, uint16_t>> trap;
    std::optional<uint8_t> tremolo_lfo;
    double lfo_period_ms[2] = {0.0, 0.0};
    double tremolo_scale = 0.0, tremolo_mod_add = 0.0;
    bool tremolo_set = false;
    double mod_vibrato_cents = 0.0;

    // Jump to step `step` of macro `id`. A macro that the pool lacks ends the voice.
    const auto jump = [&](uint16_t id, uint16_t step)
    {
        const auto target = pool.macros.find(id);
        if (target == pool.macros.end())
        {
            return false;
        }

        steps = &target->second;
        at = step;
        return true;
    };

    for (int count = 0; count < kMostSteps && at < steps->size(); count++)
    {
        const MacroStep s = (*steps)[at++];
        switch (s.Op())
        {
        case MacroOp::kEnd:
        case MacroOp::kStop:
            return run;

        case MacroOp::kGoto:
            if (!jump(s.Half(2), s.Half(4)))
            {
                return run;
            }
            break;

        case MacroOp::kTrapEvent:
            if (s.Byte(1) == kReleaseTrap && pool.macros.contains(s.Half(2)))
            {
                trap = std::make_pair(s.Half(2), s.Half(4));
            }
            else if (s.Byte(1) != kReleaseTrap)
            {
                note("traps of a sample's end or of messages left out");
            }
            break;

        case MacroOp::kUntrapEvent:
            if (s.Byte(1) == kReleaseTrap)
            {
                trap.reset();
            }
            break;

        case MacroOp::kWaitTicks:
        case MacroOp::kWaitMs:
        case MacroOp::kAddKey:
        case MacroOp::kSetKey:
            {
                const MacroOp op = s.Op();
                if (op == MacroOp::kAddKey)
                {
                    const auto add = static_cast<int8_t>(s.Byte(1));
                    v.key_shift = s.Byte(3) != 0 ? add : v.key_shift + add;
                    v.detune_cents = static_cast<int8_t>(s.Byte(2));
                    if (v.fixed_key)
                    {
                        v.fixed_key = s.Byte(3) != 0 ? std::nullopt : std::optional<int>(*v.fixed_key + add);
                    }
                }
                else if (op == MacroOp::kSetKey)
                {
                    v.fixed_key = s.Byte(1) & 0x7f;
                    v.detune_cents = static_cast<int8_t>(s.Byte(2));
                }

                // The key commands wait without flags.
                const bool key_command = op == MacroOp::kAddKey || op == MacroOp::kSetKey;
                const bool on_release = !key_command && (s.Byte(1) & 1) != 0;
                const bool on_sample_end = !key_command && (s.Byte(3) & 1) != 0;
                const bool absolute = (s.Byte(4) & 1) != 0;
                const bool ms = op == MacroOp::kWaitMs || (s.Byte(5) & 1) != 0;
                const uint16_t time = s.Half(6);
                if (time == 0)
                {
                    break;
                }
                if (!key_command && (s.Byte(2) & 1) != 0)
                {
                    note("random waits take their longest time");
                }
                if (!ms)
                {
                    note("waits in ticks take a tempo of 120");
                }

                // A wait for the release ends at once after the release, and one for the sample's end at once when the
                // sample isn't playing.
                if ((on_release && released) || (on_sample_end && !run.sample_started))
                {
                    break;
                }

                // A tick at 120 beats a minute lasts 1000 / 768 ms.
                const double length = time == kWaitForever ? kNever : (ms ? time : time * 1000.0 / 768.0);
                double end = absolute ? length : now + length;
                if (on_release)
                {
                    end = std::min(end, std::max(now, release_ms));
                }

                // A trap of the release jumps when the note is released during the wait.
                if (trap && !released && release_ms <= end)
                {
                    now = std::max(now, release_ms);
                    released = true;
                    const auto [id, step] = *trap;
                    trap.reset();
                    jump(id, step);
                    break;
                }
                if (end == kNever)
                {
                    // The voice waits for ever, or for its sample's end. Nothing it does later changes its sound.
                    return run;
                }

                now = std::max(now, end);
                released = released || release_ms <= now;
                break;
            }

        case MacroOp::kStartSample:
            if (s.Byte(3) != 0 || s.word1 != 0)
            {
                note("samples that start after their first sample play from their start");
            }
            if (!run.sample_started)
            {
                v.sample = s.Half(1);
                run.sample_started = true;
                run.envelope_before_sample = v.envelope.has_value();
            }
            else
            {
                note("a second sample of a voice left out");
            }
            break;

        case MacroOp::kStopSample:
            if (!run.stop_at)
            {
                run.stop_at = now;
            }
            break;

        case MacroOp::kKeyOff:
            if (!run.release_at)
            {
                run.release_at = now;
            }
            break;

        case MacroOp::kSetAdsr:
            v.envelope = EnvelopeOf(pool, s.Half(1), s.Byte(3) != 0);
            if (v.envelope && !v.envelope->dls && (v.envelope->decay_ms > 0.0 || v.envelope->release_ms > 0.0))
            {
                note("linear envelopes' decays and releases fall in decibels");
            }
            break;

        case MacroOp::kSetPitchAdsr:
            {
                if (const std::optional<Envelope> e = EnvelopeOf(pool, s.Half(1), true))
                {
                    note("pitch envelopes fall in a line, not in decibels");
                    const auto keys = static_cast<int8_t>(s.Byte(4));
                    const auto cents = static_cast<int8_t>(s.Byte(5));
                    const double range = keys * 100.0 + (keys >= 0 ? cents : -cents);
                    v.pitch_envelope = PitchEnvelope{*e, range};
                }
                break;
            }

        case MacroOp::kScaleVolumeDls:
            v.volume = (s.Byte(3) != 0 ? 1.0 : v.volume) * s.Half(1) / 4096.0;
            break;

        case MacroOp::kScaleVolume:
            v.volume = (s.Byte(5) != 0 ? 1.0 : v.volume) * s.Byte(1) / 127.0;
            if (s.Byte(2) != 0 || s.Byte(3) != 0 || s.Byte(4) != 0)
            {
                note("volume curves and volume additions left out");
            }
            break;

        case MacroOp::kModeSelect:
            v.dls_volume = s.Byte(1) != 0;
            v.itd = s.Byte(2) == 0;
            break;

        case MacroOp::kVibrato:
            {
                const uint16_t time = s.Half(6);
                if (time == 0)
                {
                    v.vibrato.reset();
                    break;
                }

                // A negative key range turns the vibrato's phase. A negative cent range with no keys does too.
                auto keys = static_cast<int8_t>(s.Byte(1));
                auto cents = static_cast<int8_t>(s.Byte(2));
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
                    note("vibratos that start at the bottom of their sine start at the top");
                }

                Vibrato vib;
                vib.delay_ms = now;
                vib.period_ms = (s.Byte(5) & 1) != 0 ? time : time * 1000.0 / 768.0;
                vib.depth_cents = key_range * 100.0 + cent_range;
                vib.mod_depth_cents = mod_vibrato_cents;
                vib.mod_scales_depth = (s.Byte(3) & 3) != 0;
                v.vibrato = vib;
                break;
            }

        case MacroOp::kModulationToVibrato:
            {
                // MusyX keeps the depth in 256ths of a semitone.
                const auto keys = static_cast<int8_t>(s.Byte(1));
                const auto cents = static_cast<int8_t>(s.Byte(2));
                const int scale = keys * 256 + (keys >= 0 ? cents * 256 / 100 : -(cents * 256 / 100));
                mod_vibrato_cents = scale * 100.0 / 256.0;
                if (v.vibrato)
                {
                    v.vibrato->mod_depth_cents = mod_vibrato_cents;
                }
                break;
            }

        case MacroOp::kSetupLfo:
            if (s.Byte(1) < 2)
            {
                lfo_period_ms[s.Byte(1)] = s.Half(2);
            }
            break;

        case MacroOp::kTremoloSelect:
            // Controllers 0x82 and 0x83 are the voice's two LFOs.
            if (s.Byte(1) == 0x82 || s.Byte(1) == 0x83)
            {
                tremolo_lfo = static_cast<uint8_t>(s.Byte(1) - 0x82);
                if (static_cast<int16_t>(s.Half(2)) != 100)
                {
                    note("tremolos at other than 100% of their LFO take 100%");
                }
            }
            else
            {
                note("tremolos from controllers other than an LFO left out");
            }
            break;

        case MacroOp::kSetupTremolo:
            tremolo_scale = s.Half(1) / 4096.0;
            tremolo_mod_add = (s.word1 & 0xffff) / 4096.0;
            tremolo_set = true;
            break;

        case MacroOp::kSetKeyGroup:
            // A voice that sets a key group stops the group's other voices at once, or releases them. A SoundFont's
            // exclusive class stops them.
            v.key_group = s.Byte(1);
            if (v.key_group != 0 && s.Byte(2) == 0)
            {
                note("key groups that release their voices stop them");
            }
            break;

        // The voice's priority and age only decide which voice to take when the voices run out. The resampler's
        // filter and the priority of the channel change nothing a SoundFont plays.
        case MacroOp::kSetPriority:
        case MacroOp::kAddPriority:
        case MacroOp::kAddAgeCounter:
        case MacroOp::kSetAgeCounter:
        case MacroOp::kAgeCounterSpeed:
        case MacroOp::kAgeCounterByVolume:
        case MacroOp::kSrcModeSelect:
            break;

        default:
            note("SoundMacro commands that dolphinsf2 doesn't follow left out");
            break;
        }

        // A release can come while a step runs: a trap then takes the voice on from the next step.
        if (trap && !released && release_ms <= now)
        {
            released = true;
            const auto [id, step] = *trap;
            trap.reset();
            if (!jump(id, step))
            {
                return run;
            }
        }
    }

    // The tremolo: its LFO swings between 0 and 1, and the volume drops by that times its depth. Without the mod wheel,
    // the depth is 1 minus the tremolo's scale. The mod wheel at its top multiplies the scale by the addition.
    if (tremolo_set && tremolo_lfo && lfo_period_ms[*tremolo_lfo] > 0.0)
    {
        v.tremolo = Tremolo{lfo_period_ms[*tremolo_lfo], 1.0 - tremolo_scale, 1.0 - tremolo_scale * tremolo_mod_add};
    }

    return run;
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
            // A key of a keymap that plays a keymap plays nothing. Its pan moves the note's from the centre.
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

            const int entry_pan = e.pan < 0x80 ? std::clamp(e.pan - 64 + pan, 0, 127) : pan;
            AddVoices(pool, e.object, std::clamp(key + e.transpose, 0, 127), entry_pan, volume, level + 1, out);
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

                const int entry_pan = e.pan < 0x80 ? std::clamp(e.pan - 64 + pan, 0, 127) : pan;
                AddVoices(pool, e.object, std::clamp(key + e.transpose, 0, 127), entry_pan, volume * e.volume / 127.0,
                          level + 1, out);
            }
            break;
        }
    }
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

MacroVoice FollowMacro(const Pool& pool, uint16_t macro)
{
    // The held note gives the settings, and any stop that the voice makes by itself.
    Run held = RunMacro(pool, macro, kNever);
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
    if (voice.envelope && !held.envelope_before_sample)
    {
        voice.approximations.push_back("envelopes set after their sample starts apply from its start");
    }
    if (voice.sample && !voice.dls_volume)
    {
        voice.approximations.push_back("voices on MusyX's volume curve play on the DLS curve");
    }

    // A release late in the note: after every wait of a fixed time, or halfway to the voice's stop of its own.
    const double late_ms = voice.stop_ms ? *voice.stop_ms / 2.0 : 60000.0;
    const Run late = RunMacro(pool, macro, late_ms);
    const bool cut =
        late.stop_at && *late.stop_at >= late_ms && (!late.release_at || *late.stop_at <= *late.release_at);
    if (cut)
    {
        voice.key_off = KeyOff::kCut;
    }
    else if (late.release_at && *late.release_at >= late_ms)
    {
        voice.key_off = KeyOff::kRelease;
    }

    // A release early in the note should do the same.
    constexpr double kEarly = 1.0;
    const Run early = RunMacro(pool, macro, kEarly);
    const bool early_cut = early.stop_at && (!early.release_at || *early.stop_at <= *early.release_at);
    const KeyOff early_key_off = early_cut ? KeyOff::kCut : early.release_at ? KeyOff::kRelease : KeyOff::kIgnored;
    if (early_key_off != voice.key_off)
    {
        voice.approximations.push_back("releases early in a note act as late ones do");
    }

    return voice;
}

std::vector<KeyVoice> VoicesOf(const Pool& pool, uint16_t object, int key)
{
    std::vector<KeyVoice> voices;
    AddVoices(pool, object, key, 64, 1.0, 0, voices);

    return voices;
}

Instruments::Instruments(const SongGroup& group) : group_(group)
{
}

const MacroVoice& Instruments::Macro(uint16_t macro)
{
    auto it = macros_.find(macro);
    if (it == macros_.end())
    {
        it = macros_.emplace(macro, FollowMacro(group_.pool, macro)).first;
    }

    return it->second;
}

const Sample* Instruments::GetSample(uint16_t id)
{
    auto it = samples_.find(id);
    if (it != samples_.end())
    {
        return it->second.get();
    }

    std::unique_ptr<Sample> sample;
    const auto info = std::find_if(group_.samples.begin(), group_.samples.end(),
                                   [id](const ntdlib::musyx::SampleInfo& s) { return s.id == id; });
    // A voice whose sample isn't in the directory plays nothing in the game either.
    if (info != group_.samples.end())
    {
        try
        {
            ntdlib::musyx::DecodedSample decoded = ntdlib::musyx::DecodeSample(*info, group_.sample_data);
            sample = std::make_unique<Sample>();
            sample->pcm.channels.push_back(std::move(decoded.pcm));
            sample->pcm.loop = decoded.loop && decoded.loop_start < decoded.loop_end;
            sample->pcm.loop_start = decoded.loop_start;
            sample->pcm.loop_end = decoded.loop_end;
            sample->pcm.sample_rate = static_cast<float>(info->rate);
            sample->key = info->key;
            sample->id = id;
        }
        catch (const xsflib::FormatError& e)
        {
            errors_.push_back(e.what());
        }
    }

    return samples_.emplace(id, std::move(sample)).first->second.get();
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
