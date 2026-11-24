// SPDX-License-Identifier: MIT

// Unit tests of the SoundMacros that a voice follows: DLS times, pans, volumes, loops, fades and variables.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "seqlib/sf2/units.h"
#include "xsflib/testing/check.h"

#include "musyx_test_helpers.h"
#include "musyx_voices.h"
#include "output_units.h"
#include "test_helpers.h"
#include "unit_tests.h"

namespace dolphinsf2::testing
{
namespace
{

using xsflib::testing::Check;

// MusyX turns a DLS envelope's timecents into whole milliseconds, and keeps their low 16 bits. The table's lowest
// value stands for no time.
void TestDlsMilliseconds()
{
    Check(musyx::DlsMilliseconds(0.0) == 1000.0 && musyx::DlsMilliseconds(1200.0) == 2000.0 &&
              musyx::DlsMilliseconds(-1200.0) == 500.0,
          "1200 timecents double the time");
    Check(musyx::DlsMilliseconds(100.0) == 1059.0, "the time is truncated to whole milliseconds");
    Check(musyx::DlsMilliseconds(-1.0e9) == 0.0, "the lowest value is no time");
    Check(musyx::DlsMilliseconds(7200.0) == 64000.0 && musyx::DlsMilliseconds(8400.0) == 128000.0 - 65536.0,
          "a time keeps its low 16 bits");
    Check(musyx::DlsMilliseconds(38400.0) == 65535.0, "a time past 31 bits saturates before it's cut to 16 bits");
}

// PianoPan pans a voice by its key: the centre pan at the centre key, and the scale in 128ths of a step for each key
// above it. The pan takes the key that the macro has set up when it reaches the step. The pan stays within 0 to 127.
void TestPianoPan()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kAddKey, {12}), Step(MacroOp::kPianoPan, {100, 60, 54}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kPianoPan, {0x80, 60, 64}), Step(MacroOp::kSetKey, {72}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kSetKey, {72}), Step(MacroOp::kPianoPan, {100, 60, 54}), Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kEnd)};

    const musyx::MacroVoice shifted = musyx::FollowMacro(pool, 1, 127);
    if (!Check(shifted.piano_pan && shifted.piano_pan->scale == 100 && shifted.piano_pan->centre_key == 60 &&
                   shifted.piano_pan->centre_pan == 54,
               "PianoPan takes its scale, centre key and centre pan from bytes 1 to 3"))
    {
        return;
    }

    Check(musyx::PianoPanAt(*shifted.piano_pan, 60) == 63.375 && musyx::PianoPanAt(*shifted.piano_pan, 0) == 16.5,
          "an AddKey before the step moves the key that the pan follows");

    const musyx::MacroVoice wide = musyx::FollowMacro(pool, 2, 127);
    Check(wide.piano_pan && !wide.piano_pan->fixed_key, "a SetKey after the step leaves the key of the pan alone");
    Check(wide.piano_pan && musyx::PianoPanAt(*wide.piano_pan, 0) == 124.0,
          "a scale of -128 moves the pan one step left for each key");
    Check(wide.piano_pan && musyx::PianoPanAt(*wide.piano_pan, 127) == 0.0, "the pan stops at 0");

    const musyx::MacroVoice fixed = musyx::FollowMacro(pool, 3, 127);
    Check(fixed.piano_pan && musyx::PianoPanAt(*fixed.piano_pan, 0) == 63.375,
          "a SetKey before the step fixes the key that the pan follows");
    Check(!musyx::FollowMacro(pool, 4, 127).piano_pan, "a macro without the step keeps the pan of its keymap or layer");
}

