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
#include <utility>
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

    // The DLS envelope's attack and decay are attack_ms and decay_ms, in whole milliseconds, with no scales. An
    // envelope that a macro takes from the channel's controllers gives its times so (SetAdsrFromControllers).
    bool times_in_ms = false;
};

// The values from 0 to 127 of some of a song channel's MIDI controllers, by controller, in the order of the
// controllers. The controllers are those whose values a voice's macro reads (MacroVoice::controllers). An empty list
// stands for a voice without a channel's controllers: an effect's voice, or a voice whose macro reads none.
using ChannelControls = std::vector<std::pair<uint8_t, uint8_t>>;

// The stand-in for the note's velocity among a song's channel controls. A split on the voice's volume reads it
// (SplitVelocity). A channel's controls hold the note's velocity there, and a voice's controls hold the voice's start
// volume (VoiceControls). MusyX's controllers end at 0xA4, so no controller has this number.
constexpr uint8_t kVelocityControl = 0xff;

// The MIDI controllers that MusyX songs and macros give a meaning: the mod wheel, the volume, the pan, the expression
// and the sends to aux A and aux B.
constexpr uint8_t kModWheel = 1;
constexpr uint8_t kVolumeController = 7;
constexpr uint8_t kPanController = 10;
constexpr uint8_t kExpressionController = 11;
constexpr uint8_t kReverbController = 91;
constexpr uint8_t kChorusController = 93;

// The MusyX controllers from 0x80 up that a macro step can name: the channel pitch wheel, the two voice LFOs and
// controller 132. MusyX turns 0x82 and 0x83 into 0xA0 and 0xA1 (inpTranslateExCtrl, 0x80161D20 in Pokémon Colosseum).
constexpr uint8_t kPitchWheelControl = 0x80;
constexpr uint8_t kLfo0Control = 0x82;
constexpr uint8_t kLfo1Control = 0x83;
constexpr uint8_t kController132 = 0x84;

// The controller whose value MusyX keeps for controller `controller` of a song's channel. Below 64, the controllers
// come in pairs of 14 bits. A controller below 32 holds the high 7 bits, and the one 32 above it the low 7. MusyX
// reads both through the lower one.
uint8_t KeptController(uint8_t controller);

// The volume from 0 to 127 with which a voice starts for a note of velocity `velocity`: the velocity times `share`, the
// share of the note's volume that the layers give the voice (KeyVoice::volume). MusyX multiplies the volume by each
// layer's volume in 127ths and truncates (StartLayer, 0x8014A5CC in Pokémon Colosseum). The share holds the product of
// the layers' volumes. A voice under two layers can therefore start 1 above MusyX's volume.
int StartVolume(int velocity, double share);

// `controls`, the controls of a song's channel at a note's start, for a voice of the note that takes `share` of its
// volume: the note's velocity replaced by the voice's start volume (StartVolume).
ChannelControls VoiceControls(const ChannelControls& controls, double share);

// The sustain under which a linear envelope's decay counts as a fall to silence: 40 dB down. MusyX ends a voice whose
// decay reaches a sustain of 0. A voice held further down plays far under the music, and its note ends there too.
constexpr double kSilentSustain = 0.01;

