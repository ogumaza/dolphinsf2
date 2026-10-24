// SPDX-License-Identifier: MIT

// The conversion of a MusyX song (see musyx_convert.h).
//
// Each preset of the performance plays a page of the song group: the object that a program starts, a macro, a keymap
// or a layer. The preset's SoundFont instrument has a zone for each run of keys that start the same voice the same
// way: the same macro, with the same transposition, pan and share of the volume. The zone plays the macro's sample with
// the macro's tuning, volume, envelopes, vibrato, tremolo and key group. The SFZ files have the same regions. SFZ can't
// scale an envelope's time by the key. A voice whose envelope does that therefore gets a region for each key in them.

#include "musyx_convert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/musyx/samples.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/jaudio/convert.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/sfz/track_file.h"

#include "jaudio.h"
#include "musyx_game.h"
#include "musyx_player.h"
#include "musyx_voices.h"

namespace dolphinsf2::musyx
{
namespace
{

namespace sf2 = seqlib::sf2;

using seqlib::sfz::Number;

// The rate of a fall that falls at once.
constexpr double kAtOnce = std::numeric_limits<double>::infinity();

// MusyX's milliseconds in real ones. Its clock counts the output's samples as if they came at kNominalRate.
constexpr double kMsScale = kNominalRate / kOutputRate;

// The decibels that decays and releases fall in their time. In SoundFont, they fall 100 dB. In SFZ, they fall by a
// factor of e^-8: 8 × 20 / ln 10 dB. sfzformat.com gives that curve on its page on ampeg_attack.
constexpr double kSoundFontFallDb = 100.0;
constexpr double kSfzFallDb = 8.0 * 20.0 / std::numbers::ln10;

// The lowest volume that sfzformat.com documents for an SFZ region, in decibels. Every quieter region takes it. A
// region at a gain of 0 would otherwise get -inf. sfizz 1.2 can't read that.
constexpr double kSfzLowestVolumeDb = -144.0;

// A linear fall that ends in silence is fitted to reach the level that it reaches 20 dB down. A fall to a level less
// than twice as far down is fitted halfway down in decibels. A macro's fades are fitted the same way.
constexpr double kFitDb = 20.0;

// The step in MusyX milliseconds in which FadeRate follows a macro's fade, and the most steps it takes. A fade longer
// than 25 s takes longer steps.
constexpr double kFadeStepMs = 0.25;
constexpr double kMostFadeSteps = 100000.0;

// The sustain level that DLS envelopes treat as silence: their voice ends after the decay.
constexpr double kDlsSilence = 1.0e-4; // below -80 dB, above -96.5 dB

// SoundFont's limits: silence in centibels, the longest envelope time and hold, and the widest tuning in cents.
constexpr int kSilentCentibels = 1440;
constexpr int kLongestTimecents = 8000;
constexpr int kLongestHoldTimecents = 5000;
constexpr int kWidestCents = 12000;

// The share of its range that the pan of a voice with ITD keeps.
constexpr double kItdPan = 0.76604;

// SoundFont's default modulator from the velocity to initialAttenuation (section 8.4.2): the velocity from its maximum
// to its minimum on the concave curve, with an amount of 960 cB. A zone's modulator with the same source, destination,
// amount source and transform replaces it.
constexpr uint16_t kSrcVelocityConcaveNegative = sf2::kNoteOnVelocity | sf2::kNegative | sf2::kConcave; // 0x0502
constexpr int kDefaultVelocityCentibels = 960;

// The lowest velocity that a zone's velocity response is fitted from (VelocityAttenuation).
constexpr int kLowestFittedVelocity = 32;

// The velocities at which an SFZ region gives its velocity curve (amp_velcurve_N): 1, then every 8th from 8 to 120.
// SFZ interpolates between them, and the curve reaches 1 at velocity 127.
constexpr int kSfzCurveStep = 8;

// The pan in tenths of a percent at either side: in SoundFont's pan generator, and in the CC10 modulator of each global
// zone (kCc10ToPan).
constexpr double kPanRange = 500.0;

// A run of keys of a preset that start the same voice the same way.
struct Zone
{
    bool SameVoice(const Zone& other) const
    {
        return macro == other.macro && key_offset == other.key_offset && pan == other.pan && volume == other.volume &&
               from_ms == other.from_ms && part == other.part;
    }

