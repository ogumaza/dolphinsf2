// SPDX-License-Identifier: MIT

// The layers of the Nintendo 64 sequence player (see n64_player.cpp): their scripts, their notes and voices, the update
// that steps the fades and the run of a piece.

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

#include "n64_player_parts.h"

namespace dolphinsf2::n64::player
{

namespace
{

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

// What drum `note` of `bank` plays on layer `layer` of channel `ch`, or nothing if it makes no sound. The drum sets the
// layer's pan unless the layer ignores it.
std::optional<NoteSound> DrumSound(const Channel& ch, Layer& layer, const Bank* bank, int note)
{
    const int drum = static_cast<uint8_t>(note + layer.transposition + ch.transposition);
    if (bank == nullptr || drum >= static_cast<int>(bank->drums.size()) ||
        !bank->drums[static_cast<std::size_t>(drum)] || ch.muted)
    {
        return std::nullopt;
    }

    const Drum& d = *bank->drums[static_cast<std::size_t>(drum)];
    NoteSound sound{.key = drum, .sample = d.sample.sample, .tuning = d.sample.tuning};
    sound.voice.bank = ch.bank;
    sound.voice.kind = VoiceKind::kDrums;

    // A drum with a release takes its envelope and release. The preset keeps the envelope and release of the channel
    // for the others.
    const bool any_without = std::any_of(bank->drums.begin(), bank->drums.end(),
                                         [](const std::optional<Drum>& dr) { return dr && dr->release == 0; });
    if (any_without)
    {
        sound.voice.envelope = ch.adsr.envelope;
        sound.voice.release = ch.adsr.release;
    }

    // The layer takes the drum's pan unless it ignores it. A drum doesn't sweep.
    if (!layer.ignore_drum_pan)
    {
        layer.pan = d.pan;
    }
    layer.portamento.extent = 0.0;

    return sound;
}

// What effect `note` of `bank` plays on layer `layer` of channel `ch`, or nothing if it makes no sound.
std::optional<NoteSound> EffectSound(const Channel& ch, Layer& layer, const Bank* bank, int note)
{
    const uint32_t number = static_cast<uint32_t>(note + (layer.transposition << 6)) & 0xffff;
    if (bank == nullptr || number >= bank->effects.size() || bank->effects[number].sample == nullptr || ch.muted)
    {
        return std::nullopt;
    }

    NoteSound sound{
        .key = EffectKey(number), .sample = bank->effects[number].sample, .tuning = bank->effects[number].tuning};
    sound.voice.bank = ch.bank;
    sound.voice.kind = VoiceKind::kEffects;
    sound.voice.program = static_cast<uint8_t>(number >> 7);

    // A layer with a release gives its envelope and release. The others take the envelope and release of the channel.
    // An effect doesn't sweep.
    const bool has_release = layer.adsr.release != 0;
    sound.voice.envelope = has_release ? layer.adsr.envelope : ch.adsr.envelope;
    sound.voice.release = has_release ? layer.adsr.release : ch.adsr.release;
    layer.portamento.extent = 0.0;

    return sound;
}

} // namespace

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
        if (semitone != kScriptEnd || layer.delay != 0)
        {
            break;
        }
    }

    if (semitone != kScriptEnd)
    {
        SetVoice(c, l, semitone);
    }
    if (layer.muted)
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
            SetLayerInstrument(c, l, ReadByte(s));
            break;

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
                                         : DataByte(static_cast<std::size_t>(group_.short_velocities) + i);
                layer.velocity = static_cast<float>(velocity * velocity) / static_cast<float>(127 * 127);
            }
            else if ((command & 0xf0) == 0xe0)
            {
                const std::size_t i = command & 0xf;
                layer.gate_time = group_.short_gate_times < 0
                                      ? data_.ShortGateTimes()[i]
                                      : DataByte(static_cast<std::size_t>(group_.short_gate_times) + i);
            }
            break;
        }
    }
}

