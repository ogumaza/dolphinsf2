// SPDX-License-Identifier: MIT

// What MusyX plays for a note: the SoundMacros that a program's keymaps and layers start, and what each macro does
// with its note, as far as a SoundFont can play it. A SoundMacro is a small program that a voice runs. dolphinsf2
// follows each macro's steps for a note that the player holds, and for a note that the player releases, and takes the
// sample, the volume, the envelopes, the vibrato and the tremolo that the steps set up.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "ntdlib/musyx/pool.h"

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

// The sustain under which a linear envelope's decay counts as a fall to silence: 40 dB down. MusyX ends a voice whose
// decay reaches a sustain of 0. A voice held further down plays far under the music, and its note ends there too.
constexpr double kSilentSustain = 0.01;

// The files fit a linear fall to silence by its energy: a fall of kEnergyFitDb in the game's time. The game's straight
// line from full level falls as 1 - t/T, with an energy of T/3. A fall of r dB in T has an energy of 10 T / (r ln 10).
// The two match at r = 30 / ln 10, about 13 dB.
constexpr double kEnergyFitDb = 30.0 / std::numbers::ln10;

// The decibels that a SoundFont envelope's decay and release fall in their time. A voice of the files counts as silent
// after this fall.
constexpr double kSoundFontFallDb = 100.0;

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
    kFade,    // it plays on and follows its macro's fade (MacroVoice::release_fade), then stops (StopSample)
    kAsHeld,  // it plays on as a held note does and stops by itself (MacroVoice::stop_ms)
};

// The fade that ends a stopped voice.
constexpr double kCutMs = 10.0;

// A ramp of a voice's volume that an Envelope step starts (0x0f), `start_ms` after the note's start in a held note's
// ramps (MacroVoice::held_ramps), or after the note's release in a release's fade (ReleaseFade). The volume moves in a
// line over `ms`, from where it is to `scale` times that plus `add`. MusyX holds the target to full volume.
struct VolumeRamp
{
    double start_ms = 0.0;
    double ms = 0.0;
    double scale = 1.0;
    double add = 0.0; // in units of full volume
};

// The volume `ms` after the start of `ramps`, for a voice at `volume` that follows them in order. Each ramp starts
// where the volume is. A later ramp takes over from an earlier one.
double RampedVolume(const std::vector<VolumeRamp>& ramps, double volume, double ms);

// What a voice does after its note's release when its macro plays it on until a stop (KeyOff::kFade): the ramps of its
// volume, in order, and the stop `stop_ms` after the release.
struct ReleaseFade
{
    std::vector<VolumeRamp> ramps;
    double stop_ms = 0.0;
};

// The volume of a voice `ms` after its note's release, for a voice at `volume` that follows `fade`. Return nothing
// once the voice has stopped.
std::optional<double> FadeVolume(const ReleaseFade& fade, double volume, double ms);

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

// A pan that follows the voice's key (PianoPan). The pan is `centre_pan` at `centre_key`. It moves by `scale` 128ths of
// a step for each key above the centre key. MusyX takes the voice's key when the macro reaches the step. `key_shift`
// and `fixed_key` hold the macro's changes to the key up to that step.
struct PianoPan
{
    int scale = 0;
    int centre_key = 60;
    int centre_pan = 64;
    int key_shift = 0;
    std::optional<int> fixed_key;
};

// The pan of a voice that starts with `key` and follows `pan`: 0 to 127, 64 the centre.
double PianoPanAt(const PianoPan& pan, int key);

// A voice that a macro starts on another voice (PlayMacro): the macro and the step that the new voice starts at, and
// when, after the start of the voice that starts it. The new voice starts with that voice's key at the step, plus
// `key_add`. `key_shift` and `fixed_key` hold the starting voice's changes to its key up to the step. The new voice
// shares the starting voice's controllers: `volume_controller` holds the volume controller that the starting voice has
// set by the step (MacroVoice::volume_controller).
struct StartedVoice
{
    uint16_t macro = 0;
    uint16_t step = 0;
    int key_add = 0;
    int key_shift = 0;
    std::optional<int> fixed_key;
    double at_ms = 0.0;
    std::optional<double> volume_controller;
};

// The approximation that a voice's report gives for the SoundMacro commands that dolphinsf2 doesn't follow.
inline constexpr char kCommandsLeftOut[] = "SoundMacro commands that dolphinsf2 doesn't follow left out";