    int low_key = 0;
    int high_key = 0;
    uint16_t macro = 0;
    int key_offset = 0;   // the voice's key minus the note's
    double pan = 64.0;    // 0 to 127, 64 the centre
    double volume = 1.0;  // the share of the note's volume that the layers give the voice
    double from_ms = 0.0; // the note starts this long after the voice (KeyVoice::from_ms)
    std::size_t part = 0; // the part of the voice that the zone plays (KeyVoice::part)
    bool effect = false;  // the zone plays a voice of an effect
};

// The pan of voice `v`: the pan of the macro's last Panning or PianoPan step, else the pan of the keymap or the layer
// (MacroVoice::pan).
double PanOf(Instruments& instruments, const KeyVoice& v)
{
    const MacroVoice& voice = instruments.Macro(v.macro, v.part, 0.0);
    if (voice.pan_last)
    {
        return *voice.pan;
    }
    if (voice.piano_pan)
    {
        return PianoPanAt(*voice.piano_pan, v.key);
    }

    return v.pan;
}

// The volume of a zone's voice at `volume` (VoiceVolume). An effect's voice takes the volume controller that its macro
// sets. A song's channel holds the controller. A song's voice therefore leaves it out.
double ZoneVolume(const MacroVoice& voice, const Zone& zone, double volume)
{
    return VoiceVolume(voice, volume, zone.effect ? voice.volume_controller.value_or(1.0) : 1.0);
}

// The zones of the preset that plays `object`, in order of their lowest keys.
std::vector<Zone> ZonesOf(Instruments& instruments, uint16_t object)
{
    std::vector<Zone> zones;
    std::vector<Zone> open; // the runs that the last key extended
    for (int key = 0; key < 128; key++)
    {
        std::vector<Zone> next;
        std::vector<bool> extended(open.size());
        for (const KeyVoice& v : VoicesOf(instruments.Group().pool, object, key))
        {
            Zone z{.low_key = key,
                   .high_key = key,
                   .macro = v.macro,
                   .key_offset = v.key - key,
                   .pan = PanOf(instruments, v),
                   .volume = v.volume};
            for (std::size_t i = 0; i < open.size(); i++)
            {
                if (!extended[i] && open[i].SameVoice(z))
                {
                    extended[i] = true;
                    z.low_key = open[i].low_key;
                    break;
                }
            }
            next.push_back(z);
        }
        for (std::size_t i = 0; i < open.size(); i++)
        {
            if (!extended[i])
            {
                zones.push_back(open[i]);
            }
        }
        open = std::move(next);
    }
    zones.insert(zones.end(), open.begin(), open.end());
    std::stable_sort(zones.begin(), zones.end(), [](const Zone& a, const Zone& b) { return a.low_key < b.low_key; });

    return zones;
}

// The zones of the preset that plays `note`, a note of an effect: one for each of its voices, over every key.
std::vector<Zone> ZonesOf(Instruments& instruments, const EffectNote& note)
{
    std::vector<Zone> zones;
    for (const KeyVoice& v : note.voices)
    {
        zones.push_back({.low_key = 0,
                         .high_key = 127,
                         .macro = v.macro,
                         .key_offset = v.key - note.key,
                         .pan = PanOf(instruments, v),
                         .volume = v.volume,
                         .from_ms = v.from_ms,
                         .part = v.part,
                         .effect = true});
    }

    return zones;
}

// The amplitude of a zone at full velocity and full controllers. MusyX multiplies the velocity, the controllers, the
// song's volume, the game's master volume, the macro's volume and the layers' volumes. MusyX plays the product on the
// voice's curve: the DLS curve or MusyX's volume table. SoundFont's and SFZ's default curves square the velocity and
// the controllers. The player maps the velocities and the controllers of voices on the table to those curves. The
// player puts the song's volume and the master volume in CC7. The zone takes the level that the voice's curve gives
// the macro's volume times the layers' volumes, plus the macro's addition (ZoneVolume).
double ZoneGain(const Instruments& instruments, const MacroVoice& voice, const Zone& zone)
{
    return instruments.Level(voice, ZoneVolume(voice, zone, zone.volume));
}

// The level of `zone` at MIDI velocity `velocity` over its level at full velocity, with the channel's volume and
// expression at full. The player writes a note on MusyX's volume table at the velocity whose square is the table's
// level of the note's velocity (MapLevel). A note on the DLS curve keeps its velocity. A voice that adds to its volume
// (MacroVoice::volume_add) follows the velocity less than SoundFont's and SFZ's default curves.
double VelocityResponse(const Instruments& instruments, const MacroVoice& voice, const Zone& zone, int velocity)
{
    const double full = ZoneGain(instruments, voice, zone);
    if (full <= 0.0)
    {
        return 1.0;
    }

    const double written = velocity / 127.0;
    const double note = voice.dls_volume ? written : TableVolume(instruments.Volumes(), written * written);
    return instruments.Level(voice, ZoneVolume(voice, zone, note * zone.volume)) / full;
}

// The amount of a modulator from the velocity to initialAttenuation that gives `zone` the velocity response of a voice
// that adds to its volume (VelocityResponse). The modulator replaces SoundFont's default one (section 8.4.2). At
// velocity v, a modulator of amount a takes a × -(20/96) log10((v/127)²) cB off the zone. The default one's 960 cB give
// the square of the velocity. The amount fits the response's attenuation from velocity 32 to 127 by least squares.
int VelocityAttenuation(const Instruments& instruments, const MacroVoice& voice, const Zone& zone)
{
    double product = 0.0;
    double square = 0.0;
    for (int velocity = kLowestFittedVelocity; velocity <= 127; velocity++)
    {
        const double curve = -20.0 / 96.0 * std::log10(std::pow(velocity / 127.0, 2.0));
        const double response = VelocityResponse(instruments, voice, zone, velocity);
        const double centibels = response > 0.0 ? -200.0 * std::log10(response) : kSilentCentibels;
        product += curve * centibels;
        square += curve * curve;
    }

    return std::max(static_cast<int>(std::lround(product / square)), 0);
}

// The opcodes of the SFZ velocity curve that gives `zone` the velocity response of a voice that adds to its volume
// (VelocityResponse), or nothing for a voice that adds nothing.
std::string SfzVelocityCurve(const Instruments& instruments, const MacroVoice& voice, const Zone& zone)
{
    if (voice.volume_add <= 0.0)
    {
        return "";
    }

    std::string s = " amp_velcurve_1=" + Number(VelocityResponse(instruments, voice, zone, 1), 6);
    for (int velocity = kSfzCurveStep; velocity < 127; velocity += kSfzCurveStep)
    {
        s += " amp_velcurve_" + std::to_string(velocity) + "=" +
             Number(VelocityResponse(instruments, voice, zone, velocity), 6);
    }

    return s;
}

// The tuning of a zone in cents at its sample's key, and whether its pitch follows the key. MusyX plays the voice's
// key, moved by the macro, against the sample's key. A rate that the macro sets takes the place of both keys. The
// output's rate makes every voice a little sharp.
struct Tuning
{
    double cents = 0.0;
    bool follows_key = true;
};

Tuning TuningOf(const MacroVoice& voice, const Zone& zone, const Sample& sample)
{
    const double kRateCents = 1200.0 * std::log2(kOutputRate / kNominalRate);
    if (voice.rate_hz && *voice.rate_hz > 0.0 && sample.pcm.sample_rate > 0)
    {
        return {1200.0 * std::log2(*voice.rate_hz / sample.pcm.sample_rate) + kRateCents, false};
    }
    if (voice.fixed_key)
    {
        return {(*voice.fixed_key - sample.key) * 100.0 + voice.detune_cents + kRateCents, false};
    }

    return {(zone.key_offset + voice.key_shift) * 100.0 + voice.detune_cents + kRateCents, true};
}

// The pan of a zone, -1 (left) to 1 (right), before the channel's. MusyX adds the channel's pan to the voice's, then
// narrows the pan of a voice with ITD.
double ZonePan(const MacroVoice& voice, const Zone& zone)
{
    return std::clamp((zone.pan - 64) / 63.0, -1.0, 1.0) * (voice.itd ? kItdPan : 1.0);
}

// The rate in decibels a millisecond of a fall that is linear in amplitude, from full level to `to` in `ms`, fitted as
// a fall that is linear in decibels.
double LinearFallRate(double ms, double to)
{
    const double range_db = to > 0.0 ? -20.0 * std::log10(to) : kAtOnce;
    if (ms <= 0.0 || range_db <= 0.0)
    {
        return kAtOnce;
    }

    const double fit_db = range_db <= 2.0 * kFitDb ? range_db / 2.0 : kFitDb;
    const double level = std::pow(10.0, -fit_db / 20.0);
    return fit_db / (ms * (1.0 - level) / (1.0 - to));
}

// A volume envelope in SoundFont's and SFZ's terms, in real milliseconds: an attack that rises linearly in amplitude, a
// hold, and a decay and a release that fall linearly in decibels.
struct VolumeShape
{
    double attack_ms = 0.0;
    double hold_ms = 0.0;
    double decay_db_per_ms = kAtOnce;
    double sustain_db = 0.0;
    double release_db_per_ms = kAtOnce;
};

// The amplitude of envelope `e` `ms` MusyX milliseconds into a held note, with the attack `attack_ms` and the decay
// `decay_ms` that the note gives it. The attack rises linearly in amplitude. The decay falls to the sustain, linearly
// in amplitude in the linear form and kDlsFallDb in its time in the DLS form.
double HeldEnvelopeAmplitude(const Envelope& e, double attack_ms, double decay_ms, double ms)
{
    if (ms < attack_ms)
    {
        return ms / attack_ms;
    }

    const double into_decay = ms - attack_ms;
    if (e.dls)
    {
        const double sustain = e.sustain < kDlsSilence ? 0.0 : e.sustain;
        return decay_ms > 0.0 ? std::max(std::pow(10.0, -kDlsFallDb * into_decay / decay_ms / 20.0), sustain) : sustain;
    }
    if (into_decay >= decay_ms)
    {
        return e.sustain;
    }

    return 1.0 - (1.0 - e.sustain) * into_decay / decay_ms;
}

// The milliseconds into a held note at which envelope `e`, with the decay `decay_ms` after the attack `attack_ms`,
// reaches its sustain.
double SettledMs(const Envelope& e, double attack_ms, double decay_ms)
{
    const bool silent = e.dls ? e.sustain < kDlsSilence : e.sustain <= 0.0;
    if (!e.dls || silent)
    {
        return attack_ms + decay_ms;
    }

    return attack_ms + decay_ms * -20.0 * std::log10(e.sustain) / kDlsFallDb;
}

// The hold, the decay and the sustain of a held note whose macro fades its volume (MacroVoice::held_ramps), in MusyX
// milliseconds.
struct HeldShape
{
    double hold_ms = 0.0;
    double decay_db_per_ms = kAtOnce;
    double sustain_db = 0.0;
};

// The shape of a held note of `voice` at `volume`. Its envelope, if any, has the attack `attack_ms` and the decay
// `decay_ms`. The note's level is the envelope's amplitude times the curve's level of the volume that the ramps give,
// over the level at `volume`. A voice that stops by itself falls to silence in kCutMs from its stop. The decay starts
// where the level first falls, after the attack. It reaches the level kFitDb down when the note does, or halfway down
// in decibels to a level less than twice as far down. Return nothing if the level doesn't fall.
std::optional<HeldShape> HeldShapeOf(const Instruments& instruments, const MacroVoice& voice, double volume,
                                     double attack_ms, double decay_ms)
{
    const double full = instruments.Level(voice, volume);
    if (full <= 0.0)
    {
        return std::nullopt;
    }

    const std::optional<Envelope>& e = voice.envelope;
    const auto level = [&](double ms)
    {
        double l = instruments.Level(voice, RampedVolume(voice.held_ramps, volume, ms)) / full;
        if (e)
        {
            l *= HeldEnvelopeAmplitude(*e, attack_ms, decay_ms, ms);
        }
        if (voice.stop_ms && ms > *voice.stop_ms)
        {
            l *= std::max(1.0 - (ms - *voice.stop_ms) / kCutMs, 0.0);
        }

        return l;
    };

    // Where the level starts to fall and where it stops changing: the ramps, the envelope's decay and the stop.
    double falls = voice.held_ramps.front().start_ms;
    double settles = 0.0;
    for (const VolumeRamp& r : voice.held_ramps)
    {
        settles = std::max(settles, r.start_ms + r.ms);
    }
    if (e)
    {
        falls = std::min(falls, attack_ms);
        settles = std::max(settles, SettledMs(*e, attack_ms, decay_ms));
    }
    if (voice.stop_ms)
    {
        falls = std::min(falls, *voice.stop_ms);
        settles = *voice.stop_ms + kCutMs;
    }

    const double last = level(settles);
    const double range_db = last > 0.0 ? -20.0 * std::log10(last) : kAtOnce;
    if (range_db <= 0.0)
    {
        return std::nullopt;
    }

    // The time at which the note's level reaches the fit's, step by step from the decay's start.
    const double fit_db = range_db <= 2.0 * kFitDb ? range_db / 2.0 : kFitDb;
    const double fit = std::pow(10.0, -fit_db / 20.0);
    const double start = std::max(falls, attack_ms);
    const double step = std::max(kFadeStepMs, (settles - start) / kMostFadeSteps);
    double crossed = start;
    double before = level(start);
    while (before > fit && crossed < settles)
    {
        const double at = std::min(crossed + step, settles);
        const double l = level(at);
        crossed = l <= fit ? crossed + (at - crossed) * (before - fit) / (before - l) : at;
        before = l;
    }

    HeldShape shape;
    shape.hold_ms = start - attack_ms;
    shape.decay_db_per_ms = crossed > start ? fit_db / (crossed - start) : kAtOnce;
    shape.sustain_db = last > 0.0 ? -range_db : -kSilentCentibels / 10.0;

    return shape;
}

// The rate in decibels a MusyX millisecond of the release that fits the fade of `voice` after its note's release
// (KeyOff::kFade), for the voice at `volume`. The fade's ramps move the volume in lines, and the voice's curve gives
// their levels. The stop then ends the voice with a fade of kCutMs, in a line in amplitude. The release reaches the
// level kFitDb down when the voice does.
double FadeRate(const Instruments& instruments, const MacroVoice& voice, double volume)
{
    const double full = instruments.Level(voice, volume);
    if (full <= 0.0)
    {
        return kAtOnce;
    }

    // The fade's level step by step, up to the stop.
    const double fit = full * std::pow(10.0, -kFitDb / 20.0);
    const double stop_ms = voice.release_fade.stop_ms;
    const double step = std::max(kFadeStepMs, stop_ms / kMostFadeSteps);
    double before = full;
    for (double ms = step; ms < stop_ms; ms += step)
    {
        const double level = instruments.Level(voice, FadeVolume(voice.release_fade, volume, ms).value());
        if (level <= fit)
        {
            const double crossed = ms - step * (fit - level) / (before - level);
            return kFitDb / crossed;
        }

        before = level;
    }

    // The cut from the level at the stop.
    const double last =
        instruments.Level(voice, FadeVolume(voice.release_fade, volume, std::max(stop_ms - step, 0.0)).value());
    return kFitDb / (stop_ms + kCutMs * (1.0 - fit / last));
}

// The volume envelope of `voice` at `volume` with the voice's key `key` and the note's velocity `velocity`.
VolumeShape VolumeOf(const Instruments& instruments, const MacroVoice& voice, double volume, int key, int velocity)
{
    VolumeShape shape;
    const std::optional<Envelope>& e = voice.envelope;
    const bool dls = e && e->dls;
    double attack_ms = 0.0; // MusyX's milliseconds
    double decay_ms = 0.0;
    if (e)
    {
        if (dls)
        {
            attack_ms = DlsMilliseconds(e->attack_timecents + velocity * e->attack_tc_per_velocity);
            decay_ms = DlsMilliseconds(e->decay_timecents + key * e->decay_tc_per_key);
            shape.attack_ms = attack_ms * kMsScale;
            const double real_decay_ms = decay_ms * kMsScale;
            shape.decay_db_per_ms = real_decay_ms > 0.0 ? kDlsFallDb / real_decay_ms : kAtOnce;
        }
        else
        {
            attack_ms = e->attack_ms;
            decay_ms = e->decay_ms;
            shape.attack_ms = attack_ms * kMsScale;
            shape.decay_db_per_ms = LinearFallRate(decay_ms * kMsScale, e->sustain);
        }

        const bool silent = dls ? e->sustain < kDlsSilence : e->sustain <= 0.0;
        shape.sustain_db = silent ? -kSilentCentibels / 10.0 : 20.0 * std::log10(e->sustain);
    }

    // The fades of a held note, with the envelope's decay, give the hold, the decay and the sustain (HeldShapeOf).
    const std::optional<HeldShape> held =
        voice.held_ramps.empty() ? std::nullopt : HeldShapeOf(instruments, voice, volume, attack_ms, decay_ms);
    if (held)
    {
        shape.hold_ms = held->hold_ms * kMsScale;
        shape.decay_db_per_ms = held->decay_db_per_ms / kMsScale;
        shape.sustain_db = held->sustain_db;
    }

    // The release: the macro's fade for a voice that fades, the held note's fall for a voice that goes on with it
    // through the release, the envelope's for a voice that releases, and otherwise the fade of a stopped voice. A
    // voice without an envelope releases at once.
    if (voice.key_off == KeyOff::kFade)
    {
        shape.release_db_per_ms = FadeRate(instruments, voice, volume) / kMsScale;
        return shape;
    }
    if (held && FadesThroughRelease(voice))
    {
        shape.release_db_per_ms = shape.decay_db_per_ms;
        return shape;
    }

    double release_ms = kCutMs;
    if (voice.key_off == KeyOff::kRelease)
    {
        release_ms = e ? e->release_ms : 0.0;
    }
    release_ms *= kMsScale;
    if (release_ms > 0.0)
    {
        // A linear release falls from where it starts to silence in its time.
        shape.release_db_per_ms = dls ? kDlsFallDb / release_ms : LinearFallRate(release_ms, 0.0);
    }

    return shape;
}

// A pitch envelope in SoundFont's and SFZ's terms, in real milliseconds. MusyX's decay and release fall linearly in
// decibels of their range. SoundFont's fall in a line, fitted to reach half the range at the same time as MusyX's.
// SFZ's decays and releases fall by a factor of e^-8 in their time. MusyX's decays and releases fall by the same factor
// in kSfzFallDb / kDlsFallDb of their time.
struct PitchShape
{
    double attack_ms = 0.0;
    double decay_ms = 0.0;   // SoundFont's
    double release_ms = 0.0; // SoundFont's
    double sfz_decay_ms = 0.0;
    double sfz_release_ms = 0.0;
    double sustain = 0.0; // the share of the range
    double range_cents = 0.0;
};

// The pitch envelope of `envelope` with the voice's key `key`.
PitchShape PitchOf(const PitchEnvelope& envelope, int key)
{
    const Envelope& e = envelope.envelope;
    PitchShape p;
    p.attack_ms = e.attack_ms * kMsScale;
    const double decay_ms = DlsMilliseconds(e.decay_timecents + key * e.decay_tc_per_key) * kMsScale;
    const double release_ms = e.release_ms * kMsScale;
    p.sustain = e.sustain < kDlsSilence ? 0.0 : e.sustain;
    p.range_cents = envelope.range_cents;

    // MusyX reaches the middle of the decay's range, or half of the release's, after a fall of `db`.
    const double middle = (1.0 + p.sustain) / 2.0;
    const double db = -20.0 * std::log10(middle);
    p.decay_ms = decay_ms * db / kDlsFallDb / (1.0 - middle);
    p.release_ms = release_ms * 20.0 * std::log10(2.0) / kDlsFallDb * 2.0;
    p.sfz_decay_ms = decay_ms * kSfzFallDb / kDlsFallDb;
    p.sfz_release_ms = release_ms * kSfzFallDb / kDlsFallDb;

    return p;
}

// A tremolo in decibels: MusyX's volume swings between full level and the depth's fall, a sine on the amplitude. The
// conversions swing it in decibels about the middle: half the range down, give or take half the range.
struct TremoloShape
{
    double hz = 0.0;
    double half_range_db = 0.0;
    double half_mod_range_db = 0.0; // what the mod wheel at its top adds
};

// The tremolo of `voice`, if it has one.
std::optional<TremoloShape> TremoloOf(const MacroVoice& voice)
{
    if (!voice.tremolo)
    {
        return std::nullopt;
    }

    const auto range_db = [](double depth)
    {
        return depth >= 1.0 ? kSilentCentibels / 10.0 : -20.0 * std::log10(1.0 - std::max(depth, 0.0));
    };
    const Tremolo& t = *voice.tremolo;
    const double range = range_db(t.depth);
    return TremoloShape{1000.0 / (t.period_ms * kMsScale), range / 2.0, (range_db(t.full_mod_depth) - range) / 2.0};
}

// SoundFont's units.
int FallTimecents(double db_per_ms)
{
    return db_per_ms == kAtOnce ? -12000 : ToTimecents(kSoundFontFallDb / db_per_ms, -12000, kLongestTimecents);
}

int SustainCentibels(double sustain_db)
{
    return std::clamp(static_cast<int>(std::lround(-10.0 * sustain_db)), 0, kSilentCentibels);
}

int16_t Amount(double value)
{
    return static_cast<int16_t>(std::clamp(std::lround(value), -32768L, 32767L));
}

int Cents(double cents)
{
    return static_cast<int>(std::lround(std::clamp(cents, -1.0 * kWidestCents, 1.0 * kWidestCents)));
}

int LfoFrequency(double hz)
{
    return std::clamp(static_cast<int>(std::lround(1200.0 * std::log2(hz / 8.176))), -16000, 4500);
}

// The instrument's global zone holds seqlib's kGlobalZoneModulators. MusyX doesn't filter by velocity. Its pitch wheel
// has its full range. CC10 pans at the amount of 500 that most synthesizers play (kCc10ToPan). The mod wheel gives only
// the macros' vibrato. SoundFont's default CC1 modulator would add 50 cents.
sf2::Zone GlobalZone()
{
    sf2::Zone global;
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
    global.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, 0, 0});

