// SPDX-License-Identifier: MIT

// The sequence player of the Nintendo 64's sound library (see n64_player.h).
//
// The model follows the library's build in Animal Crossing's main.dol: the sequence player's script (Nas_GroupSeq),
// the channels' (Nas_SubSeq) and the layers' (__Command_Seq, __SetNote and __SetVoice), and the update that steps the
// fades (Nas_MainCtrl). A tick runs when the player's tempo, added up once an update, passes the library's maximum. The
// voices aren't modelled. Their envelopes, releases and vibratos go to the SoundFont.

#include "n64_player.h"

#include <algorithm>
#include <utility>

#include "n64_player_parts.h"

namespace dolphinsf2::n64::player
{

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

uint8_t Performer::DataByte(std::size_t at) const
{
    if (at >= group_.data.size())
    {
        throw FormatError("a script reads past the end of sequence " + std::to_string(group_.sequence));
    }

    return group_.data[at];
}

uint16_t Performer::DataWord(std::size_t at) const
{
    const uint8_t high = DataByte(at);
    return static_cast<uint16_t>(high << 8 | DataByte(at + 1));
}

uint32_t Performer::NextRandom()
{
    approximations_["random choices made by a fixed generator"]++;
    random_ = random_ * 1103515245u + 12345u;

    return random_ >> 16;
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

void Performer::EndGroup()
{
    for (std::size_t c = 0; c < kChannels; c++)
    {
        ReleaseChannel(c);
    }
    group_.enabled = false;
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
        // A script that ends the player or stops itself ends the tick before the channels' ticks.
        GroupScript();
        if (group_.stop_script || !group_.enabled)
        {
            return;
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

void Performer::GroupScript()
{
    Script& s = group_.script;
    while (!group_.stop_script && group_.enabled)
    {
        const uint8_t command = ReadByte(s);
        if (command >= 0xf2)
        {
            const int wait = Common(s, command, ArgOf(s, command));
            if (wait == kScriptEnd)
            {
                EndGroup();
            }
            else if (wait != 0)
            {
                group_.delay = static_cast<int16_t>(wait);
                return;
            }
        }
        else if (command >= 0xc0)
        {
            if (!GroupSetting(command))
            {
                GroupFlow(command);
            }
        }
        else
        {
            GroupChannelCommand(command);
        }
    }
}

bool Performer::GroupSetting(uint8_t command)
{
    Script& s = group_.script;
    switch (command)
    {
    case 0xdf:
        group_.transposition = static_cast<int8_t>(ReadByte(s));
        break;

    case 0xde:
        group_.transposition = static_cast<int16_t>(group_.transposition + static_cast<int8_t>(ReadByte(s)));
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
            SetFadeState(state, time);
            break;
        }

    case 0xdb:
        SetFadeVolume(static_cast<float>(ReadByte(s)) / 127.0f);
        break;

    case 0xd9:
        group_.fade_volume_scale = static_cast<float>(static_cast<int8_t>(ReadByte(s))) / 127.0f;
        break;

    case 0xd7:
        {
            // The channels whose bit is set take the player's bank and mute settings. The files leave the mutes out.
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

    case 0xd4:
        approximations_["the sequence's mute of its channels left out"]++;
        break;

    case 0xd1:
        group_.short_gate_times = static_cast<uint16_t>(ReadWord(s));
        break;

    case 0xd2:
        group_.short_velocities = static_cast<uint16_t>(ReadWord(s));
        break;

    case 0xc3:
        {
            // The word of the table at the script value mutes the channels whose bits it sets.
            const uint16_t table = static_cast<uint16_t>(ReadWord(s));
            if (s.value != -1)
            {
                uint16_t mask = DataWord(table + 2u * static_cast<uint8_t>(s.value));
                for (std::size_t c = 0; c < kChannels; c++, mask >>= 1)
                {
                    group_.channels[c].muted = (mask & 1) != 0;
                }
            }
            break;
        }

    default:
        return false;
    }

    return true;
}

void Performer::SetFadeState(uint8_t state, uint16_t time)
{
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
}

void Performer::SetFadeVolume(float volume)
{
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
}

void Performer::GroupFlow(uint8_t command)
{
    Script& s = group_.script;
    switch (command)
    {
    case 0xce:
        {
            const uint8_t range = ReadByte(s);
            const uint32_t r = NextRandom();
            s.value = static_cast<int8_t>(range == 0 ? (r >> 2) & 0xff : (r >> 2) % range);
            break;
        }

    case 0xcd:
        {
            // A call through the word of the table at the script value. The library makes the call unless the depth is
            // kDepth - 1. At kDepth, the library would write past the stack. The player skips the call there as well.
            const uint16_t table = static_cast<uint16_t>(ReadWord(s));
            if (s.value != -1 && s.depth < kDepth - 1)
            {
                const std::size_t at = table + 2u * static_cast<uint8_t>(s.value);
                s.stack[s.depth++] = s.pc;
                s.pc = DataWord(at);
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
                s.pc = DataWord(table + 2u * static_cast<uint8_t>(s.value));
            }
            break;
        }

    case 0xc6:
        group_.stop_script = true;
        break;

    case 0xc4:
        {
            const uint8_t player = ReadByte(s);
            const uint8_t sequence = ReadByte(s);
            if (player != 0xff)
            {
                approximations_["a sequence that starts another in another player left out"]++;
                break;
            }

            // The script starts a sequence in its player. The player releases every note and the piece ends there. The
            // game gives Kapp'n's first parts a sequence that ends at once (Piece::end_sequence). Another sequence
            // would play on in the player.
            if (sequence != piece_.end_sequence)
            {
                approximations_["a sequence that starts another in its player"]++;
                warnings_.push_back("sequence " + std::to_string(group_.sequence) + " starts sequence " +
                                    std::to_string(sequence) + ", which plays no further");
            }
            EndGroup();
            break;
        }

    // The commands that the files leave out.
    case 0xf1:
    case 0xd5:
    case 0xd3:
    case 0xd0:
        ReadByte(s);
        break;

    case 0xd6:
    case 0xc5:
        ReadWord(s);
        break;

    case 0xef:
        ReadWord(s);
        ReadByte(s);
        break;

    default:
        break;
    }
}

void Performer::GroupChannelCommand(uint8_t command)
{
    Script& s = group_.script;
    if (command == 0xbe)
    {
        ReadByte(s);
        approximations_["the game's callbacks from a sequence left out"]++;
        return;
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

void Performer::ChannelTick(std::size_t c)
{
    Channel& ch = group_.channels[c];
    if (!ch.stop_script)
    {
        if (ch.delay > 1)
        {
            ch.delay--;
        }
        else if (!ChannelScript(c))
        {
            return;
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

bool Performer::ChannelScript(std::size_t c)
{
    Channel& ch = group_.channels[c];
    Script& s = ch.script;
    for (;;)
    {
        const uint8_t command = ReadByte(s);
        if (command >= 0xf2)
        {
            const int wait = Common(s, command, ArgOf(s, command));
            if (wait == kScriptEnd)
            {
                ReleaseChannel(c);
                return false;
            }
            if (wait != 0)
            {
                ch.delay = static_cast<int16_t>(wait);
                return true;
            }
        }
        else if (command >= 0xa0)
        {
            if (!ChannelSetting(c, command) && !ChannelVibrato(c, command))
            {
                ChannelFlow(c, command);
            }
            if (ch.stop_script)
            {
                return true;
            }
        }
        else if (command >= 0x70)
        {
            LayerControl(c, command);
        }
        else if (ChannelLowCommand(c, command))
        {
            return true;
        }
    }
}

bool Performer::ChannelSetting(std::size_t c, uint8_t command)
{
    Channel& ch = group_.channels[c];
    Script& s = ch.script;
    switch (command)
    {
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

    case 0xc6:
        if (const std::optional<uint8_t> bank = BankFromMap(ReadByte(s)))
        {
            ch.bank = *bank;
        }
        break;

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

    case 0xd4:
        ch.reverb = ReadByte(s);
        break;

    case 0xd2:
        ReadByte(s);
        approximations_["a channel's sustain left out"]++;
        break;

    case 0xb9:
        ch.velocity_variance = ReadByte(s);
        break;

    case 0xe7:
        {
            // The transposition, the pan, its weight and the reverb, from the sequence's data.
            const uint16_t at = static_cast<uint16_t>(ReadWord(s));
            ch.transposition = static_cast<int8_t>(DataByte(at + 3u));
            ch.pan = DataByte(at + 4u);
            ch.pan_weight = DataByte(at + 5u);
            ch.reverb = DataByte(at + 6u);
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

    default:
        return false;
    }

    return true;
}

bool Performer::ChannelVibrato(std::size_t c, uint8_t command)
{
    Channel& ch = group_.channels[c];
    Script& s = ch.script;
    switch (command)
    {
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

    default:
        return false;
    }

    return true;
}

void Performer::ChannelFlow(std::size_t c, uint8_t command)
{
    Channel& ch = group_.channels[c];
    Script& s = ch.script;
    switch (command)
    {
    case 0xea:
        ch.stop_script = true;
        break;

    case 0xc2:
        ch.dynamic_table = static_cast<uint16_t>(ReadWord(s));
        break;

    case 0xc5:
        if (s.value != -1)
        {
            s.pc = DataWord(ch.dynamic_table + 2u * static_cast<uint8_t>(s.value));
        }
        break;

    case 0xe4:
        if (s.value != -1 && s.depth < kDepth)
        {
            const std::size_t at = ch.dynamic_table + 2u * static_cast<uint8_t>(s.value);
            s.stack[s.depth++] = s.pc;
            s.pc = DataWord(at);
        }
        break;

    case 0xc7:
        {
            const uint8_t add = ReadByte(s);
            const uint16_t at = static_cast<uint16_t>(ReadWord(s));
            WriteData(at, static_cast<uint8_t>(s.value + add));
            break;
        }

    case 0xa6:
        {
            const uint8_t add = ReadByte(s);
            const uint16_t at = static_cast<uint16_t>(ReadWord(s));
            WriteData(at + c, static_cast<uint8_t>(s.value + add));
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

    case 0xa5:
        s.value = static_cast<int8_t>(s.value + static_cast<int8_t>(c));
        break;

    case 0xa7:
        {
            // The value's shift: a bit for the direction, one to keep the sign, then the count.
            const uint8_t arg = ReadByte(s);
            const int shift = arg & 0xf;
            const uint8_t value = static_cast<uint8_t>(s.value);
            uint8_t result =
                (arg & 0x80) == 0 ? static_cast<uint8_t>(value << shift) : static_cast<uint8_t>(value >> shift);
            if (arg & 0x40)
            {
                result = static_cast<uint8_t>((result & 0x7f) | (value & 0x80));
            }
            s.value = static_cast<int8_t>(result);
            break;
        }

    case 0xcb:
        {
            const uint16_t table = static_cast<uint16_t>(ReadWord(s));
            s.value = static_cast<int8_t>(DataByte(static_cast<uint32_t>(table + s.value)));
            break;
        }

    case 0xb8:
        {
            const uint8_t range = ReadByte(s);
            const uint32_t r = NextRandom();
            s.value = static_cast<int8_t>(range == 0 ? r : r % range);
            break;
        }

    case 0xb6:
        s.value = static_cast<int8_t>(DataByte(ch.dynamic_table + static_cast<uint8_t>(s.value)));
        break;

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

    case 0xb2:
        {
            const uint16_t table = static_cast<uint16_t>(ReadWord(s));
            ch.dynamic_value = DataWord(table + 2u * static_cast<uint8_t>(s.value));
            break;
        }

    case 0xbc:
        ch.dynamic_value = static_cast<uint16_t>(ch.dynamic_value + static_cast<uint16_t>(ReadWord(s)));
        break;

    case 0xb7:
        {
            const uint16_t range = static_cast<uint16_t>(ReadWord(s));
            const uint32_t r = NextRandom();
            ch.dynamic_value = static_cast<uint16_t>(range == 0 ? r : r % range);
            break;
        }

    case 0xb4:
        ch.dynamic_table = ch.dynamic_value;
        break;

    case 0xb5:
        ch.dynamic_value = DataWord(ch.dynamic_table + 2u * static_cast<uint8_t>(s.value));
        break;

    case 0xa8:
        ch.dynamic_value =
            static_cast<uint16_t>((ch.dynamic_value & 0x7f) | 0x8000 | ((ch.dynamic_value & 0x7fc0) << 1));
        break;

    case 0xcd:
        ReleaseChannel(ReadByte(s) & 0xf);
        break;

    case 0xbe:
        ReadByte(s);
        approximations_["the game's callbacks from a sequence left out"]++;
        break;

    // The commands that the files leave out.
    case 0xf1:
    case 0xca:
    case 0xd0:
    case 0xd1:
    case 0xd5:
    case 0xd6:
    case 0xe5:
    case 0xe6:
    case 0xe9:
    case 0xed:
    case 0xb3:
    case 0xba:
    case 0xa4:
        ReadByte(s);
        break;

    case 0xb0:
    case 0xbd:
    case 0xa0:
    case 0xa2:
        ReadWord(s);
        break;

    case 0xbb:
        ReadByte(s);
        ReadWord(s);
        break;

    case 0xef:
        ReadWord(s);
        ReadByte(s);
        break;

    default:
        break;
    }
}

void Performer::LayerControl(std::size_t c, uint8_t command)
{
    Channel& ch = group_.channels[c];
    Script& s = ch.script;
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
            StartLayer(c, l, DataWord(ch.dynamic_table + 2u * static_cast<uint8_t>(s.value)));
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
}

bool Performer::ChannelLowCommand(std::size_t c, uint8_t command)
{
    Channel& ch = group_.channels[c];
    Script& s = ch.script;
    const std::size_t low = command & 0xf;
    switch (command & 0xf0)
    {
    case 0x00:
        ch.delay = static_cast<int16_t>(low);
        return low != 0;

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

    return false;
}

} // namespace dolphinsf2::n64::player

namespace dolphinsf2::n64
{

using player::Performer;

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