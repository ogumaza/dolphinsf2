// SPDX-License-Identifier: MIT

// JAudio2's oscillators (see envelope.h). The state machine and its curve tables follow JASOscillator: initStart,
// incCounter, release, update and updateCurrentValue.

#include "envelope.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace dolphinsf2
{
namespace
{

// How far along a segment's curve its level has moved from the segment's start (1) to its target (0), in 16 steps:
// linear, square, square root and "sample cell" (JASOscillator's curve tables, at 0x8045d098, 0x8045d164, 0x8045d120
// and 0x8045d0dc in Four Swords Adventures' main.dol).
constexpr std::array<std::array<float, 17>, 4> kCurves{{
    {1.0f, 0.9375f, 0.875f, 0.8125f, 0.75f, 0.6875f, 0.625f, 0.5625f, 0.5f, 0.4375f, 0.375f, 0.3125f, 0.25f, 0.1875f,
     0.125f, 0.0625f, 0.0f},
    {1.0f, 0.9682459831237793f, 0.9354140162467957f, 0.9013879895210266f, 0.8660249710083008f, 0.8291559815406799f,
     0.790569007396698f, 0.75f, 0.7071070075035095f, 0.66143798828125f, 0.6123719811439514f, 0.55901700258255f, 0.5f,
     0.433012992143631f, 0.35355299711227417f, 0.25f, 0.0f},
    {1.0f, 0.8789060115814209f, 0.765625f, 0.6601560115814209f, 0.5625f, 0.4726560115814209f, 0.390625f,
     0.3164060115814209f, 0.25f, 0.1914059966802597f, 0.140625f, 0.09765619784593582f, 0.0625f, 0.03515620157122612f,
     0.015625f, 0.00390625f, 0.0f},
    {1.0f, 0.9704890251159668f, 0.7812740206718445f, 0.5462809801101685f, 0.39979198575019836f, 0.28931498527526855f,
     0.21210399270057678f, 0.15747599303722382f, 0.1126129999756813f, 0.08178959786891937f, 0.057985201478004456f,
     0.04364150017499924f, 0.03082370012998581f, 0.023712899535894394f, 0.015259300358593464f, 0.00915555004030466f,
     0.0f},
}};

// Point modes that end a segment run.
constexpr int16_t kModeLoop = 13;
constexpr int16_t kModeHold = 14;
constexpr int16_t kModeStop = 15;

// The longest curve traced, in sub-frames: a minute.
constexpr std::size_t kMaxTrace = static_cast<std::size_t>(60.0 / kSubFrameSeconds);

// Points passed in one update before the oscillator counts as stuck: a table of negative times that loops back.
constexpr int kMaxPointsPerUpdate = 1000;

// A level as dB, with silence at -200 dB.
double Db(double level)
{
    return level > 1e-10 ? 20.0 * std::log10(level) : -200.0;
}

} // namespace

void OscillatorState::Start(const Oscillator& osc)
{
    // initStart, and the update that the channel's first sub-frame makes before it reads the value.
    osc_ = &osc;
    counter_ = 0.0f;
    level_ = 0.0f;
    from_ = 0.0f;
    point_ = 0;
    direct_release_ = 0;
    if (osc.attack.empty())
    {
        state_ = State::kHold;
        level_ = 1.0f;
        return;
    }

    target_ = static_cast<float>(osc.attack[0].value) / 32768.0f;
    curve_ = osc.attack[0].mode;
    state_ = State::kAttack;
    Update();
}

void OscillatorState::Advance(float step)
{
    if (state_ == State::kStopped || state_ == State::kHold)
    {
        return;
    }

    counter_ += step * osc_->rate;
    Update();
}

void OscillatorState::Release(uint16_t direct_release, uint16_t default_release)
{
    if (osc_ == nullptr)
    {
        return;
    }

    if (osc_->release.empty() && direct_release == 0)
    {
        direct_release = default_release;
    }
    if (osc_->release.empty() && direct_release == 0)
    {
        state_ = State::kStopped;
        return;
    }

    if (direct_release != 0)
    {
        counter_ = 0.0f;
        from_ = level_;
        target_ = 0.0f;
        point_ = 0;
        curve_ = (direct_release >> 14) & 3;
        direct_release_ = direct_release;
        state_ = State::kDirectRelease;
        Update();
        return;
    }

    // A release table that is the attack table carries on from where the attack is.
    if (!osc_->release_is_attack)
    {
        counter_ = 0.0f;
        from_ = level_;
        target_ = static_cast<float>(osc_->release[0].value) / 32768.0f;
        point_ = 0;
        curve_ = osc_->release[0].mode;
    }
    state_ = State::kRelease;
    Update();
}

float OscillatorState::Value() const
{
    if (osc_ == nullptr)
    {
        return 1.0f;
    }

    return level_ * osc_->scale + osc_->vertex;
}

void OscillatorState::Update()
{
    if (state_ == State::kDirectRelease)
    {
        const auto duration = static_cast<float>(direct_release_ & 0x3fff);
        if (counter_ < duration)
        {
            Interpolate(duration);
        }
        else
        {
            level_ = target_;
            state_ = State::kStopped;
        }
        return;
    }
    if (state_ == State::kHold)
    {
        return;
    }

    // Like JASOscillator::update, a stopped oscillator reads its release table.
    const std::vector<OscPoint>& table = state_ == State::kAttack ? osc_->attack : osc_->release;
    if (table.empty())
    {
        state_ = State::kHold;
        return;
    }

    // A track can replace its table while a note plays it (JASTrack::overwriteOsc). The new table can be shorter.
    point_ = std::min(point_, table.size() - 1);
    for (int passed = 0; counter_ >= static_cast<float>(table[point_].time); passed++)
    {
        if (passed >= kMaxPointsPerUpdate)
        {
            state_ = State::kStopped;
            return;
        }

        counter_ -= static_cast<float>(table[point_].time);
        level_ = target_;
        point_ = std::min(point_ + 1, table.size() - 1);
        from_ = level_;
        const OscPoint& p = table[point_];
        if (p.mode == kModeStop)
        {
            state_ = State::kStopped;
            return;
        }
        if (p.mode == kModeHold)
        {
            state_ = State::kHold;
            return;
        }
        if (p.mode == kModeLoop)
        {
            // The loop's target point keeps the segment's target and curve, as in the game.
            point_ = std::clamp<std::size_t>(static_cast<uint16_t>(p.value), 0, table.size() - 1);
            continue;
        }

        curve_ = p.mode;
        target_ = static_cast<float>(p.value) / 32768.0f;
    }

    Interpolate(static_cast<float>(table[point_].time));
}

void OscillatorState::Interpolate(float duration)
{
    const std::array<float, 17>& curve = kCurves[static_cast<std::size_t>(curve_ & 3)];
    const float position = 16.0f * (counter_ / duration);
    const auto index = std::min<std::size_t>(static_cast<std::size_t>(std::max(position, 0.0f)), 15);
    const float fraction = position - static_cast<float>(index);
    const float along = (1.0f - fraction) * curve[index] + fraction * curve[index + 1];
    level_ = from_ * along + target_ * (1.0f - along);
}

EnvelopeCurve TraceEnvelope(const std::vector<const Oscillator*>& volume_oscillators, uint16_t direct_release,
                            uint16_t default_release)
{
    // A note with no volume oscillator plays at full level. Drums get a constant one. Their direct release can then
    // fade them (JASDrumSet::getParam).
    static const Oscillator kConstant;
    std::vector<OscillatorState> oscs(std::max<std::size_t>(volume_oscillators.size(), 1));
    for (std::size_t i = 0; i < volume_oscillators.size(); i++)
    {
        oscs[i].Start(*volume_oscillators[i]);
    }
    if (volume_oscillators.empty() && direct_release != 0)
    {
        oscs[0].Start(kConstant);
    }

    auto level = [&]()
    {
        float product = 1.0f;
        for (const OscillatorState& osc : oscs)
        {
            product *= osc.Value();
        }

        return std::max(product, 0.0f);
    };

    auto settled = [&]()
    {
        return std::ranges::all_of(oscs, [](const OscillatorState& o) { return !o.Valid() || o.Holding(); });
    };

    EnvelopeCurve curve;
    curve.attack.push_back(level());
    float peak = curve.attack.back();
    std::vector<OscillatorState> peak_oscs = oscs;
    while (!oscs[0].Stopped() && !settled() && curve.attack.size() < kMaxTrace)
    {
        for (OscillatorState& osc : oscs)
        {
            osc.Advance(kOscStep);
        }
        curve.attack.push_back(level());
        if (curve.attack.back() > peak)
        {
            peak = curve.attack.back();
            peak_oscs = oscs;
        }
    }
    curve.holds = !oscs[0].Stopped();

    // Notes on a curve that stops, or falls to silence, are released before then. Their release is traced from the peak
    // instead.
    if (!curve.holds || level() <= 1e-10f)
    {
        oscs = peak_oscs;
    }

    for (std::size_t i = 0; i < oscs.size(); i++)
    {
        oscs[i].Release(i == 0 ? direct_release : 0, default_release);
    }
    curve.release.push_back(level());
    while (oscs[0].Valid() && !oscs[0].Stopped() && curve.release.size() < kMaxTrace)
    {
        for (OscillatorState& osc : oscs)
        {
            osc.Advance(kOscStep);
        }
        curve.release.push_back(level());
    }

    return curve;
}

namespace
{

// The shape that both conversions share: times in seconds, levels in dB relative to the peak.
struct Shape
{
    double delay = 0.0, attack = 0.0, hold = 0.0;
    double decay_db_per_second = 0.0; // 0: none
    double sustain_db = 0.0;
    double release_db_per_second = 1e9;
    double peak_db = 0.0;
};

// The time after `from` at which `levels` (in dB relative to their first) first fall to `db` or below, interpolated
// between sub-frames. Nothing if they never do.
std::optional<double> TimeToFall(const std::vector<float>& levels, std::size_t from, double start_db, double db)
{
    for (std::size_t i = from + 1; i < levels.size(); i++)
    {
        const double now = Db(levels[i]) - start_db;
        if (now <= db)
        {
            const double before = Db(levels[i - 1]) - start_db;
            const double fraction = before > now ? (before - db) / (before - now) : 1.0;
            return (static_cast<double>(i - 1 - from) + fraction) * kSubFrameSeconds;
        }
    }

    return std::nullopt;
}

// The dB per second of a fall linear in dB that reaches the level the game's curve reaches when it has fallen half its
// range, or 20 dB if it falls further than 40.
double FitFall(const std::vector<float>& levels, std::size_t from, double start_db, double end_db)
{
    const double fall = end_db - start_db; // <= 0
    const double match = std::max(fall / 2.0, -20.0);
    if (match > -0.01)
    {
        return 0.0;
    }

    const std::optional<double> t = TimeToFall(levels, from, start_db, match);
    if (!t || *t <= 0.0)
    {
        return 1e9; // at once
    }

    return -match / *t;
}

Shape FitShape(const EnvelopeCurve& curve)
{
    Shape s;
    const std::vector<float>& a = curve.attack;
    const float peak = std::max(*std::max_element(a.begin(), a.end()), 1e-10f);
    s.peak_db = Db(peak);

    // The attack: from the last sub-frame below -60 dB before the peak to the first within 0.1 dB of it.
    const auto at_peak = static_cast<std::size_t>(
        std::find_if(a.begin(), a.end(), [&](float v) { return v >= peak * 0.9886f; }) - a.begin());
    std::size_t rise = 0;
    for (std::size_t i = 0; i < at_peak; i++)
    {
        if (a[i] <= peak * 0.001f)
        {
            rise = i;
        }
    }
    if (at_peak > 0)
    {
        // A delay shorter than 10 ms is part of the attack's rise from silence.
        s.delay = static_cast<double>(rise) * kSubFrameSeconds;
        if (s.delay < 0.01)
        {
            s.delay = 0.0;
            rise = 0;
        }
        s.attack = static_cast<double>(at_peak - rise) * kSubFrameSeconds;
    }

    // The hold: while the level stays within 0.1 dB of the peak.
    std::size_t decay_start = at_peak;
    while (decay_start + 1 < a.size() && a[decay_start + 1] >= peak * 0.9886f)
    {
        decay_start++;
    }
    s.hold = static_cast<double>(decay_start - at_peak) * kSubFrameSeconds;

    // The decay and sustain: down to the level the curve holds at, or ends at.
    s.sustain_db = Db(a.back()) - s.peak_db;
    s.decay_db_per_second = FitFall(a, decay_start, s.peak_db, Db(a.back()));

    // The release. A release that starts in silence, or doesn't fall, ends at once.
    if (curve.release.size() > 1 && curve.release.front() > 1e-10f)
    {
        const double start = Db(curve.release.front());
        const double fitted = FitFall(curve.release, 0, start, -200.0);
        s.release_db_per_second = fitted > 0.0 ? fitted : 1e9;
    }

    return s;
}

} // namespace

Sf2Envelope ConvertEnvelope(const EnvelopeCurve& curve)
{
    // SoundFont's decay and release times are those of a 100 dB fall.
    const Shape s = FitShape(curve);
    Sf2Envelope e;
    e.peak_db = s.peak_db;
    e.delay = ToTimecents(s.delay * 1000.0, -12000, 5000);
    e.attack = ToTimecents(s.attack * 1000.0, -12000, 8000);
    e.hold = ToTimecents(s.hold * 1000.0, -12000, 5000);
    if (s.decay_db_per_second > 0.0)
    {
        e.decay = ToTimecents(100.0 / s.decay_db_per_second * 1000.0, -12000, 8000);
    }
    e.sustain = std::clamp(static_cast<int>(std::lround(-10.0 * s.sustain_db)), 0, 1440);
    e.release = ToTimecents(100.0 / s.release_db_per_second * 1000.0, -12000, 8000);

    return e;
}

SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeCurve& curve)
{
    // sfizz falls by a factor of e^-9, 9 * 20 / ln(10) dB, over a decay or release time.
    const double kFallDb = 9.0 * 20.0 / std::log(10.0);
    const Shape s = FitShape(curve);
    SfzEnvelope e;
    e.peak_db = s.peak_db;
    e.delay = s.delay;
    e.attack = s.attack;
    e.hold = s.hold;
    e.decay = s.decay_db_per_second > 0.0 ? kFallDb / s.decay_db_per_second : 0.0;
    e.sustain = std::clamp(100.0 * std::pow(10.0, s.sustain_db / 20.0), 0.0, 100.0);
    e.release = kFallDb / s.release_db_per_second;

    return e;
}

} // namespace dolphinsf2