// The files fit a linear fall to silence by its energy: a fall of kEnergyFitDb in the game's time. The game's straight
// line from full level falls as 1 - t/T, with an energy of T/3. A fall of r dB in T has an energy of 10 T / (r ln 10).
// The two match at r = 30 / ln 10, about 13 dB.
constexpr double kEnergyFitDb = 30.0 / std::numbers::ln10;

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
    kFade,    // it plays on and follows its macro's fade (MacroVoice::release_fade) until it stops (ReleaseFade)
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
// volume, in order, and the stop `stop_ms` after the release. A StopSample or a KeyOff after the release is the stop.
// At a KeyOff, a voice that still sounds plays its envelope's release (`releases`). Any other stop ends the voice in
// kCutMs.
struct ReleaseFade
{
    std::vector<VolumeRamp> ramps;
    double stop_ms = 0.0;
    bool releases = false;
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

// An LFO that a macro puts on a voice's pitch wheel (PitchWheelSelect): a sine of `period_ms` that swings the wheel by
// `share` of its range either way, from `delay_ms` after the note's start. The voice's ranges (MacroVoice::bend_up and
// bend_down) turn the wheel's swing into a pitch.
struct WheelLfo
{
    double delay_ms = 0.0;
    double period_ms = 0.0;
    double share = 0.0;
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

// The settings of a voice's pitch from `ms` after the voice's start: its keys added (AddKey), its fixed key (SetKey),
// its detune and its rate (SetPitch). MacroVoice holds the same fields.
struct PitchSettings
{
    double ms = 0.0;
    int key_shift = 0;
    std::optional<int> fixed_key;
    double detune_cents = 0.0;
    std::optional<double> rate_hz;
};

// A pitch sweep that a PitchSweep1 step (`slot` 0) or a PitchSweep2 step (`slot` 1) starts `ms` after the voice's
// start. Every 16 ms, MusyX adds `step` to the voice's pitch, in the DSP's units: 4096ths of the output's rate. After
// `steps` of them, the sweep starts again from the voice's pitch. A sweep of 0 steps turns the slot's sweep off.
struct PitchSweep
{
    double ms = 0.0;
    int slot = 0;
    int steps = 0;
    int step = 0;
};

// A pan that a Panning step sets `ms` after the voice's start: `pan` from 0 to 127, 64 the centre. MusyX then moves
// the pan by `delta` over `glide_ms`, in a line.
struct PanSetting
{
    double ms = 0.0;
    int pan = 64;
    int delta = 0;
    double glide_ms = 0.0;
};

// The approximation that a voice's report gives for the SoundMacro commands that dolphinsf2 doesn't follow.
inline constexpr char kCommandsLeftOut[] = "SoundMacro commands that dolphinsf2 doesn't follow left out";

// The approximation that a report gives for the sources of the send to aux A that dolphinsf2 leaves out (ReverbSelect).
inline constexpr char kReverbSourcesLeftOut[] = "reverb sources that a macro selects left out";

// A source that a select step (ReverbSelect) names for one of a voice's inputs: a MIDI controller, or with `variable`
// one of the voice's variables, and its share (bytes 2, 3 and 6 of the step).
struct InputSource
{
    uint8_t controller = 0;
    bool variable = false;
    double share = 1.0;
};

// A controller's full value in MusyX's inputs: 14 bits.
constexpr double kFullInput = kWheelHighest;

// The value of `source`, from 0 to kFullInput, with the source's controller at `controller` in the same units. MusyX
// takes a variable, the mod wheel (CC1) and the pan (CC10) as signed sources. Their value is half of full plus the
// source's distance from that centre times the share, held to the 14 bits. A macro's variables are 0. Another
// controller gives its value times the share, up to full (0x80161134 in Pokémon Colosseum).
double SourceValue(const InputSource& source, double controller);

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

    // The settings above give the tuning of the voice's zone. `pitch_changes` holds each change of them, in order,
    // and `sweeps` the pitch sweeps. MusyX updates a voice's pitch and pan every 15 ms, at its start and at the end
    // of each of its macro's waits (`updates`). These times count from the voice's start. VoiceFrom moves them to the
    // note's start. An effect's note follows them with pitch bends (PitchCurve).
    std::vector<PitchSettings> pitch_changes;
    std::vector<PitchSweep> sweeps;
    std::vector<double> updates;

    double volume = 1.0;     // the scale of the note's volume (ScaleVolumeDLS, ScaleVolume, FadeIn)
    double volume_add = 0.0; // what ScaleVolume and FadeIn add to the scaled volume, in units of full volume

    // The time from the voice's start over which its volume rises from silence (FadeIn).
    double fade_in_ms = 0.0;

