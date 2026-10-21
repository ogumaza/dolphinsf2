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
// holds.
class MacroRun
{
public:
    MacroRun(const Pool& pool, double release_ms) : pool_(pool), release_ms_(release_ms), released_(release_ms <= 0.0)
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

    const Pool& pool_;
    const double release_ms_;
    Run run_;
    Place place_;
    double now_ = 0.0;
    bool released_;
    std::optional<Place> trap_; // where the trap of the release goes on

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
        return Pass(KeyWaitOf(s));

    case MacroOp::kPianoPan:
        // Byte 1 holds the scale, byte 2 the centre key and byte 3 the centre pan.
        v.piano_pan = PianoPan{.scale = static_cast<int8_t>(s.Byte(1)),
                               .centre_key = s.Byte(2),
                               .centre_pan = s.Byte(3),
                               .key_shift = v.key_shift,
                               .fixed_key = v.fixed_key};
        break;

    case MacroOp::kStartSample:
        if (s.Byte(3) != 0 || s.word1 != 0)
        {
            Note("samples that start after their first sample play from their start");
        }
        if (!run_.sample_started)
        {
            v.sample = s.Half(1);
            run_.sample_started = true;
            run_.envelope_before_sample = v.envelope.has_value();
        }
        else
        {
            Note("a second sample of a voice left out");
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
        v.volume = (s.Byte(3) != 0 ? 1.0 : v.volume) * s.Half(1) / 4096.0;
        break;

    case MacroOp::kScaleVolume:
        v.volume = (s.Byte(5) != 0 ? 1.0 : v.volume) * s.Byte(1) / 127.0;
        if (s.Byte(2) != 0 || s.Byte(3) != 0 || s.Byte(4) != 0)
        {
            Note("volume curves and volume additions left out");
        }
        break;

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

    default:
        Note("SoundMacro commands that dolphinsf2 doesn't follow left out");
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

    // A tick at 120 beats a minute lasts 1000 / 768 ms.
    const double length = wait.time == kWaitForever ? kNever : (wait.ms ? wait.time : wait.time * 1000.0 / 768.0);
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
    vib.period_ms = (s.Byte(5) & 1) != 0 ? time : time * 1000.0 / 768.0;
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
// holds.
Run RunMacro(const Pool& pool, uint16_t macro, double release_ms)
{
    return MacroRun(pool, release_ms).Follow(macro);
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

Instruments::Instruments(const SongGroup& group, const VolumeTable& volumes) : group_(group), volumes_(volumes)
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