    return global;
}

// Add generator `oper` with `value` to `zone`, before its sample. The sample's generator comes last.
void AddGenerator(sf2::Zone& zone, uint16_t oper, int value)
{
    zone.generators.insert(zone.generators.end() - 1, sf2::Generator::Value(oper, value));
}

// Add the scales of DLS envelope `e` to `zone`: its decay by the voice's key, and its attack by the note's velocity. An
// attack that takes no time (`attack_ms`) gets no scale.
void AddDlsScales(sf2::Zone& zone, const Envelope& e, double attack_ms)
{
    if (e.decay_tc_per_key != 0.0)
    {
        AddGenerator(zone, sf2::kKeynumToVolEnvDecay,
                     std::clamp(static_cast<int>(std::lround(-e.decay_tc_per_key)), -1200, 1200));
    }
    if (e.attack_tc_per_velocity != 0.0 && attack_ms > 0.0)
    {
        zone.modulators.push_back(
            {sf2::kNoteOnVelocity, sf2::kAttackVolEnv, Amount(e.attack_tc_per_velocity * 128.0), 0});
    }
}

// Add vibrato `v` to `zone`'s vibrato LFO. The mod wheel adds to the vibrato's depth, in 128ths of its range. It can
// scale the whole depth instead.
void AddVibrato(sf2::Zone& zone, const Vibrato& v)
{
    if (v.delay_ms > 0.0)
    {
        AddGenerator(zone, sf2::kDelayVibLfo, ToTimecents(v.delay_ms * kMsScale, -12000, 5000));
    }
    AddGenerator(zone, sf2::kFreqVibLfo, LfoFrequency(1000.0 / (v.period_ms * kMsScale)));
    if (!v.mod_scales_depth && v.depth_cents != 0.0)
    {
        AddGenerator(zone, sf2::kVibLfoToPitch, Cents(v.depth_cents));
    }

    zone.modulators.push_back(
        {sf2::kSrcModWheel, sf2::kVibLfoToPitch, Amount(v.mod_scales_depth ? v.depth_cents : v.mod_depth_cents), 0});
    if (v.mod_scales_depth && v.mod_depth_cents != 0.0)
    {
        zone.modulators.push_back(
            {sf2::kSrcModWheel, sf2::kVibLfoToPitch, Amount(v.mod_depth_cents), sf2::kSrcModWheel});
    }
}