    // The share of the volume controller (CC7) that the voice's volume follows (VolumeSelect). MusyX holds the
    // controller's value times the share at full volume. With `source_fixed`, the voice follows a variable instead.
    // Its volume then takes `source_volume` as it is. The controller doesn't change it.
    double source_volume = 1.0;
    bool source_fixed = false;

    // The volume controller from 0 to 1, if the macro sets it (SetVariable on a controller). In an effect, it takes the
    // place of the effect's volume controller at full. A song's channel holds the controller. A song's note leaves it
    // out.
    std::optional<double> volume_controller;

    // The sends to aux A and aux B (CC91 and CC93) from 0 to 1, if the macro sets them. An effect's note takes them in
    // place of the effect's sends. A song's note leaves them out.
    std::optional<double> reverb_controller;
    std::optional<double> chorus_controller;

    // The source of the send to aux A that a ReverbSelect puts in place of the reverb controller (CC91). MusyX starts a
    // voice on CC91 in full.
    std::optional<InputSource> reverb_source;

    bool dls_volume = false; // the voice's level is the square of its volume (ModeSelect)
    bool itd = false;        // the pan narrows to 0.766 of its range (ModeSelect)

    // A pan that follows the key, in place of the pan of the keymap or the layer.
    std::optional<PianoPan> piano_pan;

    // A pan from 0 to 127 that the voice sets itself (Panning). The voice takes it in place of the pan of the keymap
    // or the layer, and of a PianoPan before it.
    std::optional<int> pan;

    bool pan_last = false;  // a Panning step comes after the last PianoPan
    bool pan_moves = false; // a Panning step moves the pan (the voice keeps the first pan)

    // Each Panning step, in order. An effect's note follows them with CC10 (PanCurve).
    std::vector<PanSetting> pan_changes;

    // The voices that the macro starts on other voices (PlayMacro), in order.
    std::vector<StartedVoice> started;

    // The parts of a voice whose macro starts one sample after another, one for each sample: from the sample's start
    // to the next one's, with the settings that the macro has made by the time the sample has played for a moment.
    // Empty for a voice that starts one sample. A song's note plays the first sample with the voice's settings, and an
    // effect plays every part.
    std::vector<MacroVoice> parts;

    std::optional<Envelope> envelope;

    // The controllers whose values the envelope takes (SetAdsrFromControllers), in order. MusyX reads the 7 high bits
    // of each. A controller below 32 stands for its pair of 14 bits.
    std::vector<uint8_t> controllers;

    // The splits that the macro meets, in order: each a controller and the value from which the voice goes another way.
    // SplitModulation reads the mod wheel, and SplitVelocity the start volume (kVelocityControl). A song's voice reads
    // its controls there. An effect's voice goes on. The list comes from a run without controls, which goes on at each
    // split. A split that only another way reaches isn't listed.
    std::vector<std::pair<uint8_t, uint8_t>> splits;

    // The ramps of the voice's volume while its note is held, up to the voice's stop (stop_ms).
    std::vector<VolumeRamp> held_ramps;

    KeyOff key_off = KeyOff::kIgnored;
    ReleaseFade release_fade; // for KeyOff::kFade

    // The voice's KeyOff comes at stop_ms whatever its note's release does. The release doesn't end the voice.
    bool releases_itself = false;

    std::optional<double> stop_ms; // when a held note's voice stops or releases itself, after its start
    std::optional<Vibrato> vibrato;
    std::optional<Tremolo> tremolo;
    std::optional<PitchEnvelope> pitch_envelope;
    uint8_t key_group = 0; // the voice stops the group's other voices when it starts
    int bend_up = 2;       // the pitch wheel's range in semitones above the note (PitchWheelRange)
    int bend_down = 2;     // and below it

    // Whether the channel's pitch wheel bends the voice. A macro can turn the voice's pitch wheel to other sources
    // (PitchWheelSelect), such as an LFO (`wheel_lfo`).
    bool follows_wheel = true;
    std::optional<WheelLfo> wheel_lfo;

