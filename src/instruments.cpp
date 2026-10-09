// SPDX-License-Identifier: MIT

// Instrument lookup (see instruments.h). Four Swords Adventures' JAudio2 applies an instrument's effects and selects
// its key and velocity regions as The Wind Waker's JAudio does: TBasicInst::getParam, TDrumSet::getParam and the
// effects' getY.

#include "instruments.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <variant>

#include "binary.h"

namespace dolphinsf2
{

namespace
{

// The value of a key or velocity effect (JASInstSense::getY): a line from `low` at 0 to `high` at 127, or through 1 at
// the center when it has one.
float SenseValue(const InstEffect& e, int key, int velocity)
{
    int x = 0;
    if (e.trigger == 1)
    {
        x = velocity;
    }
    else if (e.trigger == 2)
    {
        x = key;
    }

    const auto fx = static_cast<float>(x);
    if (e.center == 127 || e.center == 0)
    {
        return e.low + fx * (e.high - e.low) / 127.0f;
    }
    if (x < e.center)
    {
        return e.low + (1.0f - e.low) * (fx / static_cast<float>(e.center));
    }

    return (e.high - 1.0f) * ((fx - e.center) / static_cast<float>(127 - e.center)) + 1.0f;
}

// Apply `effects` at `key` and `velocity` to `sound`: volume and pitch multiply, and pan moves by the effect's value.
// Effects on the effect send and surround are left out. A zone can't play them.
void ApplyEffects(const std::vector<InstEffect>& effects, int key, int velocity, NoteSound& sound)
{
    for (const InstEffect& e : effects)
    {
        float y = e.base;
        if (e.kind == InstEffect::Kind::kRandom)
        {
            sound.random = sound.random || e.width != 0.0f;
        }
        else
        {
            y = SenseValue(e, key, velocity);
            sound.key_sense = sound.key_sense || e.trigger == 2;
            sound.velocity_sense = sound.velocity_sense || e.trigger == 1;
        }

        switch (e.target)
        {
        case 0:
            sound.volume *= y;
            break;
        case 1:
            sound.pitch *= y;
            break;
        case 2:
            sound.pan += y - 0.5f;
            break;
        default:
            break;
        }
    }
}

// The first velocity region whose high velocity is at or above `velocity`.
const VelocityRegion* FindVelocityRegion(const std::vector<VelocityRegion>& regions, int velocity)
{
    for (const VelocityRegion& region : regions)
    {
        if (velocity <= region.high_velocity)
        {
            return &region;
        }
    }

    return nullptr;
}

} // namespace

std::optional<NoteSound> FindSound(const Bank& bank, uint32_t program, int key, int velocity)
{
    const auto found = bank.programs.find(program);
    if (found == bank.programs.end())
    {
        return std::nullopt;
    }

    NoteSound sound;
    sound.bank = bank.number;
    sound.program = program;
    sound.wave_system = bank.wave_system;
    const VelocityRegion* region = nullptr;
    if (const auto* inst = std::get_if<Instrument>(&found->second))
    {
        sound.volume = inst->volume;
        sound.pitch = inst->pitch;
        ApplyEffects(inst->effects, key, velocity, sound);
        for (const KeyRegion& keys : inst->keys)
        {
            if (key <= keys.high_key)
            {
                region = FindVelocityRegion(keys.velocities, velocity);
                break;
            }
        }
        for (std::size_t index : inst->oscillators)
        {
            sound.oscillators.push_back(&bank.oscillators[index]);
        }
    }
    else
    {
        const auto& drums = std::get<DrumSet>(found->second);
        if (key < 0 || key >= 128 || !drums.keys[static_cast<std::size_t>(key)])
        {
            return std::nullopt;
        }

        const Drum& drum = *drums.keys[static_cast<std::size_t>(key)];
        sound.drum = true;
        sound.volume = drum.volume;
        sound.pitch = drum.pitch;
        sound.pan = drum.pan;
        sound.direct_release = drum.release;
        ApplyEffects(drum.effects, key, velocity, sound);
        region = FindVelocityRegion(drum.velocities, velocity);
    }
    if (region == nullptr)
    {
        return std::nullopt;
    }

    sound.volume *= region->volume;
    sound.pitch *= region->pitch;
    sound.wave_id = region->wave_id;

    return sound;
}

std::vector<PlayedRegion> ProgramRegions(const Bank& bank, uint32_t program)
{
    std::vector<PlayedRegion> regions;
    const auto found = bank.programs.find(program);
    if (found == bank.programs.end())
    {
        return regions;
    }

    // Each velocity region plays from the velocity after the one before.
    auto add = [&](int low_key, int high_key, const std::vector<VelocityRegion>& velocities)
    {
        int low_velocity = 0;
        for (const VelocityRegion& v : velocities)
        {
            if (v.high_velocity >= low_velocity)
            {
                regions.push_back({low_key, high_key, low_velocity, v.high_velocity, v.wave_id});
            }
            low_velocity = std::max<int>(low_velocity, v.high_velocity + 1);
        }
    };

    if (const auto* inst = std::get_if<Instrument>(&found->second))
    {
        int low_key = 0;
        for (const KeyRegion& keys : inst->keys)
        {
            if (keys.high_key >= low_key)
            {
                add(low_key, keys.high_key, keys.velocities);
            }
            low_key = std::max<int>(low_key, keys.high_key + 1);
        }
    }
    else
    {
        const auto& drums = std::get<DrumSet>(found->second);
        for (int key = 0; key < 128; key++)
        {
            if (drums.keys[static_cast<std::size_t>(key)])
            {
                add(key, key, drums.keys[static_cast<std::size_t>(key)]->velocities);
            }
        }
    }

    return regions;
}

std::vector<PresetZone> PresetZones(const Bank& bank, const WaveSystem* waves, uint32_t program,
                                    const Oscillator* volume_override, const Oscillator* pitch_override,
                                    uint16_t direct_release)
{
    std::vector<PresetZone> zones;
    const auto found = bank.programs.find(program);
    if (found == bank.programs.end())
    {
        return zones;
    }

    // Instruments whose effects follow the key play each key differently.
    bool per_key = false;
    if (const auto* inst = std::get_if<Instrument>(&found->second))
    {
        per_key = std::ranges::any_of(
            inst->effects, [](const InstEffect& e) { return e.kind == InstEffect::Kind::kSense && e.trigger == 2; });
    }

    std::map<std::tuple<const Oscillator*, const Oscillator*, uint16_t>, EnvelopeCurve> curves;
    for (const PlayedRegion& region : ProgramRegions(bank, program))
    {
        for (int low = region.low_key; low <= region.high_key; low = per_key ? low + 1 : region.high_key + 1)
        {
            const int high = per_key ? low : region.high_key;
            const std::optional<NoteSound> sound = FindSound(bank, program, low, region.high_velocity);
            if (!sound)
            {
                continue;
            }

            // The channel's oscillators: the instrument's, or a drum's constant one, with the track's in their place.
            static const Oscillator kConstant;
            std::array<const Oscillator*, 2> oscs{};
            for (std::size_t i = 0; i < 2 && i < sound->oscillators.size(); i++)
            {
                oscs[i] = sound->oscillators[i];
            }
            if (sound->drum)
            {
                oscs = {&kConstant, nullptr};
            }
            if (volume_override != nullptr)
            {
                oscs[0] = volume_override;
            }
            if (pitch_override != nullptr)
            {
                oscs[1] = pitch_override;
            }

            PresetZone z;
            z.low_key = low;
            z.high_key = high;
            z.low_velocity = region.low_velocity;
            z.high_velocity = region.high_velocity;
            z.wave_system = sound->wave_system;
            z.wave_id = sound->wave_id;
            const WaveInfo* wave = nullptr;
            if (waves != nullptr)
            {
                const auto w = waves->waves.find(sound->wave_id);
                wave = w != waves->waves.end() ? &w->second : nullptr;
            }
            z.root_key = sound->drum ? static_cast<uint8_t>(low) : (wave ? wave->base_key : 60);
            z.cents = sound->pitch > 0.0f ? 1200.0 * std::log2(static_cast<double>(sound->pitch)) : 0.0;
            z.volume = sound->volume;
            z.pan = sound->pan;

            std::vector<const Oscillator*> volume;
            for (const Oscillator* osc : oscs)
            {
                if (osc != nullptr && osc->target == 0)
                {
                    volume.push_back(osc);
                }
                z.pitch_envelope = z.pitch_envelope || (osc != nullptr && osc->target == 1);
            }
            const uint16_t release = direct_release != 0 ? direct_release : sound->direct_release;
            const auto key =
                std::make_tuple(volume.empty() ? nullptr : volume[0], volume.size() > 1 ? volume[1] : nullptr, release);
            auto [it, added] = curves.try_emplace(key);
            if (added)
            {
                it->second = TraceEnvelope(volume, release);
            }
            z.envelope = it->second;
            zones.push_back(std::move(z));
        }
    }

    return zones;
}

BankSet::BankSet(const SoundArchive& archive, AwReader read_aw) : archive_(archive), read_aw_(std::move(read_aw))
{
}

const Pcm* BankSet::GetWave(uint32_t wave_system, uint16_t wave_id)
{
    const auto key = std::make_pair(wave_system, wave_id);
    if (const auto it = waves_.find(key); it != waves_.end())
    {
        return it->second.get();
    }

    std::unique_ptr<Pcm>& slot = waves_[key];
    const WaveSystem* ws = archive_.FindWaveSystem(wave_system);
    const WaveInfo* info = nullptr;
    if (ws != nullptr)
    {
        const auto found = ws->waves.find(wave_id);
        info = found != ws->waves.end() ? &found->second : nullptr;
    }
    if (info == nullptr)
    {
        errors_.push_back("wave " + std::to_string(wave_id) + " of wave system " + std::to_string(wave_system) +
                          " is missing");
        return nullptr;
    }

    auto [aw, added] = aws_.try_emplace(info->archive);
    if (added)
    {
        aw->second = read_aw_(info->archive);
        if (!aw->second)
        {
            errors_.push_back("wave archive " + info->archive + " is missing");
        }
    }
    if (!aw->second)
    {
        return nullptr;
    }

    try
    {
        slot = std::make_unique<Pcm>(DecodeWave(*info, *aw->second));
    }
    catch (const FormatError& e)
    {
        errors_.push_back("wave " + std::to_string(wave_id) + " of " + info->archive + ": " + e.what());
    }

    return slot.get();
}

} // namespace dolphinsf2