// Add pitch envelope `envelope` to `zone`'s modulation envelope. `key` is the voice's key.
void AddPitchEnvelope(sf2::Zone& zone, const PitchEnvelope& envelope, int key)
{
    const PitchShape p = PitchOf(envelope, key);
    AddGenerator(zone, sf2::kModEnvToPitch, Cents(p.range_cents));
    AddGenerator(zone, sf2::kAttackModEnv, ToTimecents(p.attack_ms, -12000, kLongestTimecents));
    AddGenerator(zone, sf2::kDecayModEnv, ToTimecents(p.decay_ms, -12000, kLongestTimecents));
    AddGenerator(zone, sf2::kSustainModEnv, static_cast<int>(std::lround(1000.0 * (1.0 - p.sustain))));
    AddGenerator(zone, sf2::kReleaseModEnv, ToTimecents(p.release_ms, -12000, kLongestTimecents));
    const double per_key = envelope.envelope.decay_tc_per_key;
    if (per_key != 0.0)
    {
        AddGenerator(zone, sf2::kKeynumToModEnvDecay, std::clamp(static_cast<int>(std::lround(-per_key)), -1200, 1200));
    }
}

// Add `tremolo` to `zone`'s modulation LFO. The mod wheel moves the tremolo's depth.
void AddTremolo(sf2::Zone& zone, const TremoloShape& tremolo)
{
    AddGenerator(zone, sf2::kFreqModLfo, LfoFrequency(tremolo.hz));
    AddGenerator(zone, sf2::kModLfoToVolume, static_cast<int>(std::lround(tremolo.half_range_db * 10.0)));
    if (tremolo.half_mod_range_db != 0.0)
    {
        zone.modulators.push_back(
            {sf2::kSrcModWheel, sf2::kModLfoToVolume, Amount(tremolo.half_mod_range_db * 10.0), 0});
        zone.modulators.push_back(
            {sf2::kSrcModWheel, sf2::kInitialAttenuation, Amount(tremolo.half_mod_range_db * 10.0), 0});
    }
}