    // What the macro does that a SoundFont doesn't play, for the conversion's report.
    std::vector<std::string> approximations;
};

// Follow SoundMacro `macro` of `pool` for a note. A macro that the pool lacks starts nothing. ScaleVolume divides by
// `volume_scale_divisor` (SongGroup). A song's note gives its channel's controllers in `controls`. With none, the voice
// leaves out an envelope that it takes from the controllers.
MacroVoice FollowMacro(const ntdlib::musyx::Pool& pool, uint16_t macro, int volume_scale_divisor,
                       const ChannelControls& controls = {});

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
// (MacroVoice::parts): `part` is then 1 plus the part's index, else 0. A song's voice whose macro reads its channel's
// controllers holds their values at the note's start in `controls`.
struct KeyVoice
{
    uint16_t macro = 0;
    int key = 60;
    int pan = 64;
    double volume = 1.0;
    double from_ms = 0.0;
    std::size_t part = 0;
    ChannelControls controls;
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

// A point on the curve of a voice's pitch: from `ms` after the note's start, the voice plays `semitones` away from the
// pitch that its settings give it.
struct PitchPoint
{
    double ms = 0.0;
    double semitones = 0.0;

    bool operator==(const PitchPoint&) const = default;
};

// The curve of the pitch of `voice` from its note's start to `ms` after it, or to the voice's stop: a point for each of
// MusyX's updates that moves the pitch. The voice plays `sample` and starts with `key`. The pitch wheel bends it by
// `wheel` semitones. The points count from the voice's settings. Those give its zone's tuning (MacroVoice::key_shift
// to rate_hz). MusyX adds the sweeps to the DSP's pitch after the wheel. The DSP plays a pitch of 0x4000 or more at
// 0x3fff, just under 4 times the output's rate. A negative pitch plays there too (0x8014b044 and 0x8016292c in
// Pokémon Colosseum). The curve starts with a point at 0.
std::vector<PitchPoint> PitchCurve(const MacroVoice& voice, const Sample& sample, int key, double wheel, double ms);

// A point on the curve of a voice's pan: from `ms` after the note's start, the voice's pan lies `offset` steps of CC10
// away from the pan of its last Panning step.
struct PanPoint
{
    double ms = 0.0;
    double offset = 0.0;

    bool operator==(const PanPoint&) const = default;
};

// The curve of the pan of `voice` from its note's start to `ms` after it, or to the voice's stop: a point for each of
// MusyX's updates that moves the pan. MusyX moves a pan at each update by the time since the update before
// (0x8014b044 in Pokémon Colosseum). It holds the pan within 0 to 127. The points count from the pan of the voice's
// last Panning step. Empty for a voice whose pan doesn't move or whose pan comes from elsewhere.
std::vector<PanPoint> PanCurve(const MacroVoice& voice, double ms);

// Whether `curve` moves the pitch away from where the wheel bends it by `wheel` semitones.
bool PitchMoves(const std::vector<PitchPoint>& curve, double wheel);

// The milliseconds that `voice`, started with `key`, plays `sample` for along the curve of its pitch without a wheel
// (PitchCurve). Nothing if the sample loops or the curve doesn't move the pitch.
std::optional<double> CurveSampleMs(const MacroVoice& voice, const Sample& sample, int key);

// Whether the DSP plays `voice`, started with `key` on `sample`, at its highest pitch. The DSP reads at most just under
// 4 frames of a sample for each frame of its output (0x80162944 in Pokémon Colosseum). A voice whose settings ask for
// more plays at that limit.
bool AtHighestPitch(const MacroVoice& voice, const Sample& sample, int key);

// The rate in hertz at which the DSP plays a sample at its highest pitch.
double HighestPitchHz();

// The song group's macros as voices, and its samples decoded on demand. Read failures are collected in Errors() and
// silence only the voices they affect.
class Instruments
{
public:
    // A preset of a song for a page whose voices read the channel's controllers: the page's preset key, with the
    // values of the controllers (ObjectControllers). Each such preset goes by a bank of its own, kVariantBank and up.
    struct Variant
    {
        uint32_t bank = 0;
        uint32_t program = 0;
        ChannelControls controls;
    };