// ScaleVolume multiplies the note's volume, or the voice's, by its scale over 127. The early MusyX on the NDDEMO disc
// divides by 128. The step then adds byte 2 in 127ths of full volume. MusyX plays a volume above full at full volume.
// Bytes 3 and 4 name a curve. 0xffff names none, and MusyX skips a curve that the pool doesn't have.
void TestScaleVolume()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kScaleVolume, {90, 10, 0xff, 0xff, 1}), Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kScaleVolume, {64, 20, 0xff, 0xff, 1}),
                      Step(MacroOp::kScaleVolume, {64, 0, 0xff, 0xff, 0}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kScaleVolume, {127, 50, 0xff, 0xff, 1}), Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kScaleVolume, {127, 0, 5, 0, 1}), Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kScaleVolume, {127, 0, 6, 0, 1}), Step(MacroOp::kEnd)};
    pool.tables[5] = {0, 0x7f};

    const musyx::MacroVoice later = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice early = musyx::FollowMacro(pool, 1, 128);
    const musyx::MacroVoice twice = musyx::FollowMacro(pool, 2, 128);
    const musyx::MacroVoice loud = musyx::FollowMacro(pool, 3, 128);

    Check(later.volume == 90.0 / 127.0 && later.volume_add == 10.0 / 127.0, "the scale divides by 127");
    Check(early.volume == 90.0 / 128.0 && early.volume_add == 10.0 / 127.0, "the early build's scale divides by 128");
    Check(std::abs(musyx::VoiceVolume(early, 0.5, 1.0) - (0.5 * 90.0 / 128.0 + 10.0 / 127.0)) < 1e-12,
          "the voice's volume is the scaled note's volume plus the addition");
    Check(twice.volume == 0.25 && std::abs(twice.volume_add - 10.0 / 127.0) < 1e-12,
          "a step on the voice's volume scales the addition before it");
    Check(musyx::VoiceVolume(loud, 1.0, 1.0) == 1.0 && musyx::VoiceVolume(loud, 0.25, 1.0) < 1.0,
          "a volume above full plays at full volume");
    Check(later.approximations.empty() && musyx::FollowMacro(pool, 5, 127).approximations.empty(),
          "a step that names no curve, or a curve missing from the pool, reports nothing");
    const std::vector<std::string> curved = musyx::FollowMacro(pool, 4, 127).approximations;
    Check(curved == std::vector<std::string>{"volume curves left out"}, "a curve in the pool is reported");
}

// A WaitMs step of `ms` milliseconds.
ntdlib::musyx::MacroStep WaitMsStep(uint32_t ms)
{
    return Step(ntdlib::musyx::MacroOp::kWaitMs, {0, 0, 0, 0, 0, ms & 0xff, ms >> 8});
}

// A Loop step back to step `step`, `count` times, with the flags of byte 1.
ntdlib::musyx::MacroStep LoopStep(uint32_t flags, uint32_t step, uint32_t count)
{
    return Step(ntdlib::musyx::MacroOp::kLoop, {flags, 0, 0, step & 0xff, step >> 8, count & 0xff, count >> 8});
}

// Loop goes back to step `step` `count` times. The count starts again once it runs out. A count of 0xffff with bit 0
// of byte 1 goes round until the release. A loop that doesn't wait then waits for the release, and a trap of the
// release takes the voice on from there. MusyX follows 32 steps of a voice in each millisecond.
void TestMacroLoop()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kStartSample, {1}), WaitMsStep(100), LoopStep(0, 1, 2), Step(MacroOp::kStopSample),
                      Step(MacroOp::kEnd)};
    pool.macros[2] = {
        Step(MacroOp::kStartSample, {1}), WaitMsStep(10),     LoopStep(0, 1, 1), WaitMsStep(20), LoopStep(0, 3, 1),
        Step(MacroOp::kStopSample),       Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kStartSample, {1}), LoopStep(0, 1, 70), Step(MacroOp::kStopSample),
                      Step(MacroOp::kEnd)};
    pool.macros[4] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kScaleVolume, {127, 0, 0xff, 0xff, 1}),
                      LoopStep(1, 1, 0xffff), Step(MacroOp::kStopSample), Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kTrapEvent, {0, 5, 0, 4, 0}), Step(MacroOp::kStartSample, {1}),
                      LoopStep(0, 2, 0xffff), Step(MacroOp::kEnd)};
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    pool.macros[5].insert(pool.macros[5].end(), kFade.begin() + 1, kFade.end());

    const musyx::MacroVoice counted = musyx::FollowMacro(pool, 1, 127);
    Check(counted.stop_ms == 300.0 && counted.approximations.empty(),
          "a count of 2 goes back twice, and the loop reports nothing");
    Check(musyx::FollowMacro(pool, 2, 127).stop_ms == 60.0, "a second loop starts the count again");
    Check(musyx::FollowMacro(pool, 3, 127).stop_ms == 2.0,
          "73 steps without a wait take two milliseconds: the stop comes in the third run");

    const musyx::MacroVoice held = musyx::FollowMacro(pool, 4, 127);
    Check(!held.stop_ms && held.key_off == musyx::KeyOff::kCut && held.approximations.empty(),
          "a loop until the release that doesn't wait holds the voice, and the stop after it cuts the voice there");
    const musyx::MacroVoice trapped = musyx::FollowMacro(pool, 5, 127);
    Check(!trapped.stop_ms && trapped.key_off == musyx::KeyOff::kFade && trapped.release_fade.stop_ms == 920.0,
          "a trap of the release takes a loop that doesn't wait on to the fade");
}