// What a SoundMacro does with a note.
struct MacroVoice
{
    std::optional<uint16_t> sample; // the sample it starts, if any
    double sample_ms = 0.0;         // when it starts the sample, after its start
    int key_shift = 0;              // keys added to the note's key (AddKey)
    std::optional<int> fixed_key;   // a key that replaces the note's key (SetKey)
    double detune_cents = 0.0;
    // A rate in Hz at which the voice plays its sample, whatever its key and detune (SetPitch). A later SetKey takes
    // its place.
    std::optional<double> rate_hz;
    double volume = 1.0;     // the scale of the note's volume (ScaleVolumeDLS, ScaleVolume)
    double volume_add = 0.0; // what ScaleVolume adds to the scaled volume, in units of full volume
    // The share of the volume controller (CC7) that the voice's volume follows (VolumeSelect). MusyX holds the
    // controller's value times the share at full volume.
    double source_volume = 1.0;
    // The volume controller from 0 to 1, if the macro sets it (SetVariable on a controller). In an effect, it takes the
    // place of the effect's volume controller at full. A song's channel holds the controller. A song's note leaves it
    // out.
    std::optional<double> volume_controller;
    // The sends to aux A and aux B (CC91 and CC93) from 0 to 1, if the macro sets them. An effect's note takes them in
    // place of the effect's sends. A song's note leaves them out.
    std::optional<double> reverb_controller;
    std::optional<double> chorus_controller;
    bool dls_volume = false; // the voice's level is the square of its volume (ModeSelect)
    bool itd = false;        // the pan narrows to 0.766 of its range (ModeSelect)
    // A pan that follows the key, in place of the pan of the keymap or the layer.
    std::optional<PianoPan> piano_pan;
    // A pan from 0 to 127 that the voice sets itself (Panning). The voice takes it in place of the pan of the keymap
    // or the layer, and of a PianoPan before it.
    std::optional<int> pan;
    bool pan_last = false;  // a Panning step comes after the last PianoPan
    bool pan_moves = false; // a Panning step moves the pan (the voice keeps the first pan)
    // The voices that the macro starts on other voices (PlayMacro), in order.
    std::vector<StartedVoice> started;
    // The parts of a voice whose macro starts one sample after another, one for each sample: from the sample's start
    // to the next one's, with the settings that the macro has made by the time the sample has played for a moment.
    // Empty for a voice that starts one sample. A song's note plays the first sample with the voice's settings, and an
    // effect plays every part.
    std::vector<MacroVoice> parts;
    std::optional<Envelope> envelope;
    // The ramps of the voice's volume while its note is held, up to the voice's stop (stop_ms).
    std::vector<VolumeRamp> held_ramps;
    KeyOff key_off = KeyOff::kIgnored;
    ReleaseFade release_fade;      // for KeyOff::kFade
    std::optional<double> stop_ms; // the voice stops itself this long after its start if its note is still held
    std::optional<Vibrato> vibrato;
    std::optional<Tremolo> tremolo;
    std::optional<PitchEnvelope> pitch_envelope;
    uint8_t key_group = 0; // the voice stops the group's other voices when it starts
    int bend_up = 2;       // the pitch wheel's range in semitones above the note (PitchWheelRange)
    int bend_down = 2;     // and below it

    // What the macro does that a SoundFont doesn't play, for the conversion's report.
    std::vector<std::string> approximations;
};

// Follow SoundMacro `macro` of `pool` for a note. A macro that the pool lacks starts nothing. ScaleVolume divides by
// `volume_scale_divisor` (SongGroup).
MacroVoice FollowMacro(const ntdlib::musyx::Pool& pool, uint16_t macro, int volume_scale_divisor);

// `voice` for a note that starts `ms` after the voice does, where the voice starts its sample: the times of the
// voice's settings count from the note's start. A voice that stops before then starts no sample.
MacroVoice VoiceFrom(const MacroVoice& voice, double ms);

// The volume of `voice` for a note at `volume`: the note's volume scaled by the voice's macro, plus the macro's
// addition, times the share of the volume controller that the voice follows. `controller` holds the controller, from 0
// to 1. MusyX plays a volume above full at full volume.
double VoiceVolume(const MacroVoice& voice, double volume, double controller);

// The milliseconds after its note's start at which a held note of `voice` ends through its envelope: where the decay of
// a linear envelope whose sustain is under kSilentSustain ends. A voice whose release outlasts its decay would ring on
// through that release in the files. The game's voice is silent there. Its note ends where the files' decay has fallen
// kSoundFontFallDb. Nothing for any other voice.
std::optional<double> DecayedMs(const MacroVoice& voice);

// The milliseconds after its note's start at which a held note of `voice` falls silent: at the end of the cut after
// stop_ms, at the end of a held ramp that leaves its volume at 0, or where its envelope's decay ends (DecayedMs).
// Return nothing if the note sounds on.
std::optional<double> HeldSilenceMs(const MacroVoice& voice);