    static constexpr uint32_t kVariantBank = 0x100;

    // Read `group`. The group's voices play on the game's volume table `volumes` unless they pick the DLS curve.
    // `group` must outlive the instruments. The SFZ files give a sample the name that `sfz_names` holds for its ID
    // (SfzSampleNames), or "s" and its ID, such as "s12".
    Instruments(const SongGroup& group, const VolumeTable& volumes, std::map<uint16_t, std::string> sfz_names = {});

    // The instruments keep a reference to the group. A temporary group can't be read.
    Instruments(SongGroup&& group, const VolumeTable& volumes, std::map<uint16_t, std::string> sfz_names = {}) = delete;

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
    // the voice does (VoiceFrom), made once. A voice whose macro reads its channel's controllers takes their values
    // from `controls`.
    const MacroVoice& Macro(uint16_t macro, std::size_t part, double from_ms, const ChannelControls& controls = {});

    // The controllers whose values a voice of object `object` reads at some key, in order: those of its envelope
    // (MacroVoice::controllers) and those of its splits (MacroVoice::splits). The note's velocity counts as
    // kVelocityControl.
    const std::vector<uint8_t>& ObjectControllers(uint16_t object);

    // The value of controller `controller` that stands for `value` among the controls of a channel that plays object
    // `object` (ObjectControllers). For kVelocityControl, `value` is the note's velocity. A controller that only splits
    // read gives the highest value at which one of them turns, up to `value`, or 0. Every value in between turns
    // every split the same way. The voices therefore don't change, and neither does the channel's preset.
    uint8_t ObjectValue(uint16_t object, uint8_t controller, uint8_t value);

    // The bank of the preset that plays the page with bank `bank` and program `program` at `controls`, added on
    // first use.
    uint32_t VariantBank(uint32_t bank, uint32_t program, const ChannelControls& controls);

    // The preset that variant bank `bank` stands for, or nullptr for a bank below kVariantBank.
    const Variant* VariantOf(uint32_t bank) const;

    // The voice that `voice` plays from its note's start.
    const MacroVoice& Voice(const KeyVoice& voice);

    // The amplitude of `voice` at `volume`: the volume squared on the DLS curve, else the volume table's level.
    double Level(const MacroVoice& voice, double volume) const;

    // Sample `id`, decoded once. Return nullptr if the sample directories have no such sample, or if it can't be
    // decoded. The instruments record why a sample can't be decoded.
    const Sample* GetSample(uint16_t id);

    // The sample that `voice` starts, as GetSample gives it. Return nullptr for a voice that starts none.
    const Sample* SampleOf(const MacroVoice& voice);

    // The seconds that sample `id` plays when its pitch is `ratio` times its rate, or nothing if it loops. Return 0 for
    // a sample that GetSample doesn't give.
    std::optional<double> SampleSeconds(uint16_t id, double ratio);

    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    // What the voices of an object read (ObjectControllers): the controllers, those that some voice reads in full, and
    // for the others the values at which a split turns.
    struct ObjectReads
    {
        std::vector<uint8_t> controllers;
        std::set<uint8_t> whole;
        std::map<uint8_t, std::set<uint8_t>> turns;
    };

    // What the voices of `object` read, worked out on first use.
    const ObjectReads& Reads(uint16_t object);

    const SongGroup& group_;
    VolumeTable volumes_;
    std::map<uint16_t, MacroVoice> macros_;

    // By macro, part and the note's start (VoiceFrom).
    std::map<std::tuple<uint16_t, std::size_t, double>, MacroVoice> later_macros_;

    // By macro, part, the note's start and the controllers' values, for the macros that read them.
    std::map<std::tuple<uint16_t, std::size_t, double, ChannelControls>, MacroVoice> controlled_macros_;

    std::map<uint16_t, ObjectReads> object_reads_;
    std::vector<Variant> variants_;
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