// FadeIn at the voice's start becomes the attack, and the volume takes the fade's end: byte 1 scales it in 128ths and
// byte 2 adds 127ths of full volume. The time is in milliseconds with bit 0 of byte 5, else in ticks. A FadeIn after a
// wait ramps from the voice's volume, and the report says so. A table of the pool named in bytes 3 and 4 maps the
// fade's end as a curve.
void TestFadeIn()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kFadeIn, {64, 10, 0xff, 0xff, 1, 0x20, 0x03}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    pool.macros[2] = {Step(MacroOp::kStartSample, {1}), WaitMsStep(100),
                      Step(MacroOp::kFadeIn, {127, 0, 0xff, 0xff, 1, 50}), Step(MacroOp::kEnd)};
    pool.macros[3] = {Step(MacroOp::kFadeIn, {127, 0, 0xff, 0xff, 0, 10}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};

    const musyx::MacroVoice faded = musyx::FollowMacro(pool, 1, 127);
    Check(faded.fade_in_ms == 800.0 && faded.volume == 0.5 && std::abs(faded.volume_add - 10.0 / 127.0) < 1e-12 &&
              faded.held_ramps.empty() && faded.approximations.empty(),
          "a fade at the start rises for 800 ms to half the volume plus 10/127, and reports nothing");
    const musyx::MacroVoice later = musyx::FollowMacro(pool, 2, 127);
    Check(later.fade_in_ms == 0.0 && later.held_ramps.size() == 1 && later.held_ramps[0].start_ms == 100.0 &&
              later.held_ramps[0].ms == 50.0 && later.held_ramps[0].scale == 127.0 / 128.0,
          "a later fade ramps from the volume that the voice has");
    Check(later.approximations == std::vector<std::string>{"fade-ins after a voice's start ramp from its volume"},
          "a later fade is reported");
    Check(std::abs(musyx::FollowMacro(pool, 3, 127).fade_in_ms - 10.0 * 1000.0 / 768.0) < 1e-9,
          "a time in ticks takes a tempo of 120");

    // Pokémon Colosseum's snd_se-0902 fades in over 2 s to 2/128 of its volume through table 0x20, an envelope. At full
    // volume the fade ends at 1.984 of 127. The curve takes byte 1 (3) and 0.984 of the way to byte 2 (232).
    pool.tables[0x20] = {0xe8, 0x03, 0xe8, 0x03, 0x00, 0x10, 0xe8, 0x03};
    pool.tables[0x21] = {0x00, 0x40};
    pool.macros[4] = {Step(MacroOp::kFadeIn, {2, 0, 0x20, 0x00, 1, 0xd0, 0x07}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    pool.macros[5] = {Step(MacroOp::kFadeIn, {64, 0, 0x21, 0x00, 1, 10}), Step(MacroOp::kStartSample, {1}),
                      Step(MacroOp::kEnd)};
    const musyx::MacroVoice curved = musyx::FollowMacro(pool, 4, 127);
    Check(curved.fade_in_ms == 2000.0 && std::abs(curved.volume - (3.0 + 0.984375 * 229.0) / 127.0) < 1e-12 &&
              curved.volume_add == 0.0 && curved.approximations.empty(),
          "a curve that the pool holds maps the fade's end at full volume, and reports nothing");
    const musyx::MacroVoice short_curve = musyx::FollowMacro(pool, 5, 127);
    Check(short_curve.volume == 0.5 && short_curve.approximations == std::vector<std::string>{"volume curves left out"},
          "a curve that ends before the bytes that MusyX reads is left out and reported");
}

// SetVariable, AddImmediate and the other arithmetic steps work on 16 signed bits, and IfEqual and IfLess go on at
// step `Half(6)` when they hold. Byte 5 turns a comparison round. Variables 16 to 31 are MusyX's globals. Each run
// starts them at 0. The game's start leaves them there. A comparison with a controller goes on with the next step and
// is reported. Each macro ends at a ScaleVolume of 100 when it goes on, or of 64 when it jumps.
void TestMacroVariables()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;

    const auto ending = [](std::vector<ntdlib::musyx::MacroStep> steps)
    {
        steps.push_back(Step(MacroOp::kScaleVolume, {100, 0, 0xff, 0xff, 1}));
        steps.push_back(Step(MacroOp::kEnd));
        steps.push_back(Step(MacroOp::kScaleVolume, {64, 0, 0xff, 0xff, 1}));
        steps.push_back(Step(MacroOp::kEnd));

        return steps;
    };

    // The first play of Mario Party 4's se-0768: global 16 counts the plays, and global 17 is 1.
    pool.macros[1] =
        ending({Step(MacroOp::kSetVariable, {0, 0x11, 0, 1, 0}), Step(MacroOp::kAddImmediate, {0, 0x10, 0, 0x10, 1, 0}),
                Step(MacroOp::kIfEqual, {0, 0x10, 0, 0x11, 0, 5, 0})});
    pool.macros[2] =
        ending({Step(MacroOp::kSetVariable, {0, 0x11, 0, 1, 0}), Step(MacroOp::kAddImmediate, {0, 0x10, 0, 0x10, 1, 0}),
                Step(MacroOp::kIfEqual, {0, 0x10, 0, 0x11, 1, 5, 0})});
    pool.macros[3] =
        ending({Step(MacroOp::kSetVariable, {0, 0, 0, 0xfb, 0xff}), Step(MacroOp::kIfLess, {0, 0, 0, 1, 0, 4, 0})});
    pool.macros[4] =
        ending({Step(MacroOp::kSetVariable, {0, 0, 0, 0xff, 0x7f}), Step(MacroOp::kAddImmediate, {0, 0, 0, 0, 1, 0}),
                Step(MacroOp::kDivideVariables, {0, 2, 0, 0, 0, 3}), Step(MacroOp::kIfEqual, {0, 2, 0, 3, 1, 6, 0}),
                Step(MacroOp::kSetVariable, {0, 1, 0, 0xff, 0x7f}), Step(MacroOp::kIfEqual, {0, 0, 0, 1, 0, 8, 0})});
    pool.macros[5] = ending({Step(MacroOp::kIfEqual, {1, 7, 0, 0, 0, 3, 0})});

    const musyx::MacroVoice first = musyx::FollowMacro(pool, 1, 127);
    Check(first.volume == 64.0 / 127.0 && first.approximations.empty(),
          "a global starts at 0: the first play adds 1, equals 1 and jumps, and reports nothing");
    Check(musyx::FollowMacro(pool, 1, 127).volume == 64.0 / 127.0, "each run starts the globals again");
    Check(musyx::FollowMacro(pool, 2, 127).volume == 100.0 / 127.0, "byte 5 turns the comparison round");
    Check(musyx::FollowMacro(pool, 3, 127).volume == 64.0 / 127.0, "IfLess compares 16 signed bits: -5 < 0");
    Check(musyx::FollowMacro(pool, 4, 127).volume == 64.0 / 127.0,
          "a sum past 0x7fff stays there, and a division by 0 gives 0");
    const musyx::MacroVoice controller = musyx::FollowMacro(pool, 5, 127);
    Check(controller.volume == 100.0 / 127.0 &&
              controller.approximations == std::vector<std::string>{musyx::kCommandsLeftOut},
          "a comparison with a controller goes on and is reported");
}

// A voice that plays on after its note's release follows its macro's fade (KeyOff::kFade). Each Envelope step ramps
// the volume in a line, from where it is to its scale in 128ths times that. The fade ends at the StopSample, 920 ms
// after the release. A voice that stops by itself at the same time whether the note is released or not ignores the
// release (KeyOff::kAsHeld). Its fade ramps while the note is held, from 100 ms after the note's start, and leaves it
// silent at the stop.
void TestReleaseFade()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;
    pool.macros[1] = {Step(MacroOp::kStartSample, {1})};
    const std::vector<ntdlib::musyx::MacroStep> kFade = FadeSteps();
    pool.macros[1].insert(pool.macros[1].end(), kFade.begin(), kFade.end());
    pool.macros[2] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 100, 0})};
    pool.macros[2].insert(pool.macros[2].end(), kFade.begin() + 1, kFade.end());

    const musyx::MacroVoice faded = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice own = musyx::FollowMacro(pool, 2, 127);

    const musyx::ReleaseFade& f = faded.release_fade;
    Check(faded.key_off == musyx::KeyOff::kFade && f.stop_ms == 920.0 && f.ramps.size() == 2,
          "the voice fades for 920 ms after its release, in two ramps");
    Check(f.ramps.size() == 2 && f.ramps[0].start_ms == 0.0 && f.ramps[0].ms == 120.0 && f.ramps[0].scale == 0.625 &&
              f.ramps[1].start_ms == 120.0 && f.ramps[1].ms == 800.0 && f.ramps[1].scale == 0.0,
          "the ramps start at the release and 120 ms later");
    Check(musyx::FadeVolume(f, 1.0, 60.0) == 0.8125 && musyx::FadeVolume(f, 1.0, 120.0) == 0.625 &&
              musyx::FadeVolume(f, 1.0, 520.0) == 0.3125 && !musyx::FadeVolume(f, 1.0, 920.0),
          "each ramp moves the volume in a line from where it is, and the voice stops at the end");
    Check(faded.approximations.empty() && faded.held_ramps.empty(), "a fade after the release reports nothing");
    Check(own.key_off == musyx::KeyOff::kAsHeld && own.stop_ms == 1020.0,
          "a voice that stops by itself ignores the release");
    Check(own.held_ramps.size() == 2 && own.held_ramps[0].start_ms == 100.0 && own.held_ramps[1].start_ms == 220.0,
          "its fade ramps while the note is held");
    Check(musyx::HeldSilenceMs(own) == 1020.0 && musyx::FadesThroughRelease(own) && own.approximations.empty(),
          "it falls silent at its stop, goes on with its fade through the release and reports nothing");
}

