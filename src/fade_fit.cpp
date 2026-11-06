// SPDX-License-Identifier: MIT

#include "fade_fit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "seqlib/sf2/units.h"

namespace dolphinsf2
{
namespace
{

namespace sf2 = seqlib::sf2;

// A fade is fitted down to the point where it lies 20 dB down, or down to the level that it holds if that's higher.
constexpr double kFitDb = 20.0;

// A release is fitted to reach the level that it reaches this far down. It follows the line in amplitude closely until
// then.
constexpr double kReleaseFitDb = 14.0;

// The holds and the points of the curve that the fit of a decay tries.
constexpr int kFitHolds = 32;
constexpr int kFitPoints = 64;

// The shortest span in milliseconds that a decay's fit covers.
constexpr double kShortestSpanMs = 1.0;

// The lowest amplitude that the fit reads, against the peak.
constexpr double kLowestAmplitude = 1e-6;

// The decibels that a SoundFont envelope's decay and release fall in their time, and those that an SFZ envelope's fall:
// a factor of e^-8 (sfzformat.com, ampeg_attack).
constexpr double kSoundFontFallDb = 100.0;
constexpr double kSfzFallDb = 8.0 * 20.0 / 2.302585092994046;

// SoundFont's limits: silence in centibels, the longest envelope time and the longest hold that holdVolEnv allows.
constexpr int kSilentCentibels = 1440;
constexpr int kLongestTimecents = 8000;
constexpr int kLongestHoldTimecents = 5000;

// The holds and the points of the curve that the choice of a hold before SoundFont's slowest decay tries.
constexpr int kSoundFontHolds = 64;
constexpr int kSoundFontPoints = 64;

// The milliseconds of `timecents`.
double TimecentsMs(int timecents)
{
    return 1000.0 * std::exp2(timecents / 1200.0);
}

// The timecents of a fall at `db_per_ms` that falls kSoundFontFallDb in its time. A rate of 0 never falls: it takes the
// longest time.
int FallTimecents(double db_per_ms)
{
    if (db_per_ms <= 0.0)
    {
        return kLongestTimecents;
    }

    return sf2::ToTimecents(kSoundFontFallDb / db_per_ms, -12000, kLongestTimecents);
}

// The seconds of a fall at `db_per_ms` that falls kSfzFallDb in its time.
double FallSeconds(double db_per_ms)
{
    return db_per_ms <= 0.0 ? 100.0 : kSfzFallDb / db_per_ms / 1000.0;
}

// The hold, up to the longest, that brings a SoundFont zone closest to `curve` by least squares in decibels. The
// slowest decay follows the hold down to the sustain of `shape`. The comparison runs from the peak until that decay
// reaches the sustain after the longest hold. A curve whose slow fall starts at once takes a long hold. One that drops
// quickly first and then falls slowly takes a short hold.
double SoundFontHold(const FadeCurve& curve, const FadeShape& shape)
{
    const double slowest = kSoundFontFallDb / TimecentsMs(kLongestTimecents);
    const double longest = TimecentsMs(kLongestHoldTimecents);
    const double drop = -shape.sustain_db;
    const double span = longest + drop / slowest;
    double best = std::numeric_limits<double>::max();
    double best_hold = 0.0;
    for (int h = 0; h <= kSoundFontHolds; h++)
    {
        const double hold = longest * h / kSoundFontHolds;
        double error = 0.0;
        for (int i = 0; i < kSoundFontPoints; i++)
        {
            const double t = span * (i + 0.5) / kSoundFontPoints;
            const double curve_db =
                20.0 * std::log10(std::max(FadeAmplitude(curve, shape.attack_ms + t) / shape.peak, kLowestAmplitude));
            const double zone_db = -std::min(std::max(t - hold, 0.0) * slowest, drop);
            error += (curve_db - zone_db) * (curve_db - zone_db);
        }

        if (error < best)
        {
            best = error;
            best_hold = hold;
        }
    }

    return best_hold;
}

} // namespace

double FadeAmplitude(const FadeCurve& curve, double ms)
{
    if (curve.empty())
    {
        return 0.0;
    }

    for (std::size_t i = 1; i < curve.size(); i++)
    {
        const auto [t0, l0] = curve[i - 1];
        const auto [t1, l1] = curve[i];
        if (ms <= t1)
        {
            return t1 > t0 ? l0 + (l1 - l0) * std::max(ms - t0, 0.0) / (t1 - t0) : l1;
        }
    }

    return curve.back().second;
}

FadeShape FitFades(const FadeCurve& curve)
{
    FadeShape shape;
    std::size_t peak = 0;
    for (std::size_t i = 1; i < curve.size(); i++)
    {
        if (curve[i].second > curve[peak].second)
        {
            peak = i;
        }
    }

    shape.peak = curve.empty() ? 0.0 : curve[peak].second;
    if (shape.peak <= 0.0)
    {
        shape.peak = 0.0;
        return shape;
    }

    const double start = curve[peak].first;
    shape.attack_ms = start;

    const double hold = curve.back().second / shape.peak;
    shape.sustain_db = hold > 0.0 ? 20.0 * std::log10(hold) : -kSilentCentibels / 10.0;
    if (hold >= 1.0)
    {
        return shape;
    }

    // The time at which the curve after the peak falls to the fitted level: 20 dB down, or the held level.
    const double fit = std::max(hold, std::pow(10.0, -kFitDb / 20.0)) * shape.peak;
    double reached = curve.back().first;
    for (std::size_t i = peak + 1; i < curve.size(); i++)
    {
        const auto [t0, l0] = curve[i - 1];
        const auto [t1, l1] = curve[i];
        if (l1 <= fit)
        {
            reached = l0 == l1 ? t0 : t0 + (t1 - t0) * (l0 - fit) / (l0 - l1);
            break;
        }
    }

    const double span = std::max(reached - start, kShortestSpanMs);

    // A fade in a line in amplitude starts slowly in decibels. A hold and then a decay follow it down to the fitted
    // level. Each of kFitHolds holds gets the decay that fits its points best by least squares. The best of them gives
    // the shape.
    double best = std::numeric_limits<double>::max();
    for (int h = 0; h < kFitHolds; h++)
    {
        const double held_for = span * h / kFitHolds;
        double error = 0.0;
        double xx = 0.0;
        double xy = 0.0;
        double yy = 0.0;
        for (int i = 0; i < kFitPoints; i++)
        {
            const double t = span * (i + 0.5) / kFitPoints;
            const double y =
                -20.0 * std::log10(std::max(FadeAmplitude(curve, start + t) / shape.peak, kLowestAmplitude));
            if (t <= held_for)
            {
                error += y * y;
                continue;
            }
            const double x = t - held_for;
            xx += x * x;
            xy += x * y;
            yy += y * y;
        }
        if (xx <= 0.0 || xy <= 0.0)
        {
            continue;
        }
        error += yy - xy * xy / xx;
        if (error < best)
        {
            best = error;
            shape.hold_ms = held_for;
            shape.decay_db_per_ms = xy / xx;
        }
    }

    // SoundFont's slowest decay falls faster than such a decay. A SoundFont zone then holds before it.
    if (shape.decay_db_per_ms > 0.0 && kSoundFontFallDb / shape.decay_db_per_ms > TimecentsMs(kLongestTimecents))
    {
        shape.soundfont_hold_ms = SoundFontHold(curve, shape);
    }

    return shape;
}

double FitRelease(double ms)
{
    return kReleaseFitDb / std::max((1.0 - std::pow(10.0, -kReleaseFitDb / 20.0)) * ms, 0.01);
}

void SetEnvelope(const FadeShape& shape, sf2::ZoneValues& values)
{
    values.attack = sf2::ToTimecents(shape.attack_ms, -12000, kLongestTimecents);
    values.hold = sf2::ToTimecents(shape.soundfont_hold_ms.value_or(shape.hold_ms), -12000, kLongestHoldTimecents);
    values.decay = shape.decay_db_per_ms > 0.0 ? FallTimecents(shape.decay_db_per_ms) : -12000;
    values.sustain = std::clamp(static_cast<int>(std::lround(-10.0 * shape.sustain_db)), 0, kSilentCentibels);
    values.release = FallTimecents(shape.release_db_per_ms);
}

seqlib::sfz::AmpEnvelope SfzEnvelope(const FadeShape& shape)
{
    return {.attack = shape.attack_ms / 1000.0,
            .hold = shape.hold_ms / 1000.0,
            .decay = shape.decay_db_per_ms > 0.0 ? FallSeconds(shape.decay_db_per_ms) : 0.0,
            .sustain = 100.0 * std::pow(10.0, shape.sustain_db / 20.0),
            .release = FallSeconds(shape.release_db_per_ms)};
}

} // namespace dolphinsf2
