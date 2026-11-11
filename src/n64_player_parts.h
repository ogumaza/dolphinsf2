// SPDX-License-Identifier: MIT

// The parts of the Nintendo 64 sequence player that n64_player.cpp and n64_player_layers.cpp share: the player's
// states and the class Performer (see n64_player.cpp).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ntdlib/sequence_export/passes.h"
#include "seqlib/midi/recorder.h"

#include "n64_player.h"
#include "output_units.h"

namespace dolphinsf2::n64::player
{

using seqlib::midi::Recorder;
using seqlib::midi::TrackOutput;

// The tatums of a beat.
constexpr int kTatumsPerBeat = 48;

// The number of channels and of a channel's layers, the depth of a script's calls and loops, and the ports.
constexpr std::size_t kChannels = 16;
constexpr std::size_t kLayers = 4;
constexpr std::size_t kDepth = 4;
constexpr std::size_t kPorts = 8;

// The instrument numbers that stand for the bank's drums and effects, and for no instrument.
constexpr uint8_t kDrumsProgram = 0x7f;
constexpr uint8_t kEffectsProgram = 0x7e;
constexpr int kNoInstrument = -1;

// A note's gate time and a layer's pan at the layer's start.
constexpr uint8_t kStartGateTime = 0x80;
constexpr uint8_t kCentre = 64;

// The value that a script ends with at depth 0.
constexpr int kScriptEnd = -1;

// The steps of a sweep from its first note to its second.
constexpr double kSweepSteps = 127.0;

// The release that a channel takes before its script sets one: a decay index (ReleaseFrames).
constexpr uint8_t kDefaultRelease = 240;

// The ticks that an output sample of the library's synthesis takes at a tempo of 1. The synthesis makes
// kSamplesPerUpdate samples an update, and a tick runs every update at kMaxTempo.
constexpr double kTicksPerSampleAndTempo = 1.0 / kSamplesPerUpdate / kMaxTempo;

// The state of a script: where it reads, its calls and loops, and its value.
struct Script
{
    bool operator==(const Script&) const = default;

    uint32_t pc = 0;
    std::array<uint32_t, kDepth> stack{};
    std::array<uint8_t, kDepth> loops{};
    uint8_t depth = 0;
    int8_t value = 0;
};

// An envelope and the decay index of a release (the library's adsr settings).
struct Adsr
{
    bool operator==(const Adsr&) const = default;

    std::vector<EnvelopePoint> envelope;
    uint8_t release = 0;
};

// The vibrato's settings of a channel or a layer: the start and the target of its rate and its depth, the updates
// over which they move to their targets, and the updates before it starts.
struct Vibrato
{
    bool operator==(const Vibrato&) const = default;

    uint16_t rate_start = 2048;
    uint16_t depth_start = 0;
    uint16_t rate_target = 2048;
    uint16_t depth_target = 0;
    uint16_t rate_delay = 0;
    uint16_t depth_delay = 0;
    uint16_t delay = 0;
};

// A layer's sweep from one note to another (the library's portamento). `mode` holds the sweep's kind in its low bits
// and the bit of the special sweep, whose time scales with the note's length. `target` is the other note, in the
// library's semitones with the transpositions. While a note sweeps, `steps` counts up to kSweepSteps by `rate` an
// update, and `extent` is the share of the starting frequency that the whole sweep adds.
struct Portamento
{
    bool operator==(const Portamento&) const = default;

    uint8_t mode = 0;
    int target = 0;
    uint16_t time = 0;
    double steps = 0.0;
    double rate = 0.0;
    double extent = 0.0;
};

// A layer and its note.
struct Layer
{
    bool operator==(const Layer&) const = default;

    Script script;
    bool finished = false;
    bool muted = false;
    bool continuous = false;
    bool ignore_drum_pan = false;
    int instrument = 0xff; // 0xff: the channel's; else a program, kDrumsProgram or kEffectsProgram
    Adsr adsr;
    int16_t delay = 0;
    int16_t gate_delay = 0;
    int16_t last_delay = 0;
    int16_t short_delay = 0;
    uint8_t gate_time = kStartGateTime;
    float velocity = 0.0f; // squared, over 127²
    uint8_t pan = kCentre;
    int16_t transposition = 0;
    float bend = 1.0f;
    Portamento portamento;

    // The note that sounds, if any: its id, its key, the sample that it plays, and its pitch from its key in semitones
    // before the sweep. A legato note moves the pitch of the note before.
    uint32_t note = 0;
    uint8_t key = 0;
    const Sample* sample = nullptr;
    float offset = 0.0f;
};

// A channel.
struct Channel
{
    bool operator==(const Channel&) const = default;