void Performer::SetLayerInstrument(std::size_t c, std::size_t l, uint8_t program)
{
    Channel& ch = group_.channels[c];
    Layer& layer = *ch.layers[l];
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
        return;
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

std::optional<NoteSound> Performer::InstrumentSound(const Channel& ch, Layer& layer, const Bank* bank, int instrument,
                                                    int note)
{
    const int semitone = static_cast<uint8_t>(note + group_.transposition + layer.transposition + ch.transposition);
    if (semitone > 127 || ch.muted)
    {
        return std::nullopt;
    }
    if (bank == nullptr || instrument >= static_cast<int>(bank->instruments.size()) ||
        !bank->instruments[static_cast<std::size_t>(instrument)])
    {
        return std::nullopt;
    }

    // The semitones below the instrument's low one play its low sample, those above its high one its high sample.
    const Instrument& inst = *bank->instruments[static_cast<std::size_t>(instrument)];
    const TunedSample& ts = semitone < inst.low     ? inst.low_sample
                            : semitone <= inst.high ? inst.normal_sample
                                                    : inst.high_sample;
    NoteSound sound{.sample = ts.sample, .tuning = ts.tuning};
    sound.voice.bank = ch.bank;
    sound.voice.kind = VoiceKind::kInstrument;
    sound.voice.program = static_cast<uint8_t>(instrument);

    // A layer with a release gives its envelope and release. The others take the envelope and release of the channel.
    const bool has_release = layer.adsr.release != 0;
    sound.voice.envelope = has_release ? layer.adsr.envelope : ch.adsr.envelope;
    sound.voice.release = has_release ? layer.adsr.release : ch.adsr.release;

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

    // The note's key is the key where the sweep starts. A key above MIDI's highest plays at the highest.
    sound.pitch_index = start;
    sound.key = KeyOf(start);
    if (sound.key > 127)
    {
        approximations_["notes above MIDI's highest key played at it"]++;
        sound.key = 127;
    }

    return sound;
}

std::optional<NoteSound> Performer::SoundOf(std::size_t c, std::size_t l, int note)
{
    Channel& ch = group_.channels[c];
    Layer& layer = *ch.layers[l];
    int instrument = layer.instrument;
    if (instrument == 0xff)
    {
        if (ch.instrument == kNoInstrument)
        {
            return std::nullopt;
        }
        instrument = ch.instrument;
    }

    const Bank* bank = ch.bank != 0xff ? data_.GetBank(ch.bank) : nullptr;
    if (instrument == kDrumsProgram)
    {
        return DrumSound(ch, layer, bank, note);
    }
    if (instrument == kEffectsProgram)
    {
        return EffectSound(ch, layer, bank, note);
    }

    return InstrumentSound(ch, layer, bank, instrument, note);
}

int16_t Performer::SampleTicks(const NoteSound& sound, const Layer& layer) const
{
    const double length = static_cast<double>(sound.sample->pcm.Length());
    const double pitch = sound.voice.kind != VoiceKind::kInstrument
                             ? sound.tuning
                             : sound.tuning * data_.Pitches()[static_cast<std::size_t>(sound.pitch_index)];
    double time = length * (group_.tempo) * kTicksPerSampleAndTempo / (pitch * layer.bend);
    time = std::min(time, 32766.0);

    return static_cast<int16_t>(static_cast<int>(time) + 1);
}

void Performer::SetSweepRate(Layer& layer)
{
    Portamento& sweep = layer.portamento;
    if (sweep.mode == 0)
    {
        return;
    }

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

void Performer::SetVoice(std::size_t c, std::size_t l, int note)
{
    Channel& ch = group_.channels[c];
    Layer& layer = *ch.layers[l];
    std::optional<NoteSound> sound = SoundOf(c, l, note);
    if (!sound || sound->sample == nullptr)
    {
        layer.muted = true;
        return;
    }

    // A note of no length lasts as long as its sample plays at the tempo.
    if (layer.delay == 0)
    {
        layer.delay = SampleTicks(*sound, layer);
        layer.gate_delay = 0;
    }

    if (layer.bend != 1.0f)
    {
        approximations_["a layer's bend left out"]++;
    }

    // A sweep's steps an update. A special sweep takes the share `time` / 256 of the note's length.
    SetSweepRate(layer);

    // Two notes of a channel that sound together share one pitch. When either glides, the other moves with it.
    PresetVoice& voice = sound->voice;
    const Portamento& sweep = layer.portamento;
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
        if (layer.continuous && layer.sample == sound->sample && voice.kind == VoiceKind::kInstrument)
        {
            layer.offset = static_cast<float>(sound->key - layer.key);
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
    const int velocity = std::max(SquaredValue(layer.velocity), 1);
    layer.key = static_cast<uint8_t>(sound->key);
    layer.sample = sound->sample;
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
            EndGroup();
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
        if (!ch.muted)
        {
            peak_level_ = std::max(peak_level_, level);
        }
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
    if (at >= group_.data.size())
    {
        throw FormatError("a script writes past the end of sequence " + std::to_string(group_.sequence));
    }

    group_.data[at] = value;
    data_hash_.reset();
}

uint64_t Performer::Signature() const
{
    // Every field that the scripts change, the sequence's data included, except the envelope of a layer and the state
    // of the random generator. The tempo's accumulator only moves where the ticks fall, by less than an update.
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

    // The group.
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
    add(&group_.fade_velocity, sizeof group_.fade_velocity);
    add(&group_.stored_fade_timer, sizeof group_.stored_fade_timer);
    add(&group_.state, sizeof group_.state);
    add(&group_.stop_script, 1);
    add(group_.ports.data(), group_.ports.size());
    add(&group_.short_velocities, sizeof group_.short_velocities);
    add(&group_.short_gate_times, sizeof group_.short_gate_times);

    // The channels and their layers.
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
        add(&ch.muted, 1);
        add(&ch.velocity_variance, 1);
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

            // A layer can keep an envelope from before the loop that no note of the layer plays. The signature leaves
            // the envelope out. With the envelope, the loops of 44 pieces of Animal Crossing's US disc would start
            // later. Their notes would stay the same.
            add(&layer->adsr.release, 1);
            add(&layer->ignore_drum_pan, 1);
            add(&layer->bend, sizeof layer->bend);
            add(&layer->continuous, 1);
            add(&layer->portamento.mode, 1);
            add(&layer->portamento.target, sizeof layer->portamento.target);
            add(&layer->portamento.time, sizeof layer->portamento.time);
            add(&layer->portamento.steps, sizeof layer->portamento.steps);
            add(&layer->portamento.extent, sizeof layer->portamento.extent);
            add(&layer->portamento.rate, sizeof layer->portamento.rate);
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
    for (std::size_t c = 0; c < kChannels; c++)
    {
        group_.channels[c].adsr = {data_.DefaultEnvelope(), kDefaultRelease};
        group_.channels[c].muted = (piece_.muted & (1u << c)) != 0;
    }

    // Ticks run until the state repeats, the sequence ends or the limit passes.
    std::unordered_map<uint64_t, uint32_t> seen;
    std::optional<std::pair<uint32_t, uint32_t>> loop;
    double bpm = 0.0;
    constexpr uint64_t kLimit = static_cast<uint64_t>(kLimitSeconds * kUpdatesPerSecond);
    while (group_.enabled && update_ < kLimit)
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
    else if (update_ >= kLimit)
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

} // namespace dolphinsf2::n64::player
