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
#include <initializer_list>
#include <iterator>
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
#include "musyx_macros.h"
#include "output_units.h"

namespace dolphinsf2::musyx
{
namespace
{

using macros::kLaterSamplesLeftOut;
using macros::kNever;
using macros::PitchSettingsOf;
using macros::Run;
using macros::RunMacro;
using ntdlib::musyx::KindOf;
using ntdlib::musyx::ObjectKind;
using ntdlib::musyx::Pool;

// The depth of the layers and keymaps that a note goes through at most. A layer can start a layer.
constexpr int kMostLevels = 8;

// The voices that MusyX has, and so the most that an effect can play.
constexpr std::size_t kMostEffectVoices = 64;

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

// The time of the update of `voice` after the one at `t` milliseconds: 15 ms after it, or at the end of a wait before
// then. `next_update` is the index of the first update (MacroVoice::updates) after `t`. The function moves it on.
// Nothing if that time comes after `ms`, or at or after the voice's stop.
std::optional<double> NextUpdate(const MacroVoice& voice, double t, double ms, std::size_t& next_update)
{
    while (next_update < voice.updates.size() && voice.updates[next_update] <= t)
    {
        next_update++;
    }

    double next = t + kUpdateMs;
    if (next_update < voice.updates.size())
    {
        next = std::min(next, voice.updates[next_update]);
    }
    if (next > ms || (voice.stop_ms && next >= *voice.stop_ms))
    {
        return std::nullopt;
    }

    return next;
}

} // namespace

uint8_t KeptController(uint8_t controller)
{
    return controller < 0x40 ? controller & 0x1f : controller;
}

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
    constexpr double kCentre = kWheelCentre;

    // MusyX takes the mod wheel and the pan as signed sources.
    if (source.variable || source.controller == kModWheel || source.controller == kPanController)
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

    // The ramps of the held note up to the stop.
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
    const double reference = DspPitch(PitchSettingsOf(voice, 0.0), sample, key);
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

        const std::optional<double> next = NextUpdate(voice, t, ms, next_update);
        if (!next)
        {
            return curve;
        }
        t = *next;
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
        const std::optional<double> next = NextUpdate(voice, t, ms, next_update);
        if (!next)
        {
            break;
        }
        t = *next;
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
    return DspPitch(PitchSettingsOf(voice, 0.0), sample, key) > kHighestPitch;
}

double HighestPitchHz()
{
    return kHighestPitch / 4096.0 * kOutputRate;
}

std::optional<double> CurveSampleMs(const MacroVoice& voice, const Sample& sample, int key)
{
    // The DSP plays the pitch's 4096ths of a frame of the sample for each frame of the output, at kOutputRate.
    const double frames_per_ms = DspPitch(PitchSettingsOf(voice, 0.0), sample, key) / 4096.0 * kOutputRate / 1000.0;
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
    const auto it = object_reads_.find(object);
    if (it != object_reads_.end())
    {
        return it->second;
    }

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

    return object_reads_.emplace(object, std::move(reads)).first->second;
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

    // A controller that only splits read gives the highest value at which a split turns, up to `value`, or 0.
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
            if (voices.size() >= kMostEffectVoices)
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