// A held note's fade goes on through the release of a voice that ignores the release: macro 1 waits for the release
// after its fade, then ends. A fade to silence leaves the voice silent at its end (HeldSilenceMs). A fade to 64/128 of
// the volume leaves it sounding. Ramps after the voice stops change nothing.
void TestHeldFade()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;

    const auto fade_to = [](uint32_t scale)
    {
        return std::vector<ntdlib::musyx::MacroStep>{
            Step(MacroOp::kStartSample, {1}),
            Step(MacroOp::kEnvelope, {scale, 0, 0xff, 0xff, 1, 0xe8, 0x03}),
            Step(MacroOp::kWaitTicks, {1, 0, 0, 0, 1, 0xff, 0xff}),
            Step(MacroOp::kEnd),
        };
    };

    pool.macros[1] = fade_to(0);
    pool.macros[2] = fade_to(64);
    pool.macros[3] = {Step(MacroOp::kStartSample, {1}), Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0xf4, 0x01}),
                      Step(MacroOp::kStopSample), Step(MacroOp::kEnvelope, {0, 0, 0xff, 0xff, 1, 0xe8, 0x03}),
                      Step(MacroOp::kEnd)};

    const musyx::MacroVoice silent = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice half = musyx::FollowMacro(pool, 2, 127);
    const musyx::MacroVoice stopped = musyx::FollowMacro(pool, 3, 127);

    Check(silent.key_off == musyx::KeyOff::kIgnored && silent.held_ramps.size() == 1 &&
              silent.held_ramps[0].start_ms == 0.0 && silent.held_ramps[0].ms == 1000.0,
          "the voice ignores the release and fades over 1000 ms from the note's start");
    Check(musyx::HeldSilenceMs(silent) == 1000.0 && musyx::FadesThroughRelease(silent),
          "a fade to silence ends at 1000 ms and goes on through the release");
    Check(musyx::RampedVolume(silent.held_ramps, 1.0, 250.0) == 0.75 &&
              musyx::RampedVolume(silent.held_ramps, 1.0, 2000.0) == 0.0,
          "the volume falls in a line");
    Check(!musyx::HeldSilenceMs(half) && !musyx::FadesThroughRelease(half),
          "a fade to 64/128 leaves the voice sounding");
    Check(silent.approximations.empty() && half.approximations.empty(), "the fades report nothing");
    Check(stopped.held_ramps.empty() && stopped.stop_ms == 500.0, "a ramp after the stop is left out");
}