// Whether `voice` fades to silence while its note is held and goes on with the fade through the release: a voice that
// ignores the release (KeyOff::kIgnored) or stops by itself (KeyOff::kAsHeld).
bool FadesThroughRelease(const MacroVoice& voice);

// A voice that a key of a program starts: a macro, the key that it starts with, its pan (0 to 127, 64 the centre,
// before the channel's pan moves it) and the share of the note's volume that the layers give it. The voice's note
// starts `from_ms` after the voice does (VoiceFrom). A voice of an effect can play one part of its macro's voice
// (MacroVoice::parts): `part` is then 1 plus the part's index, else 0.
struct KeyVoice
{
    uint16_t macro = 0;
    int key = 60;
    int pan = 64;
    double volume = 1.0;
    double from_ms = 0.0;
    std::size_t part = 0;
};

// The voices that `key` starts on object `object` of `pool`: a macro, a keymap or a layer.
std::vector<KeyVoice> VoicesOf(const ntdlib::musyx::Pool& pool, uint16_t object, int key);

// The amplitude that MusyX's volume table `volumes` gives `volume`. MusyX takes the volume in 127ths and interpolates
// between the table's entries. It plays a volume above 1 at full level.
double TableLevel(const VolumeTable& volumes, double volume);

// The volume from 0 to 1 to which `volumes` gives the amplitude `level`: TableLevel's inverse.
double TableVolume(const VolumeTable& volumes, double level);

// A decoded sample, with its key and rate.
struct Sample
{
    Pcm pcm;
    uint8_t key = 60; // the key that plays it at its rate
    uint32_t id = 0;
    std::string sfz_name; // its WAV file's name in the SFZ files' shared folder, without ".wav"
};

// The song group's macros as voices, and its samples decoded on demand. Read failures are collected in Errors() and
// silence only the voices they affect.
class Instruments
{
public:
    // Read `group`. The group's voices play on the game's volume table `volumes` unless they pick the DLS curve.
    // `group` must outlive the instruments. The SFZ files give a sample the name that `sfz_names` holds for its ID
    // (SfzSampleNames), or "s" and its ID, such as "s12".
    Instruments(const SongGroup& group, const VolumeTable& volumes, std::map<uint16_t, std::string> sfz_names = {});

    const SongGroup& Group() const
    {
        return group_;
    }

    const VolumeTable& Volumes() const
    {
        return volumes_;
    }

    // Macro `macro` as a voice, followed once.
    const MacroVoice& Macro(uint16_t macro);

    // Macro `macro` as a voice, or part `part` of that voice (KeyVoice::part), for a note that starts `from_ms` after
    // the voice does (VoiceFrom), made once.
    const MacroVoice& Macro(uint16_t macro, std::size_t part, double from_ms);

    // The voice that `voice` plays from its note's start.
    const MacroVoice& Voice(const KeyVoice& voice);

    // The amplitude of `voice` at `volume`: the volume squared on the DLS curve, else the volume table's level.
    double Level(const MacroVoice& voice, double volume) const;

    // Sample `id`, decoded once. Return nullptr if the sample directory lacks it, or record why if it can't be
    // decoded.
    const Sample* GetSample(uint16_t id);

    // The sample that `voice` starts, as GetSample gives it. Return nullptr for a voice that starts none.
    const Sample* SampleOf(const MacroVoice& voice);

    // The seconds that sample `id` plays when its pitch is `ratio` times its rate, or nothing if it loops. Return 0 for
    // a sample that can't be decoded.
    std::optional<double> SampleSeconds(uint16_t id, double ratio);

    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    const SongGroup& group_;
    VolumeTable volumes_;
    std::map<uint16_t, MacroVoice> macros_;
    // By macro, part and the note's start (VoiceFrom).
    std::map<std::tuple<uint16_t, std::size_t, double>, MacroVoice> later_macros_;
    std::map<uint16_t, std::unique_ptr<Sample>> samples_;
    std::map<uint16_t, std::string> sfz_names_;
    std::vector<std::string> errors_;
};

// A voice of a sound effect: a voice that the effect's object starts, or a voice that a macro starts on another voice
// (PlayMacro). The voice's macro starts `start_ms` after the effect.
struct EffectVoice
{
    KeyVoice voice;
    double start_ms = 0.0;
};

// The voices that MusyX plays for an effect of object `object` at `key`, with `instruments`: the object's voices and
// the voices that their macros start, in the order of their starts. A voice that starts one sample after another
// comes once for each part (MacroVoice::parts). MusyX has 64 voices. Add to `approximations` what the list leaves
// out.
std::vector<EffectVoice> EffectVoicesOf(Instruments& instruments, uint16_t object, int key,
                                        std::set<std::string>& approximations);

} // namespace dolphinsf2::musyx
