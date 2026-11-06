// SPDX-License-Identifier: MIT

// The sequence player of the Nintendo 64's sound library (see n64_player.h).
//
// The model follows the library's build in Animal Crossing's main.dol: the sequence player's script (Nas_GroupSeq),
// the channels' (Nas_SubSeq) and the layers' (__Command_Seq, __SetNote and __SetVoice), and the update that steps the
// fades (Nas_MainCtrl). A tick runs when the player's tempo, added up once an update, passes the library's maximum. The
// voices aren't modelled. Their envelopes, releases and vibratos go to the SoundFont.

#include "n64_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/passes.h"
#include "seqlib/midi/recorder.h"

namespace dolphinsf2::n64
{
namespace
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

// The longest performance, in seconds: a piece that hasn't repeated or ended by then stops there.
constexpr double kLimitSeconds = 15.0 * 60.0;

// A note's gate time and a layer's pan at the layer's start.
constexpr uint8_t kStartGateTime = 0x80;
constexpr uint8_t kCentre = 64;

// The value that a script ends with at depth 0.
constexpr int kScriptEnd = -1;

// The steps of a sweep from its first note to its second.
constexpr double kSweepSteps = 127.0;

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
float Bend(const std::array<float, 256>& table, int8_t arg)
{
    return table[static_cast<std::size_t>(128 + arg)];
}

// The 64-bit FNV-1a hash of `bytes`.
uint64_t Fnv1a(std::span<const uint8_t> bytes)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (uint8_t b : bytes)
    {
        hash = (hash ^ b) * 0x100000001b3;
    }

    return hash;
}

// Plays a piece and records it.
class Performer
{
public:
    // `level_scale` scales each channel's level in CC11 (ntdlib's LevelScale).
    Performer(const Piece& piece, SoundData& data, const PerformOptions& options, double level_scale);

    Played Run();

    // The loudest level that a channel's CC11 took, unscaled: 127 at full level.
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

    // The flow commands from 0xf2 that the three kinds of script share. Returns 0 to go on, the ticks to wait, or
    // kScriptEnd.
    int Common(Script& s, uint8_t command, uint16_t arg);

    // A tick of the player, of a channel and of a layer.
    void GroupTick();
    void ChannelTick(std::size_t c);
    void LayerTick(std::size_t c, std::size_t l);

    // A layer's commands up to its next note: returns the note's command, or kScriptEnd.
    int LayerCommands(std::size_t c, std::size_t l);

    // Starts the note of `command` on layer `l` of channel `c`: its length and gate. Returns the note's semitone, the
    // command's low 6 bits, or kScriptEnd for a rest.
    int SetNote(std::size_t c, std::size_t l, int command);