    bool enabled = false;
    bool stop_script = false;
    bool large_notes = false;
    bool muted = false;
    Script script;
    int16_t delay = 0;
    uint8_t bank = 0xff;
    int instrument = kNoInstrument; // a program, kDrumsProgram or kEffectsProgram
    Adsr adsr;
    float volume = 1.0f;
    float volume_scale = 1.0f;
    float frequency_scale = 1.0f;
    uint8_t pan = kCentre;
    uint8_t pan_weight = 128;
    int8_t transposition = 0;
    uint8_t reverb = 0;
    Vibrato vibrato;
    std::array<int8_t, kPorts> ports{-1, -1, -1, -1, -1, -1, -1, -1};
    uint16_t dynamic_value = 0;
    uint32_t dynamic_table = 0;
    uint8_t velocity_variance = 0;
    std::array<std::optional<Layer>, kLayers> layers;

    // The pitch in semitones that a layer's legato note or sweep adds to the channel's. It holds through the releases
    // until a note sounds again.
    float layer_pitch = 0.0f;
};

// The sequence player: its script, its data (which the scripts can write), its tempo, volume and fades, and its
// channels.
struct Group
{
    bool operator==(const Group&) const = default;

    bool enabled = false;
    bool stop_script = false;
    std::vector<uint8_t> data;
    Script script;
    int16_t delay = 0;
    uint16_t tempo = 120 * kTatumsPerBeat;
    int16_t tempo_change = 0;
    int16_t transposition = 0;
    uint8_t bank = 0xff;
    uint32_t sequence = 0;

    // The fade: its state (0 normal, 1 fading in, 2 fading out), the volume, its step an update and the updates left.
    // The stored time is the time over which the next volume fades.
    int state = 1;
    float fade_volume = 1.0f;
    float fade_volume_scale = 1.0f;
    float fade_velocity = 0.0f;
    uint16_t fade_timer = 0;
    uint16_t stored_fade_timer = 0;

    std::array<int8_t, kPorts> ports{-1, -1, -1, -1, -1, -1, -1, -1};
    int short_velocities = -1; // an offset in the data, or -1 for the library's table
    int short_gate_times = -1;
    std::array<Channel, kChannels> channels;
};

// The bend of large bends (PCENTTABLE) and of small ones (PCENTTABLE2) for an argument `arg`, as a frequency factor.
inline float Bend(const std::array<float, 256>& table, int8_t arg)
{
    return table[static_cast<std::size_t>(128 + arg)];
}

// What a note plays: the voice of its preset, its MIDI key, and its sample at its tuning. An instrument's note also
// gives the semitone at which the library's table of pitches tunes it.
struct NoteSound
{
    PresetVoice voice;
    int key = 0;
    const Sample* sample = nullptr;
    float tuning = 1.0f;
    int pitch_index = 0;
};

// Plays a piece and records it.
class Performer
{
public:
    // `level_scale` scales each channel's level in CC11 (ntdlib's LevelScale).
    Performer(const Piece& piece, SoundData& data, const PerformOptions& options, double level_scale);

    Played Run();

    // The loudest level that the CC11 of a channel took, unscaled: 127 at full level. A muted channel plays no notes
    // and doesn't count.
    double PeakLevel() const
    {
        return 127.0 * peak_level_;
    }

private:
    // Reading the scripts.
    uint8_t ReadByte(Script& s) const;
    int16_t ReadWord(Script& s) const;
    uint16_t ReadLength(Script& s) const;
    uint16_t ArgOf(Script& s, uint8_t command) const;

    // The byte and the big-endian word at `at` in the sequence's data. Throw FormatError past its end.
    uint8_t DataByte(std::size_t at) const;
    uint16_t DataWord(std::size_t at) const;

    // The flow commands from 0xf2 that the three kinds of script share. Returns 0 to go on, the ticks to wait, or
    // kScriptEnd.
    int Common(Script& s, uint8_t command, uint16_t arg);

    // The library's random numbers: the high 16 bits of the next state of its generator. The report counts each
    // choice: the generator starts from a fixed state.
    uint32_t NextRandom();

    // A tick of the player, of a channel and of a layer.
    void GroupTick();
    void ChannelTick(std::size_t c);
    void LayerTick(std::size_t c, std::size_t l);

    // Runs the player's script up to its next wait, or until it ends the player or stops itself.
    void GroupScript();

    // Runs command `command` (0xc0 to 0xf1) of the player's script if it sets the tempo, the transposition, the fades
    // or the channels' banks and mutes. Returns false for any other command.
    bool GroupSetting(uint8_t command);

    // Runs the other commands from 0xc0 to 0xf1 of the player's script: the value and the stop of the script, the
    // sequence data and the start of another sequence.
    void GroupFlow(uint8_t command);

    // Runs command `command` (below 0xc0) of the player's script: the channels' starts and ends, and the ports.
    void GroupChannelCommand(uint8_t command);

