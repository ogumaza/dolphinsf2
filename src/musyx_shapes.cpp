// SPDX-License-Identifier: MIT

// The shapes of a MusyX voice (see musyx_shapes.h).

#include "musyx_shapes.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "jaudio.h"

namespace dolphinsf2::musyx::shapes
{
namespace
{

// A macro's fades and the fade of a stopped voice are fitted to reach the level that they reach kFitDb down. A fade to
// a level less than twice as far down is fitted halfway down in decibels. kFitShare is the share of the amplitude at
// the start of a fade that remains kFitDb down.
const double kFitShare = std::pow(10.0, -kFitDb / 20.0);

// The step in MusyX milliseconds in which FadeRate follows a macro's fade, and the most steps it takes. A fade longer
// than 25 s takes longer steps.
constexpr double kFadeStepMs = 0.25;
constexpr double kMostFadeSteps = 100000.0;

// The shortest delay of a volume envelope in real milliseconds. SoundFont's shortest delay is -12000 timecents, about
// 1 ms. A shorter delay stays out of both files.
constexpr double kShortestDelayMs = 1.0;

// The sustain level that DLS envelopes treat as silence: their voice ends after the decay.
constexpr double kDlsSilence = 1.0e-4; // below -80 dB, above -96.5 dB

// The rate in decibels a millisecond of a fall that is linear in amplitude, from full level to `to` in `ms`, fitted as
// a fall that is linear in decibels. A fall to a level under kSilentSustain is fitted by its energy (kEnergyFitDb).
double LinearFallRate(double ms, double to)
{
    const double range_db = to > 0.0 ? -20.0 * std::log10(to) : kAtOnce;
    if (ms <= 0.0 || range_db <= 0.0)
    {
        return kAtOnce;
    }
    if (to < kSilentSustain)
    {
        return kEnergyFitDb / ms;
    }

    // A fall to a sustain above silence reaches the level halfway down in decibels when the game's line does.
    const double fit_db = range_db / 2.0;
    const double level = std::pow(10.0, -fit_db / 20.0);
    return fit_db / (ms * (1.0 - level) / (1.0 - to));
}

// The rate in decibels a millisecond of the fade that ends a stopped voice, a line from full level to silence in `ms`.
// It reaches the level kFitDb down when the line does.
double CutRate(double ms)
{
    return kFitDb / (ms * (1.0 - kFitShare));
}

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
// `decay_ms`. The files play the attack `played_attack_ms`: the envelope's or the fade-in's, the longer. The note's
// level is the envelope's amplitude times the curve's level of the volume that the ramps give, over the level at
// `volume`. A voice that stops by itself falls to silence in kCutMs from its stop. The decay starts where the level
// first falls, after the played attack. The hold fills the time between them. The decay reaches the level kFitDb down
// when the note does, or halfway down in decibels to a level less than twice as far down. Return nothing if the level
// doesn't fall.
std::optional<HeldShape> HeldShapeOf(const Instruments& instruments, const MacroVoice& voice, double volume,
                                     double attack_ms, double decay_ms, double played_attack_ms)
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
    const double start = std::max(falls, played_attack_ms);
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
    shape.hold_ms = start - played_attack_ms;
    shape.decay_db_per_ms = crossed > start ? fit_db / (crossed - start) : kAtOnce;
    shape.sustain_db = last > 0.0 ? -range_db : -kSilentCentibels / 10.0;