// Build the SoundFont zone of `z`. The zone plays `voice` of `instruments` at amplitude `gain` (ZoneGain) with
// `sample`, at `index` in the SoundFont's samples. Add `quieter` centibels of attenuation. A voice that adds to its
// volume follows the velocity by `velocity_centibels` (VelocityAttenuation) in place of the default 960.
sf2::Zone SoundFontZone(const Instruments& instruments, const Zone& z, const MacroVoice& voice, double gain,
                        const Sample& sample, uint16_t index, bool loop, double quieter, int velocity_centibels)
{
    // SoundFont's key scales leave key 60 as it is. The envelopes therefore take the voice's key at the note's key 60.
    const int key60 = 60 + z.key_offset;
    const VolumeShape shape = VolumeOf(instruments, voice, ZoneVolume(voice, z, z.volume), key60, 0);
    const Tuning tuning = TuningOf(voice, z, sample);
    const int cents = Cents(tuning.cents);
    const std::optional<TremoloShape> tremolo = TremoloOf(voice);

    // RegionZone writes the attenuation in the form that seqlib's ToZoneAttenuation gives. A tremolo swings about the
    // middle of its range, half the range down (TremoloShape).
    sf2::ZoneValues values;
    values.keys = {z.low_key, z.high_key};
    values.attenuation = -200.0 * std::log10(gain) + quieter + (tremolo ? tremolo->half_range_db * 10.0 : 0.0);
    values.pan = static_cast<int>(std::lround(ZonePan(voice, z) * kPanRange));
    values.attack = ToTimecents(shape.attack_ms, -12000, kLongestTimecents);
    values.hold = ToTimecents(shape.hold_ms, -12000, kLongestHoldTimecents);
    values.decay = FallTimecents(shape.decay_db_per_ms);
    values.sustain = SustainCentibels(shape.sustain_db);
    values.release = FallTimecents(shape.release_db_per_ms);
    values.root_key = sample.key;
    values.coarse_tune = cents / 100;
    values.fine_tune = cents % 100;
    values.loop = loop;
    values.exclusive_class = voice.key_group;
    values.sample = index;
    sf2::Zone zone = sf2::RegionZone(values);

    // The generators and modulators that RegionZone doesn't write. A voice with ITD follows less of the channel's pan.
    if (!tuning.follows_key)
    {
        AddGenerator(zone, sf2::kScaleTuning, 0);
    }
    if (voice.envelope && voice.envelope->dls)
    {
        AddDlsScales(zone, *voice.envelope, shape.attack_ms);
    }
    if (voice.itd)
    {
        zone.modulators.push_back({sf2::kSrcPan, sf2::kPan, Amount(kPanRange * kItdPan), 0});
    }
    if (velocity_centibels != kDefaultVelocityCentibels)
    {
        zone.modulators.push_back(
            {kSrcVelocityConcaveNegative, sf2::kInitialAttenuation, Amount(velocity_centibels), 0});
    }
    if (voice.vibrato)
    {
        AddVibrato(zone, *voice.vibrato);
    }
    if (voice.pitch_envelope)
    {
        AddPitchEnvelope(zone, *voice.pitch_envelope, key60);
    }
    if (tremolo)
    {
        AddTremolo(zone, *tremolo);
    }

    return zone;
}