// A held note whose linear envelope decays to silence ends where the decay does. Macros 1 and 2 set linear envelopes
// with an attack of 20 ms, a decay of 300 ms and a sustain of 0, start sample 1 and release the envelope at the note's
// release. Envelope 3 releases over 100 ms. Envelope 4 releases over 1,800 ms, more slowly than its decay falls. Its
// note ends where the files' decay has fallen silent.
void TestDecayToSilence()
{
    using ntdlib::musyx::MacroOp;
    ntdlib::musyx::Pool pool;

    const auto releasing = [](uint32_t table)
    {
        return std::vector<ntdlib::musyx::MacroStep>{
            Step(MacroOp::kSetAdsr, {table, 0, 0}),
            Step(MacroOp::kStartSample, {1}),
            Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
            Step(MacroOp::kKeyOff),
            Step(MacroOp::kEnd),
        };
    };

    pool.macros[1] = releasing(3);
    pool.macros[2] = releasing(4);
    pool.tables[3] = {20, 0, 0x2c, 0x01, 0, 0, 100, 0};
    pool.tables[4] = {20, 0, 0x2c, 0x01, 0, 0, 0x08, 0x07};

    const musyx::MacroVoice fast = musyx::FollowMacro(pool, 1, 127);
    const musyx::MacroVoice slow = musyx::FollowMacro(pool, 2, 127);

    Check(fast.key_off == musyx::KeyOff::kRelease && slow.key_off == musyx::KeyOff::kRelease,
          "both voices release their envelopes");
    Check(musyx::DecayedMs(fast) == 320.0 && musyx::HeldSilenceMs(fast) == 320.0,
          "a release faster than the decay: the note ends with the decay, after the attack");
    const double kSilent = 20.0 + 300.0 * seqlib::sf2::kEnvelopeFallDb / musyx::kEnergyFitDb;
    Check(musyx::DecayedMs(slow) == kSilent && musyx::HeldSilenceMs(slow) == kSilent,
          "a release slower than the decay: the note ends where the files' decay has fallen silent, at " +
              std::to_string(musyx::DecayedMs(slow).value_or(0.0)) + " ms");
}

} // namespace

void RunMusyxMacroTests()
{
    TestDlsMilliseconds();
    TestPianoPan();
    TestScaleVolume();
    TestMacroLoop();
    TestFadeIn();
    TestMacroVariables();
    TestReleaseFade();
    TestHeldFade();
    TestDecayToSilence();
}

} // namespace dolphinsf2::testing