    return shape;
}

// The delay in MusyX milliseconds before the attack of `voice` at `volume`, whose fade-in of `fade_ms` sets the attack.
// FadeIn moves the volume in a line from silence (0x80153c10 in Pokémon Colosseum), and the voice's curve gives its
// level. The envelope's attack of `attack_ms` and its decay of `decay_ms`, if the voice has an envelope, scale that
// level. The files' attack rises in a line in amplitude. After the delay, the attack reaches the level kFitDb below the
// fade's end when the voice does. It ends when the fade does.
double FadeInDelayMs(const Instruments& instruments, const MacroVoice& voice, double volume, double attack_ms,
                     double decay_ms, double fade_ms)
{
    const double full = instruments.Level(voice, volume);
    if (full <= 0.0)
    {
        return 0.0;
    }

    const std::optional<Envelope>& e = voice.envelope;
    const auto level = [&](double ms)
    {
        double l = instruments.Level(voice, volume * std::min(ms / fade_ms, 1.0)) / full;
        if (e)
        {
            l *= HeldEnvelopeAmplitude(*e, attack_ms, decay_ms, ms);
        }

        return l;
    };

    // The time at which the voice's level reaches the fit's, step by step from the voice's start.
    const double fit = kFitShare * level(fade_ms);
    const double step = std::max(kFadeStepMs, fade_ms / kMostFadeSteps);
    double crossed = fade_ms;
    double before = 0.0;
    for (double at = step; at < fade_ms; at += step)
    {
        const double l = level(at);
        if (l >= fit)
        {
            crossed = at - step * (l - fit) / (l - before);
            break;
        }
        before = l;
    }

    return std::max((crossed - kFitShare * fade_ms) / (1.0 - kFitShare), 0.0);
}

// The rate in decibels a MusyX millisecond of the release that fits the fade of `voice` after its note's release
// (KeyOff::kFade), for the voice at `volume`. The fade's ramps move the volume in lines, and the voice's curve gives
// their levels. A KeyOff at the stop then plays the envelope's release (ReleaseFade::releases): a linear one falls in a
// line in amplitude to silence in its time, and a DLS one falls kDlsFallDb in its time. Any other stop ends the voice
// with a fade of kCutMs, in a line in amplitude. The release reaches the level kFitDb down when the voice does.
double FadeRate(const Instruments& instruments, const MacroVoice& voice, double volume)
{
    const double full = instruments.Level(voice, volume);
    if (full <= 0.0)
    {
        return kAtOnce;
    }

    // The fade's level step by step, up to the stop.
    const double fit = full * kFitShare;
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

    // The level at the stop. A voice without an envelope ends with the cut of kCutMs.
    const double last =
        instruments.Level(voice, FadeVolume(voice.release_fade, volume, std::max(stop_ms - step, 0.0)).value());
    const std::optional<Envelope>& e = voice.envelope;
    if (voice.release_fade.releases && e && e->release_ms > 0.0)
    {
        if (e->dls)
        {
            return kFitDb / (stop_ms + e->release_ms * 20.0 * std::log10(last / fit) / kDlsFallDb);
        }

        return kFitDb / (stop_ms + e->release_ms * (1.0 - fit / last));
    }

    // The cut from the level at the stop.
    return kFitDb / (stop_ms + kCutMs * (1.0 - fit / last));
}

} // namespace

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
            attack_ms = e->times_in_ms ? e->attack_ms
                                       : DlsMilliseconds(e->attack_timecents + velocity * e->attack_tc_per_velocity);
            decay_ms = e->times_in_ms ? e->decay_ms : DlsMilliseconds(e->decay_timecents + key * e->decay_tc_per_key);
            const double real_decay_ms = decay_ms * kMsScale;
            shape.decay_db_per_ms = real_decay_ms > 0.0 ? kDlsFallDb / real_decay_ms : kAtOnce;
        }
        else
        {
            attack_ms = e->attack_ms;
            decay_ms = e->decay_ms;
            shape.decay_db_per_ms = LinearFallRate(decay_ms * kMsScale, e->sustain);
        }

        const bool silent = dls ? e->sustain < kDlsSilence : e->sustain <= 0.0;
        shape.sustain_db = silent ? -kSilentCentibels / 10.0 : 20.0 * std::log10(e->sustain);
    }

    // A fade-in at the voice's start becomes the attack. The longer of the two plays. A fade-in that plays starts after
    // a delay (FadeInDelayMs).
    const double fade_ms = std::max(voice.fade_in_ms - voice.sample_ms, 0.0);
    const double played_attack_ms = std::max(attack_ms, fade_ms);
    if (fade_ms > attack_ms)
    {
        const double delay_ms = FadeInDelayMs(instruments, voice, volume, attack_ms, decay_ms, fade_ms) * kMsScale;
        shape.delay_ms = delay_ms >= kShortestDelayMs ? delay_ms : 0.0;
    }
    shape.attack_ms = played_attack_ms * kMsScale - shape.delay_ms;

    // The fades of a held note, with the envelope's decay, give the hold, the decay and the sustain (HeldShapeOf).
    const std::optional<HeldShape> held =
        voice.held_ramps.empty() ? std::nullopt
                                 : HeldShapeOf(instruments, voice, volume, attack_ms, decay_ms, played_attack_ms);
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
    if (release_ms <= 0.0)
    {
        return shape;
    }

    // A DLS release falls kDlsFallDb in its time. A linear release falls from where it starts to silence in its time.
    // A stopped voice fades out in kCutMs.
    if (dls)
    {
        shape.release_db_per_ms = kDlsFallDb / release_ms;
    }
    else if (voice.key_off == KeyOff::kRelease)
    {
        shape.release_db_per_ms = LinearFallRate(release_ms, 0.0);
    }
    else
    {
        shape.release_db_per_ms = CutRate(release_ms);
    }

    return shape;
}