// The name of `sample` among a song's samples in the SoundFont, such as "s12". The SFZ files take Sample::sfz_name.
std::string SampleName(const Sample& sample)
{
    return "s" + std::to_string(sample.id);
}

// The SoundFont of a song: an instrument and a preset for each preset of the performance, with each sample once.
class SoundFontBuilder
{
public:
    SoundFontBuilder(Instruments& instruments, double quieter) : instruments_(instruments), quieter_(quieter)
    {
    }

    // Add preset `index`, named `name`, with `zones`, at bank index / 128, program index % 128.
    void Add(uint32_t index, const std::string& name, const std::vector<Zone>& zones);

    // Copy a preset that Add added to bank 128 for MIDI channel 10.
    void AddChannel10Copy(uint32_t index)
    {
        sf2::AddChannel10Copy(sf_, index, warnings_);
    }

    sf2::SoundFont& Result()
    {
        return sf_;
    }

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    Instruments& instruments_;
    double quieter_;
    sf2::SoundFont sf_;
    SoundFontSamples samples_{sf_};
    std::vector<std::string> warnings_;
};

void SoundFontBuilder::Add(uint32_t index, const std::string& name, const std::vector<Zone>& zones)
{
    sf2::Instrument inst;
    inst.name = name;
    inst.zones.push_back(GlobalZone());
    for (const Zone& z : zones)
    {
        const MacroVoice& voice = instruments_.Macro(z.macro, z.part, z.from_ms);
        const Sample* sample = instruments_.SampleOf(voice);
        if (sample == nullptr)
        {
            continue;
        }

        const SoundFontSamples::Sample added =
            samples_.Get(SampleName(*sample), sample->key, [sample] { return &sample->pcm; }).value();
        const int velocity =
            voice.volume_add > 0.0 ? VelocityAttenuation(instruments_, voice, z) : kDefaultVelocityCentibels;
        inst.zones.push_back(SoundFontZone(instruments_, z, voice, ZoneGain(instruments_, voice, z), *sample, added.id,
                                           added.loop, quieter_, velocity));
    }

    sf2::AddInstrumentPreset(sf_, index, name, std::move(inst));
}

// The opcodes of vibrato `v` on LFO 1, a sine. sfizz divides the mod wheel by 127, MusyX by 128.
std::string SfzVibrato(const Vibrato& v)
{
    std::string s = " lfo1_wave=1 lfo1_freq=" + Number(1000.0 / (v.period_ms * kMsScale), 6);
    if (v.delay_ms > 0.0)
    {
        s += " lfo1_delay=" + Number(v.delay_ms * kMsScale / 1000.0, 6);
    }
    if (!v.mod_scales_depth && v.depth_cents != 0.0)
    {
        s += " lfo1_pitch=" + Number(v.depth_cents, 3);
    }

    const double mod = v.mod_scales_depth ? v.depth_cents + v.mod_depth_cents : v.mod_depth_cents;
    if (mod != 0.0)
    {
        s += " lfo1_pitch_oncc1=" + Number(mod * 127.0 / 128.0, 6);
    }

    return s;
}

// The opcodes of pitch envelope `envelope`. `key` is the voice's key.
std::string SfzPitchEnvelope(const PitchEnvelope& envelope, int key)
{
    const PitchShape p = PitchOf(envelope, key);
    return " pitcheg_attack=" + Number(p.attack_ms / 1000.0, 6) +
           " pitcheg_decay=" + Number(p.sfz_decay_ms / 1000.0, 6) + " pitcheg_sustain=" + Number(p.sustain * 100.0, 3) +
           " pitcheg_release=" + Number(p.sfz_release_ms / 1000.0, 6) +
           " pitcheg_depth=" + std::to_string(Cents(p.range_cents));
}

// The opcodes of `tremolo` on LFO 2.
std::string SfzTremolo(const TremoloShape& tremolo)
{
    std::string s =
        " lfo2_wave=1 lfo2_freq=" + Number(tremolo.hz, 6) + " lfo2_volume=" + Number(tremolo.half_range_db, 3);
    if (tremolo.half_mod_range_db != 0.0)
    {
        s += " lfo2_volume_oncc1=" + Number(tremolo.half_mod_range_db, 3) +
             " volume_oncc1=" + Number(-tremolo.half_mod_range_db, 3);
    }

    return s;
}

// The SFZ region of keys `low` to `high` of `z`. The region plays `voice` of `instruments` at amplitude `zone_gain`
// (ZoneGain) with `wave`, and follows the velocity by `velocity_curve` (SfzVelocityCurve). Add `gain_db` to its volume.
std::string SfzRegion(const Instruments& instruments, const Zone& z, int low, int high, const MacroVoice& voice,
                      double zone_gain, const std::string& velocity_curve, const Sample& sample,
                      const SfzSamples::Wave& wave, double gain_db)
{
    // A voice whose envelope follows the key has a region for each key.
    const int key = low + z.key_offset;
    const VolumeShape slow = VolumeOf(instruments, voice, ZoneVolume(voice, z, z.volume), key, 0);
    const VolumeShape fast = VolumeOf(instruments, voice, ZoneVolume(voice, z, z.volume), key, 127);
    const Tuning tuning = TuningOf(voice, z, sample);
    const std::optional<TremoloShape> tremolo = TremoloOf(voice);

    std::string s = "<region> sample=" + wave.name + " lokey=" + std::to_string(low) +
                    " hikey=" + std::to_string(high) + " pitch_keycenter=" + std::to_string(sample.key);
    if (!tuning.follows_key)
    {
        s += " pitch_keytrack=0";
    }
    s += seqlib::sfz::TuneOpcodes(Cents(tuning.cents));

    double volume = 20.0 * std::log10(zone_gain) + gain_db;
    if (tremolo)
    {
        volume -= tremolo->half_range_db;
    }
    volume = std::max(volume, kSfzLowestVolumeDb);
    if (std::abs(volume) >= 0.005)
    {
        s += " volume=" + Number(volume, 2);
    }

    const double pan = ZonePan(voice, z) * 100.0;
    if (std::abs(pan) >= 0.05)
    {
        s += " pan=" + Number(pan, 1);
    }
    if (voice.itd)
    {
        s += " pan_oncc10=" + Number(100.0 * kItdPan, 3);
    }
    s += velocity_curve;

    // SFZ's decays and releases fall kSfzFallDb in their time.
    const auto fall_seconds = [](double db_per_ms)
    {
        return db_per_ms == kAtOnce ? 0.0 : kSfzFallDb / db_per_ms / 1000.0;
    };
    s += seqlib::sfz::AmpEnvelopeOpcodes({.attack = slow.attack_ms / 1000.0,
                                          .hold = slow.hold_ms / 1000.0,
                                          .decay = fall_seconds(slow.decay_db_per_ms),
                                          .sustain = 100.0 * std::pow(10.0, slow.sustain_db / 20.0),
                                          .release = fall_seconds(slow.release_db_per_ms)});
    if (fast.attack_ms != slow.attack_ms)
    {
        s += " ampeg_vel2attack=" + Number((fast.attack_ms - slow.attack_ms) / 1000.0, 6);
    }
    s += seqlib::sfz::LoopOpcodes(wave.loop, wave.loop_start, wave.loop_end);

    if (voice.vibrato)
    {
        s += SfzVibrato(*voice.vibrato);
    }
    if (voice.pitch_envelope)
    {
        s += SfzPitchEnvelope(*voice.pitch_envelope, key);
    }
    if (tremolo)
    {
        s += SfzTremolo(*tremolo);
    }

    // A key group: a voice of the group stops the others.
    if (voice.key_group != 0)
    {
        s += " group=" + std::to_string(voice.key_group) + " off_by=" + std::to_string(voice.key_group);
    }

    return s + "\n";
}

