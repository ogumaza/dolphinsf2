// SPDX-License-Identifier: MIT

// Unit tests of the fit of fades in lines in amplitude to the envelopes of SoundFont and SFZ.

#include "fade_fit.h"

#include <cmath>

#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/units.h"
#include "xsflib/testing/check.h"

#include "test_helpers.h"
#include "unit_tests.h"

namespace dolphinsf2::testing
{
namespace
{

using xsflib::testing::Check;

// A curve that stays at its level has no decay, and a rise is an attack.
void TestFitFadesHolds()
{
    const FadeShape flat = FitFades({{0.0, 0.5}});
    Check(flat.peak == 0.5 && flat.attack_ms == 0.0 && flat.decay_db_per_ms == 0.0 && flat.sustain_db == 0.0,
          "a flat curve only holds");

    const FadeShape rise = FitFades({{0.0, 0.0}, {500.0, 1.0}});
    Check(rise.peak == 1.0 && rise.attack_ms == 500.0 && rise.decay_db_per_ms == 0.0, "a rise is the attack");

    const FadeShape silent = FitFades({{0.0, 0.0}});
    Check(silent.peak == 0.0, "a silent curve has no peak");
}

// A line in amplitude to silence starts slowly in decibels. The fit holds, then falls. It follows the line to within a
// decibel at half the line's time, where the line is 6 dB down, and within 5 dB at 900 ms, where the line is 20 dB
// down.
void TestFitFadesLine()
{
    const FadeShape line = FitFades({{0.0, 1.0}, {1000.0, 0.0}});
    Check(line.peak == 1.0 && line.attack_ms == 0.0, "the line falls from its peak");
    Check(line.hold_ms > 0.0, "the fit holds first");

    const auto fitted_db = [&line](double ms)
    {
        return (ms - line.hold_ms) * line.decay_db_per_ms;
    };

    Check(std::abs(fitted_db(500.0) - 6.02) < 1.0, "the fit follows the line's middle");
    Check(std::abs(fitted_db(900.0) - 20.0) < 5.0, "the fit follows the line down to 20 dB");
    Check(line.sustain_db < -100.0, "the line ends in silence");

    Check(FadeAmplitude({{0.0, 1.0}, {1000.0, 0.0}}, 250.0) == 0.75, "the curve's level between its points");
    Check(FadeAmplitude({{0.0, 1.0}, {1000.0, 0.0}}, 2000.0) == 0.0, "the curve holds its last level");
}

// A release in a line in amplitude takes the rate that falls 14 dB when the line does.
void TestFitRelease()
{
    const double kFall = 1.0 - std::pow(10.0, -14.0 / 20.0);
    Check(std::abs(FitRelease(1000.0) - 14.0 / (kFall * 1000.0)) < 1e-12, "the rate of a release");
    Check(FitRelease(0.0) > 1000.0, "a release of no time cuts the note");
}

// A decay slower than SoundFont's slowest (100 dB in 101.6 s) falls at the slowest rate after the hold that comes
// closest to the curve. A curve that falls 1 dB in 143 s holds for the longest that holdVolEnv allows. One that drops
// 2 dB first and then falls 1 dB in 143 s doesn't hold. A faster decay keeps the fitted hold.
void TestSlowDecayHold()
{
    using seqlib::sf2::ToTimecents;
    const double kOneDb = std::pow(10.0, -1.0 / 20.0);
    const double kTwoDb = std::pow(10.0, -2.0 / 20.0);
    const FadeShape slow = FitFades({{0.0, 1.0}, {143000.0, kOneDb}});
    const FadeShape drop = FitFades({{0.0, 1.0}, {50.0, kTwoDb}, {143000.0, kTwoDb * kOneDb}});
    const FadeShape fast = FitFades({{0.0, 1.0}, {1000.0, 0.0}});
    seqlib::sf2::ZoneValues slow_zone;
    seqlib::sf2::ZoneValues drop_zone;
    seqlib::sf2::ZoneValues fast_zone;

    SetEnvelope(slow, slow_zone);
    SetEnvelope(drop, drop_zone);
    SetEnvelope(fast, fast_zone);

    Check(slow_zone.hold == 5000 && slow_zone.decay == 8000 && slow_zone.sustain == 10,
          "a slow fall holds for the longest, then falls at the slowest rate");
    Check(drop.soundfont_hold_ms == 0.0 && drop_zone.hold == -12000 && drop_zone.decay == 8000,
          "a quick drop before a slow fall doesn't hold");
    Check(!fast.soundfont_hold_ms && fast_zone.hold == ToTimecents(fast.hold_ms, -12000, 5000),
          "a faster decay keeps the fitted hold");
}

} // namespace

void RunFadeFitTests()
{
    TestFitFadesHolds();
    TestFitFadesLine();
    TestSlowDecayHold();
    TestFitRelease();
}

} // namespace dolphinsf2::testing