PitchShape PitchOf(const PitchEnvelope& envelope, int key)
{
    const Envelope& e = envelope.envelope;
    PitchShape p;
    p.attack_ms = e.attack_ms * kMsScale;
    const double decay_ms = DlsMilliseconds(e.decay_timecents + key * e.decay_tc_per_key) * kMsScale;
    const double release_ms = e.release_ms * kMsScale;
    p.sustain = e.sustain < kDlsSilence ? 0.0 : e.sustain;
    p.range_cents = envelope.range_cents;

    // MusyX reaches the middle of the decay's range, or half of the release's, after a fall of `db`. A sustain at full
    // level leaves the decay no range. The decay then takes the limit of `db` over the range, 20 / ln 10.
    const double middle = (1.0 + p.sustain) / 2.0;
    const double db = -20.0 * std::log10(middle);
    p.decay_ms =
        middle < 1.0 ? decay_ms * db / kDlsFallDb / (1.0 - middle) : decay_ms * 20.0 / std::log(10.0) / kDlsFallDb;
    p.release_ms = release_ms * 20.0 * std::log10(2.0) / kDlsFallDb * 2.0;
    p.sfz_decay_ms = decay_ms * kSfzFallDb / kDlsFallDb;
    p.sfz_release_ms = release_ms * kSfzFallDb / kDlsFallDb;

    return p;
}

double LfoHz(double period_ms)
{
    return 1000.0 / (period_ms * kMsScale);
}

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
    return TremoloShape{LfoHz(t.period_ms), range / 2.0, (range_db(t.full_mod_depth) - range) / 2.0};
}

double WheelLfoCents(const MacroVoice& voice, const WheelLfo& lfo)
{
    return lfo.share * 100.0 * (voice.bend_up + voice.bend_down) / 2.0;
}

std::optional<WheelLfo> PlayedWheelLfo(const MacroVoice& voice)
{
    const std::optional<TremoloShape> tremolo = TremoloOf(voice);
    if (!voice.wheel_lfo ||
        (tremolo && (voice.wheel_lfo->delay_ms > 0.0 || voice.wheel_lfo->period_ms != voice.tremolo->period_ms)))
    {
        return std::nullopt;
    }

    return voice.wheel_lfo;
}

int ShapeFallTimecents(double db_per_ms)
{
    return db_per_ms == kAtOnce ? kShortestTimecents
                                : ToTimecents(kSoundFontFallDb / db_per_ms, kShortestTimecents, kLongestTimecents);
}

int KeyScaleTimecents(double per_key)
{
    constexpr int kWidestKeyScale = 1200;
    return std::clamp(static_cast<int>(std::lround(-per_key)), -kWidestKeyScale, kWidestKeyScale);
}

} // namespace dolphinsf2::musyx::shapes