// Whether `voice`'s envelopes follow the key.
bool FollowsKey(const MacroVoice& voice)
{
    return (voice.envelope && voice.envelope->dls && voice.envelope->decay_tc_per_key != 0.0) ||
           (voice.pitch_envelope && voice.pitch_envelope->envelope.decay_tc_per_key != 0.0);
}

// The SFZ group of a preset that plays `zones`, with `gain_db` added to each region's volume. The samples that the
// regions play go in `samples`.
std::string SfzGroup(const std::vector<Zone>& zones, Instruments& instruments, SfzSamples& samples, double gain_db)
{
    std::string s = "<group>\n";
    for (const Zone& z : zones)
    {
        const MacroVoice& voice = instruments.Macro(z.macro, z.part, z.from_ms);
        const Sample* sample = instruments.SampleOf(voice);
        if (sample == nullptr)
        {
            continue;
        }

        const SfzSamples::Wave& wave = samples.Get(sample->sfz_name, [sample] { return &sample->pcm; });
        const double zone_gain = ZoneGain(instruments, voice, z);
        const std::string curve = SfzVelocityCurve(instruments, voice, z);
        if (FollowsKey(voice))
        {
            for (int key = z.low_key; key <= z.high_key; key++)
            {
                s += SfzRegion(instruments, z, key, key, voice, zone_gain, curve, *sample, wave, gain_db);
            }
        }
        else
        {
            s += SfzRegion(instruments, z, z.low_key, z.high_key, voice, zone_gain, curve, *sample, wave, gain_db);
        }
    }

    return s;
}

// Build the SFZ files of `perf`, whose presets play `zones`.
SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, Instruments& instruments,
                        const std::vector<std::vector<Zone>>& zones, const std::string& title,
                        const std::vector<std::string>& preset_names)
{
    seqlib::sfz::TrackFileOptions options;
    options.title = title;
    options.program = kProgram;
    options.preset_names = preset_names;
    options.presets = perf.presets.size();
    options.level_cut_db = perf.level_cut_db;
    options.track_names = true;

    SfzSamples samples;
    auto text = [&](int)
    {
        seqlib::sfz::TrackText added;
        added.preset = [&](uint32_t p)
        {
            return SfzGroup(zones[p], instruments, samples, perf.level_cut_db);
        };

        return added;
    };
    SfzInstruments out;
    out.files = seqlib::sfz::TrackFiles(perf, midi, options, text);
    out.samples = samples.Take();

    return out;
}

// The zones of each preset of `perf`, from the song group's pages. A preset whose page the song group lacks has none.
// Add a warning for it to `warnings`, after its name in `names`.
std::vector<std::vector<Zone>> PresetZones(const Performance& perf, Instruments& instruments,
                                           const std::vector<std::string>& names, std::vector<std::string>& warnings)
{
    std::vector<std::vector<Zone>> zones(perf.presets.size());
    for (std::size_t i = 0; i < zones.size(); i++)
    {
        const std::optional<uint16_t> object = PresetObject(instruments.Group(), perf.presets[i]);
        if (!object)
        {
            warnings.push_back(names[i] + ": the song group has no page for it");
            continue;
        }

        zones[i] = ZonesOf(instruments, *object);
    }

    return zones;
}

// The loudest of the zones that play a sample, as amplitude (ZoneGain).
double LoudestZone(const std::vector<std::vector<Zone>>& zones, Instruments& instruments)
{
    double loudest = 0.0;
    for (const std::vector<Zone>& preset : zones)
    {
        for (const Zone& z : preset)
        {
            const MacroVoice& voice = instruments.Macro(z.macro, z.part, z.from_ms);
            if (instruments.SampleOf(voice) != nullptr)
            {
                loudest = std::max(loudest, ZoneGain(instruments, voice, z));
            }
        }
    }

    return loudest;
}

// Write the SoundFont and the SFZ files of `c`, whose MIDI file `midi` describes, titled `title`. The presets play
// `zones` and take `names`. The SoundFont's comment says that the files come from `source`, such as "song
// title_song". `warnings` holds the warnings so far. The builder's go before them.
void WriteInstruments(Conversion& c, const MidiReport& midi, const std::vector<std::vector<Zone>>& zones,
                      const std::vector<std::string>& names, std::vector<std::string> warnings,
                      Instruments& instruments, const std::string& title, const std::string& source)
{
    // SoundFont zones can't amplify their samples. A zone above full level makes every zone quieter. SFZ keeps the
    // levels.
    const double quieter = ntdlib::sequence_export::CutAboveFullLevel(c, LoudestZone(zones, instruments));
    SoundFontBuilder builder(instruments, quieter);
    for (std::size_t i = 0; i < zones.size(); i++)
    {
        builder.Add(static_cast<uint32_t>(i), names[i], zones[i]);
    }
    for (uint32_t preset : midi.channel_10_presets)
    {
        builder.AddChannel10Copy(preset);
    }

    const std::string comment = "Converted by " + std::string(kProgram) + " from " + source + ".";
    ntdlib::sequence_export::WriteSoundFont(c, builder.Result(), title, comment, kProgram);
    c.sfz = BuildSfz(c.performance, midi, instruments, zones, title, names);

    warnings.insert(warnings.begin(), builder.Warnings().begin(), builder.Warnings().end());
    ntdlib::sequence_export::CollectWarnings(c, midi, warnings, {});
}

