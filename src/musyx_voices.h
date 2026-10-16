// SPDX-License-Identifier: MIT

// What MusyX plays for a note: the SoundMacros that a program's keymaps and layers start, and what each macro does
// with its note, as far as a SoundFont can play it. A SoundMacro is a small program that a voice runs. dolphinsf2
// follows each macro's steps for a note that the player holds, and for a note that the player releases, and takes the
// sample, the volume, the envelopes, the vibrato and the tremolo that the steps set up.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/samples.h"

#include "jaudio.h"
#include "musyx_game.h"

namespace dolphinsf2::musyx
{

// The rate the GameCube mixes at, 108 MHz / 3372. MusyX takes it for 32000 Hz. Its songs therefore run faster than
// their tempo, and its voices play sharp, by the ratio of the two.
constexpr double kOutputRate = 108.0e6 / 3372.0;
constexpr double kNominalRate = 32000.0;

// An envelope of a voice's volume or pitch, in MusyX's units. The attack rises linearly in amplitude. In the DLS form,
// the decay and the release fall linearly in decibels: 96.5 dB in their time. In the linear form, they fall linearly
// in amplitude: the decay from full level to the sustain in its time, the release from where it starts to silence in
// its time. An envelope whose sustain is silent ends its voice after the decay.
struct Envelope
{
    bool dls = false;
    double attack_ms = 0.0;
    double decay_ms = 0.0;
    double sustain = 1.0; // amplitude
    double release_ms = 0.0;

    // A DLS envelope's attack and decay in timecents, and its scales. MusyX adds `attack_tc_per_velocity` times the
    // note's velocity to the attack, and `decay_tc_per_key` times the voice's key to the decay. It then takes the times
    // in whole milliseconds (DlsMilliseconds). The times above are those without the scales.
    double attack_timecents = 0.0;
    double decay_timecents = 0.0;
    double attack_tc_per_velocity = 0.0;
    double decay_tc_per_key = 0.0;
};

// The milliseconds of `timecents`. MusyX turns a DLS envelope's time into whole milliseconds this way.
double DlsMilliseconds(double timecents);

// The decibels that a DLS envelope's decay and release fall in their time: 193 steps of 0.5 dB.
constexpr double kDlsFallDb = 96.5;

// What a voice does when the player releases its note.
enum class KeyOff
{
    kIgnored, // it plays on: to the sample's end, or as long as the sample loops
    kRelease, // its envelope's release
    kCut,     // a fade of 10 ms (StopSample)
};

// The fade that ends a stopped voice.
constexpr double kCutMs = 10.0;

// A vibrato: a sine on the pitch from `delay_ms` after the note's start. Its depth is `depth_cents`, plus
// `mod_depth_cents` times the mod wheel. With `mod_scales_depth`, the mod wheel scales the whole depth.
struct Vibrato
{
    double delay_ms = 0.0;
    double period_ms = 0.0;
    double depth_cents = 0.0;
    double mod_depth_cents = 0.0;
    bool mod_scales_depth = false;
};

// A tremolo: a sine on the volume. The volume drops by up to `depth` of full level, or `full_mod_depth` with the mod
// wheel at its top. The depth moves in a line between the two with the mod wheel.
struct Tremolo
{
    double period_ms = 0.0;
    double depth = 0.0;
    double full_mod_depth = 0.0;
};

// An envelope on the pitch: `range_cents` at its peak.
struct PitchEnvelope
{
    Envelope envelope;
    double range_cents = 0.0;
};

// What a SoundMacro does with a note.
struct MacroVoice
{
    std::optional<uint16_t> sample; // the sample it starts, if any
    int key_shift = 0;              // keys added to the note's key (AddKey)
    std::optional<int> fixed_key;   // a key that replaces the note's key (SetKey)
    double detune_cents = 0.0;
    double volume = 1.0;     // the scale of the note's volume (ScaleVolumeDLS, ScaleVolume)
    bool dls_volume = false; // the voice's level is the square of its volume (ModeSelect)
    bool itd = false;        // the pan narrows to 0.766 of its range (ModeSelect)
    std::optional<Envelope> envelope;
    KeyOff key_off = KeyOff::kIgnored;
    std::optional<double> stop_ms; // the voice stops itself this long after its start, unless released before
    std::optional<Vibrato> vibrato;
    std::optional<Tremolo> tremolo;
    std::optional<PitchEnvelope> pitch_envelope;
    uint8_t key_group = 0; // the voice stops the group's other voices when it starts

    // What the macro does that a SoundFont doesn't play, for the conversion's report.
    std::vector<std::string> approximations;
};

// Follow SoundMacro `macro` of `pool` for a note. A macro that the pool lacks starts nothing.
MacroVoice FollowMacro(const ntdlib::musyx::Pool& pool, uint16_t macro);

// A voice that a key of a program starts: a macro, the key that it starts with, its pan (0 to 127, 64 the centre,
// before the channel's pan moves it) and the share of the note's volume that the layers give it.
struct KeyVoice
{
    uint16_t macro = 0;
    int key = 60;
    int pan = 64;
    double volume = 1.0;
};

// The voices that `key` starts on object `object` of `pool`: a macro, a keymap or a layer.
std::vector<KeyVoice> VoicesOf(const ntdlib::musyx::Pool& pool, uint16_t object, int key);

// A decoded sample, with its key and rate.
struct Sample
{
    Pcm pcm;
    uint8_t key = 60; // the key that plays it at its rate
    uint32_t id = 0;
};

// The song group's macros as voices, and its samples decoded on demand. Read failures are collected in Errors() and
// silence only the voices they affect.
class Instruments
{
public:
    // Read `group`. `group` must outlive the instruments.
    explicit Instruments(const SongGroup& group);

    const SongGroup& Group() const
    {
        return group_;
    }

    // Macro `macro` as a voice, followed once.
    const MacroVoice& Macro(uint16_t macro);

    // Sample `id`, decoded once. Return nullptr if the sample directory lacks it, or record why if it can't be
    // decoded.
    const Sample* GetSample(uint16_t id);

    // The seconds that sample `id` plays when its pitch is `ratio` times its rate, or nothing if it loops. Return 0 for
    // a sample that can't be decoded.
    std::optional<double> SampleSeconds(uint16_t id, double ratio);

    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    const SongGroup& group_;
    std::map<uint16_t, MacroVoice> macros_;
    std::map<uint16_t, std::unique_ptr<Sample>> samples_;
    std::vector<std::string> errors_;
};

} // namespace dolphinsf2::musyx