    // Plays note `note` on layer `l` of channel `c`. A note that makes no sound mutes the layer.
    void SetVoice(std::size_t c, std::size_t l, int note);

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

Performer::Performer(const Piece& piece, SoundData& data, const PerformOptions& options, double level_scale)
    : piece_(piece), data_(data), options_(options), random_(options.seed), level_scale_(level_scale)
{
    for (std::size_t c = 0; c < kChannels; c++)
    {
        outputs_[c].midi_track = static_cast<uint8_t>(c);
    }
}

uint8_t Performer::ReadByte(Script& s) const
{
    if (s.pc >= group_.data.size())
    {
        throw FormatError("a script reads past the end of sequence " + std::to_string(group_.sequence));
    }

    return group_.data[s.pc++];
}

int16_t Performer::ReadWord(Script& s) const
{
    const uint8_t high = ReadByte(s);
    return static_cast<int16_t>(high << 8 | ReadByte(s));
}

uint16_t Performer::ReadLength(Script& s) const
{
    const uint8_t first = ReadByte(s);
    if ((first & 0x80) == 0)
    {
        return first;
    }

    return static_cast<uint16_t>((first & 0x7f) << 8 | ReadByte(s));
}

// The argument of a flow command (from 0xf2): a byte for the relative branches and the loop, else a word.
uint16_t Performer::ArgOf(Script& s, uint8_t command) const
{
    switch (command)
    {
    case 0xf2:
    case 0xf3:
    case 0xf4:
    case 0xf8:
        return ReadByte(s);
    case 0xf5:
    case 0xf9:
    case 0xfa:
    case 0xfb:
    case 0xfc:
        return static_cast<uint16_t>(ReadWord(s));
    default:
        return 0;
    }
}

int Performer::Common(Script& s, uint8_t command, uint16_t arg)
{
    switch (command)
    {
    case 0xff:
        if (s.depth == 0)
        {
            return kScriptEnd;
        }
        s.pc = s.stack[--s.depth];
        break;

    case 0xfd:
        return ReadLength(s);

    case 0xfe:
        return 1;

    case 0xfc:
        if (s.depth < kDepth)
        {
            s.stack[s.depth++] = s.pc;
        }
        s.pc = arg;
        break;

    case 0xf8:
        if (s.depth < kDepth)
        {
            s.loops[s.depth] = static_cast<uint8_t>(arg);
            s.stack[s.depth++] = s.pc;
        }
        break;

    case 0xf7:
        if (s.depth > 0)
        {
            if (--s.loops[s.depth - 1] != 0)
            {
                s.pc = s.stack[s.depth - 1];
            }
            else
            {
                s.depth--;
            }
        }
        break;

    case 0xf6:
        if (s.depth > 0)
        {
            s.depth--;
        }
        break;

    case 0xf5:
    case 0xf9:
    case 0xfa:
    case 0xfb:
        if ((command == 0xfa && s.value != 0) || (command == 0xf9 && s.value >= 0) || (command == 0xf5 && s.value < 0))
        {
            break;
        }
        s.pc = arg;
        break;

    case 0xf2:
    case 0xf3:
    case 0xf4:
        if ((command == 0xf3 && s.value != 0) || (command == 0xf2 && s.value >= 0))
        {
            break;
        }
        s.pc = static_cast<uint32_t>(static_cast<int64_t>(s.pc) + static_cast<int8_t>(arg));
        break;

    default:
        break;
    }

    return 0;
}

std::optional<uint8_t> Performer::BankFromMap(uint8_t index) const
{
    // The index counts from the end of the sequence's list of banks: 1 is the last.
    const std::vector<uint8_t> banks = data_.BanksOf(group_.sequence);
    if (index == 0 || index > banks.size())
    {
        return std::nullopt;
    }

    return data_.ResolveBank(banks[banks.size() - index]);
}

void Performer::OpenChannel(std::size_t c, uint32_t pc)
{
    Channel& ch = group_.channels[c];
    for (std::size_t l = 0; l < kLayers; l++)
    {
        CloseLayer(c, l);
    }
    ch.enabled = true;
    ch.stop_script = false;
    ch.script = Script{};
    ch.script.pc = pc;
    ch.delay = 0;
}

void Performer::ReleaseChannel(std::size_t c)
{
    for (std::size_t l = 0; l < kLayers; l++)
    {
        CloseLayer(c, l);
    }
    group_.channels[c].enabled = false;
}

void Performer::StartLayer(std::size_t c, std::size_t l, uint32_t pc)
{
    Channel& ch = group_.channels[c];
    Release(c, l);
    Layer layer;
    layer.script.pc = pc;
    layer.adsr = ch.adsr;
    layer.adsr.release = 0;
    ch.layers[l] = std::move(layer);
}

void Performer::CloseLayer(std::size_t c, std::size_t l)
{
    Release(c, l);
    group_.channels[c].layers[l].reset();
}

void Performer::Release(std::size_t c, std::size_t l)
{
    std::optional<Layer>& layer = group_.channels[c].layers[l];
    if (layer && layer->note != 0)
    {
        recorder_.NoteOff(out_.performance, static_cast<uint8_t>(c), tick_, outputs_[c].part, layer->key, layer->note);
        layer->note = 0;
    }
}

void Performer::SetInstrument(std::size_t c, uint8_t program)
{
    Channel& ch = group_.channels[c];
    if (program == kDrumsProgram || program == kEffectsProgram)
    {
        ch.instrument = program;
        return;
    }
    if (program >= 0x80)
    {
        approximations_["synthesized waves left out"]++;
        ch.instrument = kNoInstrument;
        return;
    }

    // The instrument's envelope and release become the channel's.
    const Bank* bank = ch.bank != 0xff ? data_.GetBank(ch.bank) : nullptr;
    if (bank == nullptr || program >= bank->instruments.size() || !bank->instruments[program])
    {
        ch.instrument = kNoInstrument;
        return;
    }
    const Instrument& inst = *bank->instruments[program];
    ch.adsr = {inst.envelope, inst.release};
    ch.instrument = program;
}

void Performer::GroupTick()
{
    if (group_.stop_script)
    {
        return;
    }

    if (group_.delay > 1)
    {
        group_.delay--;
    }
    else
    {
        Script& s = group_.script;
        for (;;)
        {
            const uint8_t command = ReadByte(s);
            if (command >= 0xf2)
            {
                const int wait = Common(s, command, ArgOf(s, command));
                if (wait == kScriptEnd)
                {
                    for (std::size_t c = 0; c < kChannels; c++)
                    {
                        ReleaseChannel(c);
                    }
                    group_.enabled = false;
                    return;
                }
                if (wait != 0)
                {
                    group_.delay = static_cast<int16_t>(wait);
                    break;
                }
                continue;
            }

            if (command >= 0xc0)
            {
                switch (command)
                {
                case 0xf1:
                    ReadByte(s);
                    break;

                case 0xdf:
                    group_.transposition = static_cast<int8_t>(ReadByte(s));
                    break;

                case 0xde:
                    group_.transposition =
                        static_cast<int16_t>(group_.transposition + static_cast<int8_t>(ReadByte(s)));
                    break;

                case 0xdd:
                    group_.tempo = static_cast<uint16_t>(std::clamp(ReadByte(s) * kTatumsPerBeat, 1, kMaxTempo));
                    break;

                case 0xdc:
                    group_.tempo_change = static_cast<int16_t>(static_cast<int8_t>(ReadByte(s)) * kTatumsPerBeat);
                    break;

                case 0xda:
                    {
                        const uint8_t state = ReadByte(s);
                        const uint16_t time = static_cast<uint16_t>(ReadWord(s));
                        if (state <= 1 && group_.state != 2)
                        {
                            group_.stored_fade_timer = time;
                            group_.state = state;
                        }
                        else if (state == 2)
                        {
                            group_.fade_timer = time;
                            group_.state = state;
                            group_.fade_velocity = time != 0 ? -group_.fade_volume / time : 0.0f;
                        }
                        break;
                    }

                case 0xdb:
                    {
                        const float volume = static_cast<float>(ReadByte(s)) / 127.0f;
                        if (group_.state == 1)
                        {
                            group_.fade_volume = 0.0f;
                            group_.state = 0;
                        }
                        if (group_.state == 0)
                        {
                            group_.fade_timer = group_.stored_fade_timer;
                            if (group_.fade_timer != 0)
                            {
                                group_.fade_velocity = (volume - group_.fade_volume) / group_.fade_timer;
                            }
                            else
                            {
                                group_.fade_volume = volume;
                            }
                        }
                        break;
                    }

                case 0xd9:
                    group_.fade_volume_scale = static_cast<float>(static_cast<int8_t>(ReadByte(s))) / 127.0f;
                    break;

                case 0xd7:
                    {
                        // The channels whose bit is set take the player's bank and mute settings. The files leave
                        // the mutes out.
                        uint16_t mask = static_cast<uint16_t>(ReadWord(s));
                        for (std::size_t c = 0; c < kChannels; c++, mask >>= 1)
                        {
                            if (mask & 1)
                            {
                                group_.channels[c].bank = group_.bank;
                            }
                        }
                        break;
                    }

                case 0xd6:
                    ReadWord(s);
                    break;

                case 0xd5:
                    ReadByte(s);
                    break;

                case 0xd4:
                    approximations_["the sequence's mute of its channels left out"]++;
                    break;

                case 0xd3:
                    ReadByte(s);
                    break;

                case 0xd1:
                    group_.short_gate_times = static_cast<uint16_t>(ReadWord(s));
                    break;

                case 0xd2:
                    group_.short_velocities = static_cast<uint16_t>(ReadWord(s));
                    break;

                case 0xd0:
                    ReadByte(s);
                    break;

                case 0xce:
                    {
                        const uint8_t range = ReadByte(s);
                        random_ = random_ * 1103515245u + 12345u;
                        const uint32_t r = random_ >> 16;
                        s.value = static_cast<int8_t>(range == 0 ? (r >> 2) & 0xff : (r >> 2) % range);
                        break;
                    }

                case 0xcd:
                    {
                        const uint16_t table = static_cast<uint16_t>(ReadWord(s));
                        if (s.value != -1 && s.depth != 3)
                        {
                            const std::size_t at = table + 2u * static_cast<uint8_t>(s.value);
                            s.stack[s.depth++] = s.pc;
                            s.pc = static_cast<uint32_t>(group_.data.at(at) << 8 | group_.data.at(at + 1));
                        }
                        break;
                    }

                case 0xcc:
                    s.value = static_cast<int8_t>(ReadByte(s));
                    break;

                case 0xc9:
                    s.value = static_cast<int8_t>(s.value & ReadByte(s));
                    break;

                case 0xc8:
                    s.value = static_cast<int8_t>(s.value - ReadByte(s));
                    break;

                case 0xc7:
                    {
                        const uint8_t add = ReadByte(s);
                        const uint16_t at = static_cast<uint16_t>(ReadWord(s));
                        WriteData(at, static_cast<uint8_t>(s.value + add));
                        break;
                    }

                case 0xc2:
                    {
                        const uint16_t table = static_cast<uint16_t>(ReadWord(s));
                        if (s.value != -1)
                        {
                            const std::size_t at = table + 2u * static_cast<uint8_t>(s.value);
                            s.pc = static_cast<uint32_t>(group_.data.at(at) << 8 | group_.data.at(at + 1));
                        }
                        break;
                    }

                case 0xc6:
                    group_.stop_script = true;
                    return;

                case 0xc5:
                    ReadWord(s);
                    break;

                case 0xef:
                    ReadWord(s);
                    ReadByte(s);
                    break;

                case 0xc4:
                    {
                        const uint8_t player = ReadByte(s);
                        const uint8_t sequence = ReadByte(s);
                        if (player == 0xff)
                        {
                            // The script starts a sequence in its player. The player releases every note and the
                            // piece ends there. The game gives Kapp'n's first parts a sequence that ends at once
                            // (Piece::end_sequence). Another sequence would play on in the player.
                            if (sequence != piece_.end_sequence)
                            {
                                approximations_["a sequence that starts another in its player"]++;
                                warnings_.push_back("sequence " + std::to_string(group_.sequence) +
                                                    " starts sequence " + std::to_string(sequence) +
                                                    ", which plays no further");
                            }
                            for (std::size_t c = 0; c < kChannels; c++)
                            {
                                ReleaseChannel(c);
                            }
                            group_.enabled = false;
                            return;
                        }
                        approximations_["a sequence that starts another in another player left out"]++;
                        break;
                    }

                case 0xc3:
                    {
                        const uint16_t table = static_cast<uint16_t>(ReadWord(s));
                        if (s.value != -1)
                        {
                            const std::size_t at = table + 2u * static_cast<uint8_t>(s.value);
                            uint16_t mask = static_cast<uint16_t>(group_.data.at(at) << 8 | group_.data.at(at + 1));
                            for (std::size_t c = 0; c < kChannels; c++, mask >>= 1)
                            {
                                group_.channels[c].muted = (mask & 1) != 0;
                            }
                        }
                        break;
                    }

                default:
                    break;
                }
                continue;
            }

            if (command == 0xbe)
            {
                ReadByte(s);
                approximations_["the game's callbacks from a sequence left out"]++;
                continue;
            }

            const std::size_t low = command & 0xf;
            switch (command & 0xf0)
            {
            case 0x00:
                s.value = group_.channels[low].enabled ? 0 : 1;
                break;

            case 0x50:
                s.value = static_cast<int8_t>(s.value - group_.ports[low & 7]);
                break;

            case 0x70:
                group_.ports[low & 7] = s.value;
                break;

            case 0x80:
                s.value = group_.ports[low & 7];
                if ((low & 7) < 2)
                {
                    group_.ports[low & 7] = -1;
                }
                break;

            case 0x40:
                ReleaseChannel(low);
                break;

            case 0x90:
                OpenChannel(low, static_cast<uint16_t>(ReadWord(s)));
                break;

            case 0xa0:
                {
                    const int16_t offset = ReadWord(s);
                    OpenChannel(low, static_cast<uint32_t>(static_cast<int64_t>(s.pc) + offset));
                    break;
                }

            case 0xb0:
                ReadByte(s);
                ReadWord(s);
                approximations_["sequence loads left out"]++;
                break;

            case 0x60:
                ReadByte(s);
                ReadByte(s);
                approximations_["sequence loads left out"]++;
                break;

            default:
                break;
            }
        }
    }

    for (std::size_t c = 0; c < kChannels; c++)
    {
        if (group_.channels[c].enabled)
        {
            ChannelTick(c);
        }
    }
}

void Performer::ChannelTick(std::size_t c)
{
    Channel& ch = group_.channels[c];
    if (!ch.stop_script)
    {
        if (ch.delay > 1)
        {
            ch.delay--;
        }
        else
        {
            Script& s = ch.script;
            bool waiting = false;
            while (!waiting)
            {
                const uint8_t command = ReadByte(s);
                if (command >= 0xf2)
                {
                    const int wait = Common(s, command, ArgOf(s, command));
                    if (wait == kScriptEnd)
                    {
                        ReleaseChannel(c);
                        return;
                    }
                    if (wait != 0)
                    {
                        ch.delay = static_cast<int16_t>(wait);
                        waiting = true;
                    }
                    continue;
                }

                if (command >= 0xa0)
                {
                    switch (command)
                    {
                    case 0xea:
                        ch.stop_script = true;
                        waiting = true;
                        break;

                    case 0xf1:
                        ReadByte(s);
                        break;

                    case 0xf0:
                        break;

                    case 0xc2:
                        ch.dynamic_table = static_cast<uint16_t>(ReadWord(s));
                        break;

                    case 0xc5:
                        if (s.value != -1)
                        {
                            const std::size_t at = ch.dynamic_table + 2u * static_cast<uint8_t>(s.value);
                            s.pc = static_cast<uint32_t>(group_.data.at(at) << 8 | group_.data.at(at + 1));
                        }
                        break;

                    case 0xeb:
                        {
                            const uint8_t index = ReadByte(s);
                            const uint8_t program = ReadByte(s);
                            if (const std::optional<uint8_t> bank = BankFromMap(index))
                            {
                                ch.bank = *bank;
                            }
                            SetInstrument(c, program);
                            break;
                        }

                    case 0xc1:
                        SetInstrument(c, ReadByte(s));
                        break;

                    case 0xc3:
                        ch.large_notes = false;
                        break;

                    case 0xc4:
                        ch.large_notes = true;
                        break;

                    case 0xdf:
                        ch.volume = static_cast<float>(ReadByte(s)) / 127.0f;
                        break;

                    case 0xe0:
                        ch.volume_scale = static_cast<float>(ReadByte(s)) / 128.0f;
                        break;

                    case 0xde:
                        ch.frequency_scale = static_cast<float>(static_cast<uint16_t>(ReadWord(s))) / 32768.0f;
                        break;

                    case 0xd3:
                        ch.frequency_scale = Bend(data_.Cents(), static_cast<int8_t>(ReadByte(s)));
                        break;

                    case 0xee:
                        ch.frequency_scale = Bend(data_.SmallCents(), static_cast<int8_t>(ReadByte(s)));
                        break;

                    case 0xdd:
                        ch.pan = ReadByte(s);
                        break;

                    case 0xdc:
                        ch.pan_weight = ReadByte(s);
                        break;

                    case 0xdb:
                        ch.transposition = static_cast<int8_t>(ReadByte(s));
                        break;

                    case 0xda:
                        ch.adsr.envelope = ReadEnvelope(group_.data, static_cast<uint16_t>(ReadWord(s)));
                        break;

                    case 0xd9:
                        ch.adsr.release = ReadByte(s);
                        break;

                    case 0xd8:
                        ch.vibrato.depth_target = static_cast<uint16_t>(ReadByte(s) * 8);
                        ch.vibrato.depth_start = 0;
                        ch.vibrato.depth_delay = 0;
                        break;

                    case 0xd7:
                        {
                            const uint16_t rate = static_cast<uint16_t>(ReadByte(s) * 32);
                            ch.vibrato.rate_target = rate;
                            ch.vibrato.rate_start = rate;
                            ch.vibrato.rate_delay = 0;
                            break;
                        }

                    case 0xe2:
                        ch.vibrato.depth_start = static_cast<uint16_t>(ReadByte(s) * 8);
                        ch.vibrato.depth_target = static_cast<uint16_t>(ReadByte(s) * 8);
                        ch.vibrato.depth_delay = static_cast<uint16_t>(ReadByte(s) * 16);
                        break;

                    case 0xe1:
                        ch.vibrato.rate_start = static_cast<uint16_t>(ReadByte(s) * 32);
                        ch.vibrato.rate_target = static_cast<uint16_t>(ReadByte(s) * 32);
                        ch.vibrato.rate_delay = static_cast<uint16_t>(ReadByte(s) * 16);
                        break;

                    case 0xe3:
                        ch.vibrato.delay = static_cast<uint16_t>(ReadByte(s) * 16);
                        break;

                    case 0xd4:
                        ch.reverb = ReadByte(s);
                        break;

                    case 0xc6:
                        if (const std::optional<uint8_t> bank = BankFromMap(ReadByte(s)))
                        {
                            ch.bank = *bank;
                        }
                        break;

                    case 0xc7:
                        {
                            const uint8_t add = ReadByte(s);
                            const uint16_t at = static_cast<uint16_t>(ReadWord(s));
                            WriteData(at, static_cast<uint8_t>(s.value + add));
                            break;
                        }

                    case 0xc8:
                        s.value = static_cast<int8_t>(s.value - static_cast<int8_t>(ReadByte(s)));
                        break;

                    case 0xcc:
                        s.value = static_cast<int8_t>(ReadByte(s));
                        break;

                    case 0xc9:
                        s.value = static_cast<int8_t>(s.value & static_cast<int8_t>(ReadByte(s)));
                        break;

                    case 0xcd:
                        ReleaseChannel(ReadByte(s) & 0xf);
                        break;

                    case 0xca:
                        ReadByte(s);
                        break;

                    case 0xcb:
                        {
                            const uint16_t table = static_cast<uint16_t>(ReadWord(s));
                            s.value = static_cast<int8_t>(group_.data.at(static_cast<uint32_t>(table + s.value)));
                            break;
                        }

                    case 0xce:
                        ch.dynamic_value = static_cast<uint16_t>(ReadWord(s));
                        break;

                    case 0xcf:
                        {
                            const uint16_t at = static_cast<uint16_t>(ReadWord(s));
                            WriteData(at, static_cast<uint8_t>(ch.dynamic_value >> 8));
                            WriteData(at + 1u, static_cast<uint8_t>(ch.dynamic_value));
                            break;
                        }

                    case 0xd0:
                    case 0xd1:
                    case 0xd2:
                    case 0xd5:
                    case 0xd6:
                    case 0xe5:
                    case 0xe6:
                    case 0xe9:
                    case 0xed:
                    case 0xb3:
                    case 0xb9:
                    case 0xba:
                    case 0xa4:
                    case 0xa7:
                        if (command == 0xd2)
                        {
                            approximations_["a channel's sustain left out"]++;
                        }
                        if (command == 0xb9)
                        {
                            ch.velocity_variance = ReadByte(s);
                            break;
                        }
                        if (command == 0xa7)
                        {
                            // The value's shift: a bit for the direction, one to keep the sign, then the count.
                            const uint8_t arg = ReadByte(s);
                            const int shift = arg & 0xf;
                            const uint8_t value = static_cast<uint8_t>(s.value);
                            uint8_t result = (arg & 0x80) == 0 ? static_cast<uint8_t>(value << shift)
                                                               : static_cast<uint8_t>(value >> shift);
                            if (arg & 0x40)
                            {
                                result = static_cast<uint8_t>((result & 0x7f) | (value & 0x80));
                            }
                            s.value = static_cast<int8_t>(result);
                            break;
                        }
                        ReadByte(s);
                        break;

                    case 0xe7:
                        {
                            const uint16_t at = static_cast<uint16_t>(ReadWord(s));
                            ch.transposition = static_cast<int8_t>(group_.data.at(at + 3u));
                            ch.pan = group_.data.at(at + 4u);
                            ch.pan_weight = group_.data.at(at + 5u);
                            ch.reverb = group_.data.at(at + 6u);
                            break;
                        }

                    case 0xe8:
                        ReadByte(s);
                        ReadByte(s);
                        ReadByte(s);
                        ch.transposition = static_cast<int8_t>(ReadByte(s));
                        ch.pan = ReadByte(s);
                        ch.pan_weight = ReadByte(s);
                        ch.reverb = ReadByte(s);
                        ReadByte(s);
                        break;

                    case 0xec:
                        ch.frequency_scale = 1.0f;
                        ch.vibrato = Vibrato{0, 0, 0, 0, 0, 0, ch.vibrato.delay};
                        ch.velocity_variance = 0;
                        break;

                    case 0xe4:
                        if (s.value != -1 && s.depth < kDepth)
                        {
                            const std::size_t at = ch.dynamic_table + 2u * static_cast<uint8_t>(s.value);
                            s.stack[s.depth++] = s.pc;
                            s.pc = static_cast<uint32_t>(group_.data.at(at) << 8 | group_.data.at(at + 1));
                        }
                        break;

                    case 0xb0:
                    case 0xb2:
                    case 0xbd:
                    case 0xbc:
                    case 0xb7:
                    case 0xa0:
                    case 0xa2:
                        {
                            const uint16_t arg = static_cast<uint16_t>(ReadWord(s));
                            if (command == 0xb2)
                            {
                                const std::size_t at = arg + 2u * static_cast<uint8_t>(s.value);
                                ch.dynamic_value =
                                    static_cast<uint16_t>(group_.data.at(at) << 8 | group_.data.at(at + 1));
                            }
                            else if (command == 0xbc)
                            {
                                ch.dynamic_value = static_cast<uint16_t>(ch.dynamic_value + arg);
                            }
                            else if (command == 0xb7)
                            {
                                random_ = random_ * 1103515245u + 12345u;
                                const uint32_t r = random_ >> 16;
                                ch.dynamic_value = static_cast<uint16_t>(arg == 0 ? r : r % arg);
                            }
                            break;
                        }

                    case 0xb8:
                        {
                            const uint8_t range = ReadByte(s);
                            random_ = random_ * 1103515245u + 12345u;
                            const uint32_t r = random_ >> 16;
                            s.value = static_cast<int8_t>(range == 0 ? r : r % range);
                            approximations_["random choices made by a fixed generator"]++;
                            break;
                        }

                    case 0xb4:
                        ch.dynamic_table = ch.dynamic_value;
                        break;

                    case 0xb5:
                        {
                            const std::size_t at = ch.dynamic_table + 2u * static_cast<uint8_t>(s.value);
                            ch.dynamic_value = static_cast<uint16_t>(group_.data.at(at) << 8 | group_.data.at(at + 1));
                            break;
                        }

                    case 0xb6:
                        s.value = static_cast<int8_t>(group_.data.at(ch.dynamic_table + static_cast<uint8_t>(s.value)));
                        break;

                    case 0xbb:
                        ReadByte(s);
                        ReadWord(s);
                        break;

                    case 0xef:
                        ReadWord(s);
                        ReadByte(s);
                        break;

                    case 0xbe:
                        ReadByte(s);
                        approximations_["the game's callbacks from a sequence left out"]++;
                        break;

                    case 0xa5:
                        s.value = static_cast<int8_t>(s.value + static_cast<int8_t>(c));
                        break;

                    case 0xa6:
                        {
                            const uint8_t add = ReadByte(s);
                            const uint16_t at = static_cast<uint16_t>(ReadWord(s));
                            WriteData(at + c, static_cast<uint8_t>(s.value + add));
                            break;
                        }

                    case 0xa8:
                        ch.dynamic_value = static_cast<uint16_t>((ch.dynamic_value & 0x7f) | 0x8000 |
                                                                 ((ch.dynamic_value & 0x7fc0) << 1));
                        break;

                    default:
                        break;
                    }
                    continue;
                }

                if (command >= 0x70)
                {
                    const uint8_t kind = command & 0xf8;
                    std::size_t l = command & 7;
                    if (kind != 0x70 && l >= kLayers)
                    {
                        l = 0;
                    }
                    switch (kind)
                    {
                    case 0x80:
                        s.value = ch.layers[l] ? (ch.layers[l]->finished ? 1 : 0) : -1;
                        break;

                    case 0x88:
                        StartLayer(c, l, static_cast<uint16_t>(ReadWord(s)));
                        break;

                    case 0x90:
                        CloseLayer(c, l);
                        break;

                    case 0x98:
                        if (s.value != -1)
                        {
                            const std::size_t at = ch.dynamic_table + 2u * static_cast<uint8_t>(s.value);
                            StartLayer(c, l, static_cast<uint32_t>(group_.data.at(at) << 8 | group_.data.at(at + 1)));
                        }
                        break;

                    case 0x70:
                        ch.ports[l] = s.value;
                        break;

                    case 0x78:
                        {
                            const int16_t offset = ReadWord(s);
                            StartLayer(c, l, static_cast<uint32_t>(static_cast<int64_t>(s.pc) + offset));
                            break;
                        }

                    default:
                        break;
                    }
                    continue;
                }

                const std::size_t low = command & 0xf;
                switch (command & 0xf0)
                {
                case 0x00:
                    ch.delay = static_cast<int16_t>(low);
                    waiting = low != 0;
                    break;

                case 0x10:
                    ch.ports[low & 7] = -1;
                    approximations_["sample loads left out"]++;
                    break;

                case 0x60:
                    s.value = ch.ports[low & 7];
                    if ((low & 7) < 2)
                    {
                        ch.ports[low & 7] = -1;
                    }
                    break;

                case 0x50:
                    s.value = static_cast<int8_t>(s.value - ch.ports[low & 7]);
                    break;

                case 0x20:
                    OpenChannel(low, static_cast<uint16_t>(ReadWord(s)));
                    break;

                case 0x30:
                    {
                        const uint8_t port = ReadByte(s);
                        group_.channels[low].ports[port & 7] = s.value;
                        break;
                    }

                case 0x40:
                    {
                        const uint8_t port = ReadByte(s);
                        s.value = group_.channels[low].ports[port & 7];
                        break;
                    }

                default:
                    break;
                }
            }
        }
    }

    for (std::size_t l = 0; l < kLayers; l++)
    {
        if (group_.channels[c].enabled && group_.channels[c].layers[l])
        {
            LayerTick(c, l);
        }
    }
}

void Performer::LayerTick(std::size_t c, std::size_t l)
{
    Layer& layer = *group_.channels[c].layers[l];
    if (layer.finished)
    {
        return;
    }
    if (layer.delay > 1)
    {
        layer.delay--;
        if (!layer.muted && layer.delay <= layer.gate_delay)
        {
            Release(c, l);
            layer.muted = true;
        }
        return;
    }

    // The note before ends, unless the layer plays legato. A sweep of modes 1 and 2 sweeps one note.
    if (!layer.continuous)
    {
        Release(c, l);
    }
    const int sweep = layer.portamento.mode & 0x7f;
    if (sweep == 1 || sweep == 2)
    {
        layer.portamento.mode = 0;
    }

    int semitone = kScriptEnd;
    for (;;)
    {
        const int command = LayerCommands(c, l);
        if (command == kScriptEnd)
        {
            return;
        }
        semitone = SetNote(c, l, command);
        if (semitone != kScriptEnd || group_.channels[c].layers[l]->delay != 0)
        {
            break;
        }
    }

    Layer& now = *group_.channels[c].layers[l];
    if (semitone != kScriptEnd)
    {
        SetVoice(c, l, semitone);
    }
    if (now.muted)
    {
        Release(c, l);
    }
}

int Performer::LayerCommands(std::size_t c, std::size_t l)
{
    Channel& ch = group_.channels[c];
    Layer& layer = *ch.layers[l];
    Script& s = layer.script;
    for (;;)
    {
        const uint8_t command = ReadByte(s);
        if (command <= 0xc0)
        {
            return command;
        }
        if (command >= 0xf2)
        {
            if (Common(s, command, ArgOf(s, command)) == 0)
            {
                continue;
            }

            // A wait or the script's end stops the layer.
            Release(c, l);
            layer.finished = true;
            return kScriptEnd;
        }

        switch (command)
        {
        case 0xc1:
            {
                const int velocity = ReadByte(s);
                layer.velocity = static_cast<float>(velocity * velocity) / static_cast<float>(127 * 127);
                break;
            }

        case 0xca:
            layer.pan = ReadByte(s);
            break;

        case 0xc2:
            layer.transposition = ReadByte(s);
            break;

        case 0xc9:
            layer.gate_time = ReadByte(s);
            break;

        case 0xc4:
        case 0xc5:
            layer.continuous = command == 0xc4;
            Release(c, l);
            break;

        case 0xc3:
            layer.short_delay = static_cast<int16_t>(ReadLength(s));
            break;

        case 0xc6:
            {
                const uint8_t program = ReadByte(s);
                if (program >= 0x7e)
                {
                    layer.instrument = program == 0x7e ? kEffectsProgram : program == 0x7f ? kDrumsProgram : 0xff;
                    if (program == 0xff)
                    {
                        layer.adsr.release = 0;
                    }
                    if (program >= 0x80 && program != 0xff)
                    {
                        approximations_["synthesized waves left out"]++;
                    }
                    break;
                }

                // The instrument's envelope and release become the layer's.
                const Bank* bank = ch.bank != 0xff ? data_.GetBank(ch.bank) : nullptr;
                if (bank != nullptr && program < bank->instruments.size() && bank->instruments[program])
                {
                    layer.adsr = {bank->instruments[program]->envelope, bank->instruments[program]->release};
                    layer.instrument = program;
                }
                else
                {
                    layer.instrument = 0xff;
                }
                break;
            }

        case 0xc7:
            {
                layer.portamento.mode = ReadByte(s);
                const int target = (ReadByte(s) + ch.transposition + layer.transposition + group_.transposition) & 0xff;
                layer.portamento.target = target >= 0x80 ? 0 : target;
                layer.portamento.time = (layer.portamento.mode & 0x80) != 0 ? ReadByte(s) : ReadLength(s);
                break;
            }

        case 0xc8:
            layer.portamento = Portamento{};
            break;

        case 0xcb:
            layer.adsr.envelope = ReadEnvelope(group_.data, static_cast<uint16_t>(ReadWord(s)));
            layer.adsr.release = ReadByte(s);
            break;

        case 0xcf:
            layer.adsr.release = ReadByte(s);
            break;

        case 0xcc:
            layer.ignore_drum_pan = true;
            break;

        case 0xcd:
            ReadByte(s);
            break;

        case 0xce:
            layer.bend = Bend(data_.SmallCents(), static_cast<int8_t>(ReadByte(s)));
            break;

        case 0xf0:
            ReadWord(s);
            approximations_["a layer's flags left out"]++;
            break;

        case 0xf1:
            ReadByte(s);
            break;

        default:
            if ((command & 0xf0) == 0xd0)
            {
                const std::size_t i = command & 0xf;
                const int velocity = group_.short_velocities < 0
                                         ? data_.ShortVelocities()[i]
                                         : group_.data.at(static_cast<std::size_t>(group_.short_velocities) + i);
                layer.velocity = static_cast<float>(velocity * velocity) / static_cast<float>(127 * 127);
            }
            else if ((command & 0xf0) == 0xe0)
            {
                const std::size_t i = command & 0xf;
                layer.gate_time = group_.short_gate_times < 0
                                      ? data_.ShortGateTimes()[i]
                                      : group_.data.at(static_cast<std::size_t>(group_.short_gate_times) + i);
            }
            break;
        }
    }
}

int Performer::SetNote(std::size_t c, std::size_t l, int command)
{
    Channel& ch = group_.channels[c];
    Layer& layer = *ch.layers[l];
    Script& s = layer.script;
    if (command == 0xc0)
    {
        layer.delay = static_cast<int16_t>(ReadLength(s));
        layer.muted = true;
        return kScriptEnd;
    }

    layer.muted = false;
    int16_t delay = 0;
    if (ch.large_notes)
    {
        int velocity = 127;
        switch (command & 0xc0)
        {
        case 0x00:
            delay = static_cast<int16_t>(ReadLength(s));
            velocity = ReadByte(s);
            layer.gate_time = ReadByte(s);
            layer.last_delay = delay;
            break;

        case 0x40:
            delay = static_cast<int16_t>(ReadLength(s));
            velocity = ReadByte(s);
            layer.gate_time = 0;
            layer.last_delay = delay;
            break;

        default:
            delay = layer.last_delay;
            velocity = ReadByte(s);
            layer.gate_time = ReadByte(s);
            break;
        }
        if (velocity > 127)
        {
            velocity = 127;
        }
        layer.velocity = static_cast<float>(velocity * velocity) / static_cast<float>(127 * 127);
    }
    else
    {
        switch (command & 0xc0)
        {
        case 0x00:
            delay = static_cast<int16_t>(ReadLength(s));
            layer.last_delay = delay;
            break;

        case 0x40:
            delay = layer.short_delay;
            break;

        default:
            delay = layer.last_delay;
            break;
        }
    }
    if (ch.velocity_variance != 0)
    {
        approximations_["a random variance of velocities left out"]++;
    }

    layer.delay = delay;
    layer.gate_delay = static_cast<int16_t>((layer.gate_time * delay) >> 8);

    return command & 0x3f;
}

void Performer::SetVoice(std::size_t c, std::size_t l, int note)
{
    Channel& ch = group_.channels[c];
    Layer& layer = *ch.layers[l];
    int instrument = layer.instrument;
    if (instrument == 0xff)
    {
        if (ch.instrument == kNoInstrument)
        {
            layer.muted = true;
            return;
        }
        instrument = ch.instrument;
    }

    const Bank* bank = ch.bank != 0xff ? data_.GetBank(ch.bank) : nullptr;
    PresetVoice voice;
    voice.bank = ch.bank;
    int key = 0;
    const Sample* sample = nullptr;
    float tuning = 1.0f;
    int pitch_index = 0;
    Adsr adsr = layer.adsr;

    if (instrument == kDrumsProgram)
    {
        const int drum = static_cast<uint8_t>(note + layer.transposition + ch.transposition);
        if (bank == nullptr || drum >= static_cast<int>(bank->drums.size()) ||
            !bank->drums[static_cast<std::size_t>(drum)] || ch.muted)
        {
            layer.muted = true;
            return;
        }

        // A drum with a release takes its envelope and release. The preset keeps the channel's for the others.
        const Drum& d = *bank->drums[static_cast<std::size_t>(drum)];
        voice.kind = VoiceKind::kDrums;
        const bool any_without = std::any_of(bank->drums.begin(), bank->drums.end(),
                                             [](const std::optional<Drum>& dr) { return dr && dr->release == 0; });
        if (any_without)
        {
            voice.envelope = ch.adsr.envelope;
            voice.release = ch.adsr.release;
        }
        if (!layer.ignore_drum_pan)
        {
            layer.pan = d.pan;
        }
        key = drum;
        layer.portamento.extent = 0.0;
        sample = d.sample.sample;
        tuning = d.sample.tuning;
    }
    else if (instrument == kEffectsProgram)
    {
        const uint32_t number = static_cast<uint32_t>(note + (layer.transposition << 6)) & 0xffff;
        if (bank == nullptr || number >= bank->effects.size() || bank->effects[number].sample == nullptr || ch.muted)
        {
            layer.muted = true;
            return;
        }

        voice.kind = VoiceKind::kEffects;
        voice.program = static_cast<uint8_t>(number >> 7);
        const bool has_release = adsr.release != 0;
        voice.envelope = has_release ? adsr.envelope : ch.adsr.envelope;
        voice.release = has_release ? adsr.release : ch.adsr.release;
        key = EffectKey(number);
        layer.portamento.extent = 0.0;
        sample = bank->effects[number].sample;
        tuning = bank->effects[number].tuning;
    }
    else
    {
        const int semitone = static_cast<uint8_t>(note + group_.transposition + layer.transposition + ch.transposition);
        if (semitone > 127 || ch.muted)
        {
            layer.muted = true;
            return;
        }
        if (bank == nullptr || instrument >= static_cast<int>(bank->instruments.size()) ||
            !bank->instruments[static_cast<std::size_t>(instrument)])
        {
            layer.muted = true;
            return;
        }

        const Instrument& inst = *bank->instruments[static_cast<std::size_t>(instrument)];
        voice.kind = VoiceKind::kInstrument;
        voice.program = static_cast<uint8_t>(instrument);
        const bool has_release = layer.adsr.release != 0;
        voice.envelope = has_release ? layer.adsr.envelope : ch.adsr.envelope;
        voice.release = has_release ? layer.adsr.release : ch.adsr.release;
        const TunedSample& ts = semitone < inst.low     ? inst.low_sample
                                : semitone <= inst.high ? inst.normal_sample
                                                        : inst.high_sample;
        sample = ts.sample;
        tuning = ts.tuning;
        pitch_index = semitone;

        // A sweep starts at one of its two notes. A bend takes it to the other.
        int start = semitone;
        Portamento& sweep = layer.portamento;
        if (sweep.mode != 0)
        {
            const int mode = sweep.mode & 0x7f;
            const double to_note = std::pow(2.0, (semitone - sweep.target) / 12.0);
            if (mode == 1 || mode == 3 || mode == 5)
            {
                start = sweep.target;
                sweep.extent = to_note - 1.0;
            }
            else
            {
                sweep.extent = 1.0 / to_note - 1.0;
            }
            sweep.steps = 0.0;
            if (mode == 5)
            {
                sweep.target = semitone;
            }
        }
        pitch_index = start;
        key = KeyOf(start);
        if (key > 127)
        {
            approximations_["notes above MIDI's highest key played at it"]++;
            key = 127;
        }
    }

    if (sample == nullptr)
    {
        layer.muted = true;
        return;
    }

    // A note of no length lasts as long as its sample plays at the tempo.
    if (layer.delay == 0)
    {
        const double length = static_cast<double>(sample->pcm.Length());
        const double pitch = instrument == kDrumsProgram || instrument == kEffectsProgram
                                 ? tuning
                                 : tuning * data_.Pitches()[static_cast<std::size_t>(pitch_index)];
        const double scale = (60.0 * 4.0 / 48000.0) / kMaxTempo;
        double time = length * (group_.tempo) * scale / (pitch * layer.bend);
        time = std::min(time, 32766.0);
        layer.delay = static_cast<int16_t>(static_cast<int>(time) + 1);
        layer.gate_delay = 0;
    }

    if (layer.bend != 1.0f)
    {
        approximations_["a layer's bend left out"]++;
    }

    // A sweep's steps an update. A special sweep takes the share `time` / 256 of the note's length.
    Portamento& sweep = layer.portamento;
    if (sweep.mode != 0)
    {
        int speed = 0x7fff;
        if ((sweep.mode & 0x80) != 0)
        {
            const int tempo = std::min(group_.tempo + group_.tempo_change, kMaxTempo);
            speed = tempo * 0x8000 / kMaxTempo;
            if (layer.delay * sweep.time != 0)
            {
                speed = speed * 0x100 / (layer.delay * sweep.time);
            }
        }
        else
        {
            const int denominator = sweep.time * kUpdatesPerFrame / 3;
            speed = denominator != 0 ? 0x20000 / denominator : 0x7fff;
        }
        sweep.rate = std::clamp(speed, 1, 0x7fff) / 256.0;
    }

    // Two notes of a channel that sound together share one pitch. When either glides, the other moves with it.
    const bool glides = voice.kind == VoiceKind::kInstrument &&
                        ((layer.continuous && layer.note != 0) || (sweep.mode != 0 && sweep.extent != 0.0));
    for (std::size_t other = 0; other < kLayers; other++)
    {
        const std::optional<Layer>& o = ch.layers[other];
        if (other != l && o && o->note != 0 &&
            (glides || o->offset != 0.0f || (o->portamento.mode != 0 && o->portamento.extent != 0.0)))
        {
            approximations_["notes that share their channel with a layer's glide"]++;
            break;
        }
    }

    // A legato layer keeps its note while the sample stays the same. Only the pitch moves. The model moves an
    // instrument's notes only.
    if (layer.note != 0)
    {
        if (layer.continuous && layer.sample == sample && voice.kind == VoiceKind::kInstrument)
        {
            layer.offset = static_cast<float>(key - layer.key);
            return;
        }
        if (layer.continuous)
        {
            approximations_["legato notes played as separate notes"]++;
        }
        Release(c, l);
    }

    voice.vibrato_rate = ch.vibrato.rate_delay == 0 ? ch.vibrato.rate_target : ch.vibrato.rate_start;
    voice.vibrato_delay = ch.vibrato.delay;
    voice.pan_weight = std::min<uint8_t>(ch.pan_weight, 128);
    if (voice.pan_weight < 128 && voice.kind != VoiceKind::kDrums)
    {
        voice.note_pan = layer.pan;
    }

    // The note: its preset, then its velocity, which squared gives its level.
    const uint32_t preset = PresetOf(voice);
    recorder_.SendPreset(out_.performance, outputs_[c], tick_, preset);
    const int velocity = std::clamp(static_cast<int>(std::lround(127.0 * std::sqrt(layer.velocity))), 1, 127);
    layer.key = static_cast<uint8_t>(key);
    layer.sample = sample;
    layer.offset = 0.0f;
    layer.note = recorder_.NoteOn(out_.performance, outputs_[c], tick_, layer.key, static_cast<uint8_t>(velocity));
}

uint32_t Performer::PresetOf(const PresetVoice& voice)
{
    const auto found = std::find(out_.voices.begin(), out_.voices.end(), voice);
    if (found != out_.voices.end())
    {
        return static_cast<uint32_t>(found - out_.voices.begin());
    }

    out_.voices.push_back(voice);
    PresetKey key;
    key.bank = static_cast<uint32_t>(voice.kind) << 8 | voice.bank;
    key.program = voice.program;
    out_.performance.presets.push_back(key);

    return static_cast<uint32_t>(out_.voices.size() - 1);
}

double Performer::TailSeconds()
{
    double frames = 0.0;
    for (const PresetVoice& voice : out_.voices)
    {
        frames = std::max(frames, ReleaseFrames(voice.release));
        const Bank* bank = voice.kind == VoiceKind::kDrums ? data_.GetBank(voice.bank) : nullptr;
        if (bank != nullptr)
        {
            for (const std::optional<Drum>& drum : bank->drums)
            {
                if (drum)
                {
                    frames = std::max(frames, ReleaseFrames(drum->release));
                }
            }
        }
    }

    return frames / kFramesPerSecond;
}

void Performer::Update()
{
    for (Channel& ch : group_.channels)
    {
        for (std::optional<Layer>& layer : ch.layers)
        {
            if (layer && layer->note != 0 && layer->portamento.mode != 0 && layer->portamento.steps < kSweepSteps)
            {
                layer->portamento.steps += layer->portamento.rate;
            }
        }
    }

    if (group_.fade_timer != 0)
    {
        group_.fade_volume = std::clamp(group_.fade_volume + group_.fade_velocity, 0.0f, 1.0f);
        if (--group_.fade_timer == 0 && group_.state == 2)
        {
            for (std::size_t c = 0; c < kChannels; c++)
            {
                ReleaseChannel(c);
            }
            group_.enabled = false;
        }
    }
}

double Performer::Level(const Channel& ch) const
{
    return static_cast<double>(ch.volume) * ch.volume_scale * group_.fade_volume * group_.fade_volume_scale;
}

void Performer::SendControllers()
{
    for (std::size_t c = 0; c < kChannels; c++)
    {
        const Channel& ch = group_.channels[c];
        if (!ch.enabled)
        {
            continue;
        }

        TrackOutput& out = outputs_[c];
        const double level = Level(ch);
        peak_level_ = std::max(peak_level_, level);
        recorder_.SendSetting(out_.performance, out, tick_, EventKind::kVolume, 127);
        recorder_.SendSetting(out_.performance, out, tick_, EventKind::kExpression,
                              static_cast<uint8_t>(std::clamp(std::lround(127.0 * level * level_scale_), 0L, 127L)));

        // The library's pans run from 0 to 127 on a sine law. RP-036 puts 1 to 127 on the same law.
        const uint8_t pan = piece_.centre_pans ? kCentre : ch.pan;
        const double weighted = (static_cast<double>(pan) * ch.pan_weight + kCentre * (128.0 - ch.pan_weight)) / 128.0;
        recorder_.SendSetting(out_.performance, out, tick_, EventKind::kPan,
                              static_cast<uint8_t>(std::lround(1.0 + weighted * 126.0 / 127.0)));
        recorder_.SendSetting(out_.performance, out, tick_, EventKind::kReverb, std::min<uint8_t>(ch.reverb, 127));

        // The vibrato's depth, as the peak of its swing in cents.
        const double depth_cents = 1200.0 * std::log2(1.0 + ch.vibrato.depth_target / 4096.0);
        recorder_.SendSetting(
            out_.performance, out, tick_, EventKind::kModulation,
            static_cast<uint8_t>(std::clamp(std::lround(127.0 * depth_cents / kVibratoRangeCents), 0L, 127L)));
        recorder_.SendPitch(out_.performance, out, tick_,
                            static_cast<float>(12.0 * std::log2(ch.frequency_scale)) + LayerPitch(c));
    }
}

float Performer::LayerPitch(std::size_t c)
{
    // The first layer whose note moves apart gives the pitch. A note that sounds with it moves with it (SetVoice counts
    // those notes).
    Channel& ch = group_.channels[c];
    bool sounding = false;
    bool found = false;
    for (const std::optional<Layer>& layer : ch.layers)
    {
        if (!layer || layer->note == 0)
        {
            continue;
        }
        float pitch = layer->offset;
        const Portamento& sweep = layer->portamento;
        if (sweep.mode != 0 && sweep.extent != 0.0)
        {
            const double steps = std::min(sweep.steps, kSweepSteps);
            const double scale = 1.0 + sweep.extent * (std::pow(2.0, steps / kSweepSteps) - 1.0);
            pitch += static_cast<float>(12.0 * std::log2(std::max(scale, 1.0 / 65536.0)));
        }
        if (pitch != 0.0f && !found)
        {
            ch.layer_pitch = pitch;
            found = true;
        }
        sounding = true;
    }
    if (sounding && !found)
    {
        ch.layer_pitch = 0.0f;
    }

    return ch.layer_pitch;
}

void Performer::WriteData(std::size_t at, uint8_t value)
{
    group_.data.at(at) = value;
    data_hash_.reset();
}

uint64_t Performer::Signature() const
{
    // Every field that the scripts change, the sequence's data included. The tempo's accumulator only moves where the
    // ticks fall, by less than an update.
    std::string s;
    const auto add = [&s](const void* p, std::size_t n)
    {
        s.append(static_cast<const char*>(p), n);
    };
    const auto add_script = [&add](const Script& sc)
    {
        add(&sc.pc, sizeof sc.pc);
        add(sc.stack.data(), sizeof(uint32_t) * sc.depth);
        add(sc.loops.data(), sc.depth);
        add(&sc.depth, 1);
        add(&sc.value, 1);
    };
    if (!data_hash_)
    {
        data_hash_ = Fnv1a(group_.data);
    }
    add(&*data_hash_, sizeof *data_hash_);
    add_script(group_.script);
    add(&group_.delay, sizeof group_.delay);
    add(&group_.tempo, sizeof group_.tempo);
    add(&group_.tempo_change, sizeof group_.tempo_change);
    add(&group_.transposition, sizeof group_.transposition);
    add(&group_.fade_volume, sizeof group_.fade_volume);
    add(&group_.fade_timer, sizeof group_.fade_timer);
    add(&group_.fade_volume_scale, sizeof group_.fade_volume_scale);
    add(group_.ports.data(), group_.ports.size());
    for (const Channel& ch : group_.channels)
    {
        add(&ch.enabled, 1);
        if (!ch.enabled)
        {
            continue;
        }
        add_script(ch.script);
        add(&ch.delay, sizeof ch.delay);
        add(&ch.bank, 1);
        add(&ch.instrument, sizeof ch.instrument);
        add(&ch.volume, sizeof ch.volume);
        add(&ch.volume_scale, sizeof ch.volume_scale);
        add(&ch.frequency_scale, sizeof ch.frequency_scale);
        add(&ch.pan, 1);
        add(&ch.pan_weight, 1);
        add(&ch.transposition, 1);
        add(&ch.reverb, 1);
        add(&ch.vibrato, sizeof ch.vibrato);
        add(ch.ports.data(), ch.ports.size());
        add(&ch.dynamic_value, sizeof ch.dynamic_value);
        add(&ch.dynamic_table, sizeof ch.dynamic_table);
        add(&ch.large_notes, 1);
        add(&ch.stop_script, 1);
        for (const EnvelopePoint& p : ch.adsr.envelope)
        {
            add(&p, sizeof p);
        }
        add(&ch.adsr.release, 1);
        add(&ch.layer_pitch, sizeof ch.layer_pitch);
        for (const std::optional<Layer>& layer : ch.layers)
        {
            const bool present = layer.has_value();
            add(&present, 1);
            if (!present)
            {
                continue;
            }
            add_script(layer->script);
            add(&layer->finished, 1);
            add(&layer->muted, 1);
            add(&layer->delay, sizeof layer->delay);
            add(&layer->gate_delay, sizeof layer->gate_delay);
            add(&layer->last_delay, sizeof layer->last_delay);
            add(&layer->short_delay, sizeof layer->short_delay);
            add(&layer->gate_time, 1);
            add(&layer->velocity, sizeof layer->velocity);
            add(&layer->pan, 1);
            add(&layer->transposition, sizeof layer->transposition);
            add(&layer->instrument, sizeof layer->instrument);
            add(&layer->adsr.release, 1);
            add(&layer->continuous, 1);
            add(&layer->portamento.mode, 1);
            add(&layer->portamento.target, sizeof layer->portamento.target);
            add(&layer->portamento.time, sizeof layer->portamento.time);
            add(&layer->portamento.steps, sizeof layer->portamento.steps);
            add(&layer->portamento.extent, sizeof layer->portamento.extent);
            add(&layer->offset, sizeof layer->offset);
            if (layer->continuous)
            {
                const bool sounds = layer->note != 0;
                add(&sounds, 1);
            }
        }
    }

    return Fnv1a(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

Played Performer::Run()
{
    // The game starts the sequence on its player: the player takes the sequence's last bank. The game's mutes and
    // ports then apply before the first tick.
    group_.sequence = piece_.sequence;
    const std::span<const uint8_t> sequence = data_.Sequence(piece_.sequence);
    group_.data.assign(sequence.begin(), sequence.end());
    const std::vector<uint8_t> banks = data_.BanksOf(piece_.sequence);
    if (!banks.empty())
    {
        group_.bank = data_.ResolveBank(banks.back()).value_or(0xff);
    }
    group_.enabled = true;
    group_.fade_volume_scale = piece_.volume;
    for (const auto& [port, value] : piece_.ports)
    {
        group_.ports[port & 7] = value;
    }
    group_.channels.fill(Channel{});
    for (Channel& ch : group_.channels)
    {
        ch.adsr = {data_.DefaultEnvelope(), 240};
    }

    // Ticks run until the state repeats, the sequence ends or the limit passes.
    std::unordered_map<uint64_t, uint32_t> seen;
    std::optional<std::pair<uint32_t, uint32_t>> loop;
    double bpm = 0.0;
    const uint64_t limit = static_cast<uint64_t>(kLimitSeconds * kUpdatesPerSecond);
    for (std::size_t c = 0; c < kChannels; c++)
    {
        group_.channels[c].muted = (piece_.muted & (1u << c)) != 0;
    }
    while (group_.enabled && update_ < limit)
    {
        const int tempo = std::min(group_.tempo + group_.tempo_change, kMaxTempo);
        tempo_accumulator_ += tempo;
        update_++;
        if (tempo_accumulator_ < kMaxTempo)
        {
            Update();
            continue;
        }
        tempo_accumulator_ -= kMaxTempo;

        if (!loop)
        {
            const auto [found, inserted] = seen.emplace(Signature(), tick_);
            if (!inserted)
            {
                loop = {found->second, tick_};
                if (options_.loops <= 1)
                {
                    break;
                }
            }
        }
        if (loop && tick_ >= loop->second + (loop->second - loop->first) * static_cast<uint32_t>(options_.loops - 1))
        {
            break;
        }

        // The MIDI tempo: the ticks a minute at the player's tempo, over the tatums of a beat.
        const double now = tempo * kUpdatesPerSecond * 60.0 / kMaxTempo / kTatumsPerBeat;
        if (now != bpm)
        {
            recorder_.SetTempo(out_.performance, tick_, now);
            bpm = now;
        }

        GroupTick();
        Update();
        SendControllers();
        tick_++;
    }

    // Every note still sounding ends at the last tick.
    for (std::size_t c = 0; c < kChannels; c++)
    {
        for (std::size_t l = 0; l < kLayers; l++)
        {
            Release(c, l);
        }
    }

    Performance& perf = out_.performance;
    perf.end_tick = tick_;
    if (loop)
    {
        perf.loop = seqlib::midi::Performance::Loop{loop->first, loop->second};
        perf.loops = options_.loops;
    }
    else if (update_ >= limit)
    {
        perf.truncated = true;
    }
    perf.seconds = static_cast<double>(update_) / kUpdatesPerSecond + TailSeconds();
    for (const auto& [what, count] : approximations_)
    {
        perf.approximations[what] += count;
    }
    perf.warnings = warnings_;
    out_.performance.approximations.insert(recorder_.Approximations().begin(), recorder_.Approximations().end());

    return std::move(out_);
}

} // namespace

Played PlayPiece(const Piece& piece, SoundData& data, const PerformOptions& options)
{
    // The first run finds the loudest level of a channel. If CC11 can't hold it, a second run scales every level to
    // fit, and the performance records the cut.
    Performer first(piece, data, options, 1.0);
    Played played = first.Run();
    const double peak = first.PeakLevel();
    if (ntdlib::sequence_export::LevelScale(peak) < 1.0)
    {
        Performer second(piece, data, options, ntdlib::sequence_export::LevelScale(peak));
        played = second.Run();
        ntdlib::sequence_export::SetLevelCut(played.performance, peak);
    }

    return played;
}

} // namespace dolphinsf2::n64