// The data of the sample that `info` describes in `data`, a samples file: 8 bytes for each frame of 14 DSP-ADPCM
// samples, 2 for each 16-bit sample and 1 for each 8-bit sample. Empty if the data runs past the file's end.
std::span<const uint8_t> SampleBytes(const ntdlib::musyx::SampleInfo& info, std::span<const uint8_t> data)
{
    constexpr std::size_t kFrameSamples = 14, kFrameBytes = 8;
    std::size_t size = info.length;
    if (info.format == ntdlib::musyx::SampleFormat::kAdpcm || info.format == ntdlib::musyx::SampleFormat::kAdpcmPlus)
    {
        size = (size + kFrameSamples - 1) / kFrameSamples * kFrameBytes;
    }
    else if (info.format == ntdlib::musyx::SampleFormat::kPcm16)
    {
        size *= 2;
    }

    if (info.offset > data.size() || size > data.size() - info.offset)
    {
        return {};
    }

    return data.subspan(info.offset, size);
}

// Whether the sample of `a` in the samples file `a_data` and that of `b` in `b_data` are the same: their entries agree
// apart from the offset, and their data is the same.
bool SameSample(const ntdlib::musyx::SampleInfo& a, std::span<const uint8_t> a_data, const ntdlib::musyx::SampleInfo& b,
                std::span<const uint8_t> b_data)
{
    const auto entry = [](const ntdlib::musyx::SampleInfo& s)
    {
        return std::tie(s.id, s.key, s.rate, s.format, s.length, s.loop_start, s.loop_length);
    };
    const auto adpcm = [](const ntdlib::musyx::AdpcmInfo& d)
    {
        return std::tie(d.pred_scale, d.loop_pred_scale, d.loop_hist1, d.loop_hist2, d.coefficients);
    };
    if (entry(a) != entry(b) || a.adpcm.has_value() != b.adpcm.has_value() ||
        (a.adpcm && adpcm(*a.adpcm) != adpcm(*b.adpcm)))
    {
        return false;
    }

    return std::ranges::equal(SampleBytes(a, a_data), SampleBytes(b, b_data));
}

} // namespace

Conversion ConvertSong(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments,
                       const PerformOptions& options)
{
    Conversion c;
    c.performance = PlaySong(song, listed, instruments, options);
    const MidiReport midi = ntdlib::sequence_export::WriteMidiFile(c, listed.name);

    const std::vector<std::string> names = PresetNames(c.performance.presets);
    std::vector<std::string> warnings;
    const std::vector<std::vector<Zone>> zones = PresetZones(c.performance, instruments, names, warnings);
    WriteInstruments(c, midi, zones, names, std::move(warnings), instruments, listed.name, "song " + listed.name);

    return c;
}

Conversion ConvertEffect(const GameEffect& effect, Instruments& instruments)
{
    Conversion c;
    c.performance = PlayEffect(effect, instruments);
    const MidiReport midi = ntdlib::sequence_export::WriteMidiFile(c, effect.name);

    // Each of the effect's presets plays one of its notes.
    std::set<std::string> left_out;
    std::vector<std::vector<Zone>> zones;
    for (const EffectNote& note : EffectNotes(effect, instruments, left_out))
    {
        zones.push_back(ZonesOf(instruments, note));
    }
    zones.resize(c.performance.presets.size());
    const std::vector<std::string> names = PresetNames(c.performance.presets);
    WriteInstruments(c, midi, zones, names, {}, instruments, effect.name, "effect " + effect.name);

    return c;
}

std::vector<std::map<uint16_t, std::string>> SfzSampleNames(const std::vector<std::shared_ptr<const SongGroup>>& groups)
{
    // Each group's samples by their IDs. Instruments::GetSample takes an ID's first entry.
    std::vector<std::map<uint16_t, const ntdlib::musyx::SampleInfo*>> entries(groups.size());
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        for (const ntdlib::musyx::SampleInfo& info : groups[g]->samples)
        {
            entries[g].try_emplace(info.id, &info);
        }
    }

    // A sample takes the index of the first group that holds it, unless that group is the first to hold its ID.
    std::vector<std::map<uint16_t, std::string>> names(groups.size());
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        for (const auto& [id, info] : entries[g])
        {
            std::optional<std::size_t> first_holder;
            std::size_t first_same = g;
            for (std::size_t h = 0; h < g; h++)
            {
                const auto held = entries[h].find(id);
                if (held == entries[h].end())
                {
                    continue;
                }

                first_holder = first_holder.value_or(h);
                if (SameSample(*held->second, groups[h]->sample_data, *info, groups[g]->sample_data))
                {
                    first_same = h;
                    break;
                }
            }

            const std::string number = std::to_string(id);
            names[g][id] =
                first_same == first_holder.value_or(g) ? "s" + number : "s" + std::to_string(first_same) + "_" + number;
        }
    }

    return names;
}

std::map<uint16_t, std::string> GameInput::SfzNames(std::size_t group) const
{
    const std::vector<std::shared_ptr<const SongGroup>>& groups = game_.Groups();
    std::map<uint16_t, std::string> names = sfz_names_.at(group);
    for (const std::shared_ptr<const SongGroup>& pushed : groups[group]->pushed)
    {
        const auto found = std::find(groups.begin(), groups.end(), pushed);
        if (found != groups.end())
        {
            const std::map<uint16_t, std::string>& more = sfz_names_[static_cast<std::size_t>(found - groups.begin())];
            names.insert(more.begin(), more.end());
        }
    }

    return names;
}

GameInput::GameInput(Game game)
    : game_(std::move(game)),
      instruments_(game_.Groups().size()),
      errors_reported_(game_.Groups().size()),
      sfz_names_(SfzSampleNames(game_.Groups()))
{
}

Conversion GameInput::Convert(std::size_t index, const PerformOptions& perform)
{
    const std::vector<GameSong>& songs = game_.Songs();
    const std::size_t group =
        index < songs.size() ? songs[index].group : game_.Effects().at(index - songs.size()).group;
    std::unique_ptr<Instruments>& instruments = instruments_.at(group);
    if (!instruments)
    {
        instruments = std::make_unique<Instruments>(*game_.Groups()[group], game_.Volumes(), SfzNames(group));
    }

    Conversion c;
    if (index < songs.size())
    {
        const std::vector<uint8_t> file = game_.ReadSong(index);
        c = ConvertSong(ntdlib::musyx::ReadSong(file), songs[index], *instruments, perform);
    }
    else
    {
        c = ConvertEffect(game_.Effects()[index - songs.size()], *instruments);
    }

    std::size_t& reported = errors_reported_[group];
    for (; reported < instruments->Errors().size(); reported++)
    {
        c.warnings.push_back(instruments->Errors()[reported]);
    }

    return c;
}

} // namespace dolphinsf2::musyx