    // The fade commands of the player's script: a state (0 normal, 1 fading in, 2 fading out) with its time, and a
    // volume to fade to.
    void SetFadeState(uint8_t state, uint16_t time);
    void SetFadeVolume(float volume);

    // Releases every channel and stops the player.
    void EndGroup();

    // Runs channel `c`'s script up to its next wait or its stop. Returns false if the script ends the channel.
    bool ChannelScript(std::size_t c);

    // Runs command `command` (0xa0 to 0xf1) of channel `c`'s script if it sets the channel's instrument, levels,
    // pitch, pan, envelope or reverb. Returns false for any other command.
    bool ChannelSetting(std::size_t c, uint8_t command);

    // Runs command `command` (0xa0 to 0xf1) of channel `c`'s script if it sets the channel's vibrato. Returns false
    // for any other command.
    bool ChannelVibrato(std::size_t c, uint8_t command);

    // Runs the other commands from 0xa0 to 0xf1 of channel `c`'s script: the value and the stop of the script, the
    // sequence data, and the commands that the files leave out.
    void ChannelFlow(std::size_t c, uint8_t command);

    // Runs command `command` (0x70 to 0x9f) of channel `c`'s script: the layers' starts and ends, and the ports.
    void LayerControl(std::size_t c, uint8_t command);

    // Runs command `command` (below 0x70) of channel `c`'s script: a short wait, the ports and the other channels.
    // Returns true if the script waits.
    bool ChannelLowCommand(std::size_t c, uint8_t command);

    // A layer's commands up to its next note: returns the note's command, or kScriptEnd.
    int LayerCommands(std::size_t c, std::size_t l);

    // Sets the instrument of layer `l` of channel `c` to `program`, with its envelope and release.
    void SetLayerInstrument(std::size_t c, std::size_t l, uint8_t program);

    // Starts the note of `command` on layer `l` of channel `c`: its length and gate. Returns the note's semitone, the
    // command's low 6 bits, or kScriptEnd for a rest.
    int SetNote(std::size_t c, std::size_t l, int command);

    // Plays note `note` on layer `l` of channel `c`. A note that makes no sound mutes the layer.
    void SetVoice(std::size_t c, std::size_t l, int note);

    // What note `note` on layer `l` of channel `c` plays, or nothing if it makes no sound.
    std::optional<NoteSound> SoundOf(std::size_t c, std::size_t l, int note);

    // What instrument `instrument` of `bank` plays for `note` on layer `layer` of channel `ch`, or nothing if it makes
    // no sound. The note starts the layer's sweep.
    std::optional<NoteSound> InstrumentSound(const Channel& ch, Layer& layer, const Bank* bank, int instrument,
                                             int note);

    // The ticks of a note of no length on `layer`: as long as `sound`'s sample plays at the tempo.
    int16_t SampleTicks(const NoteSound& sound, const Layer& layer) const;

    // Sets the steps an update of `layer`'s sweep. A special sweep takes the share `time` / 256 of the note's length.
    void SetSweepRate(Layer& layer);

    void OpenChannel(std::size_t c, uint32_t pc);
    void ReleaseChannel(std::size_t c);
    void StartLayer(std::size_t c, std::size_t l, uint32_t pc);
    void CloseLayer(std::size_t c, std::size_t l);
    void SetInstrument(std::size_t c, uint8_t program);
    std::optional<uint8_t> BankFromMap(uint8_t index) const;

    // Ends the note that layer `l` of channel `c` plays, if any.
    void Release(std::size_t c, std::size_t l);

    // The fades of an update (Nas_MainCtrl).
    void Update();

    // Sends the channels' controllers for the current tick.
    void SendControllers();

    // The pitch in semitones that the layers of channel `c` add to the channel's pitch for the current tick.
    float LayerPitch(std::size_t c);

    // The level of channel `c`, before the game's mutes: its volume times the player's.
    double Level(const Channel& ch) const;

    uint32_t PresetOf(const PresetVoice& voice);

    // The longest release of the performance's presets, in seconds.
    double TailSeconds();

    // Writes `value` at `at` in the sequence's data.
    void WriteData(std::size_t at, uint8_t value);

    // A hash of the whole state, which repeats when the music does.
    uint64_t Signature() const;

    const Piece& piece_;
    SoundData& data_;
    const PerformOptions& options_;
    Group group_;
    Played out_;
    Recorder recorder_;
    std::array<TrackOutput, kChannels> outputs_{};
    uint32_t tick_ = 0;
    uint64_t update_ = 0;
    uint32_t random_ = 0;
    int tempo_accumulator_ = 0;
    mutable std::optional<uint64_t> data_hash_;
    double level_scale_ = 1.0;
    double peak_level_ = 0.0;
    std::map<std::string, uint32_t> approximations_;
    std::vector<std::string> warnings_;
};

} // namespace dolphinsf2::n64::player
