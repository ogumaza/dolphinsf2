// SPDX-License-Identifier: MIT

// Sequence conversion (see convert.h). This file builds the SoundFont, and sfz.cpp writes the SFZ files. seqlib writes
// the MIDI and SoundFont files.
//
// Each performance preset is a bank's program with the track's oscillators and settings. Each such preset becomes one
// SoundFont instrument and preset, with a zone for each of the program's regions. Zones contain the region's key and
// velocity ranges, sample, root key, tuning, volume, pan, envelope and loop. Presets used by MIDI channel 10 also get a
// copy in bank 128. Synths look for drum instruments there.

#include "convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/conversion.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/soundfont.h"

#include "envelope.h"
#include "wave.h"

namespace dolphinsf2
{

namespace sf2 = seqlib::sf2;

namespace
{

// Build the instrument's global zone. JAudio2 doesn't filter by velocity. The zone starts with seqlib's
// kNoVelocityFilterFullBend. Those modulators turn the default velocity-to-filter modulators off, and give the pitch
// wheel its full range.
//
// CC1 holds 128 times the vibrato's depth over kVibratoRangeCents, and the vibrato LFO has the preset's speed and
// delay. A vibrato that doesn't move (a speed of 0) gets no depth.
sf2::Zone GlobalZone(const PresetKey& key)
{
    sf2::Zone global;
    global.modulators.assign(sf2::kNoVelocityFilterFullBend.begin(), sf2::kNoVelocityFilterFullBend.end());

    const double hz = kVibratoHz * key.vibrato_rate;
    const auto depth = static_cast<int16_t>(hz > 0.0 ? kVibratoRangeCents : 0.0);
    global.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, depth, 0});
    if (hz > 0.0)
    {
        const int freq = std::clamp(static_cast<int>(std::lround(1200.0 * std::log2(hz / 8.176))), -16000, 4500);
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqVibLfo, freq));
        const int delay = ToTimecents(key.vibrato_delay * kSubFrameSeconds * 1000.0, -12000, 5000);
        global.generators.push_back(sf2::Generator::Value(sf2::kDelayVibLfo, delay));
    }

    return global;
}

// Build a zone for `z` with `sample`. Add `quieter` centibels of attenuation.
sf2::Zone RegionZone(const PresetZone& z, double quieter, uint16_t sample, bool loop)
{
    const Sf2Envelope e = ConvertEnvelope(z.envelope);
    const double level_db = (z.volume > 0.0 ? 20.0 * std::log10(z.volume) : -144.0) + e.peak_db;
    const int cents = static_cast<int>(std::lround(std::clamp(z.cents, -12000.0, 12000.0)));

    sf2::ZoneValues values;
    values.keys = {z.low_key, z.high_key};
    values.velocities = {z.low_velocity, z.high_velocity};
    values.attenuation = quieter - 10.0 * level_db;
    // JAudio2's pan law is SoundFont's: the left side plays at the cosine and the right at the sine of a quarter turn
    // times the pan.
    values.pan = std::clamp(static_cast<int>(std::lround((z.pan - 0.5) * 1000.0)), -500, 500);
    if (e.delay > -12000)
    {
        values.delay = e.delay;
    }
    values.attack = e.attack;
    values.hold = e.hold;
    values.decay = e.decay;
    values.sustain = e.sustain;
    values.release = e.release;
    values.root_key = z.root_key;
    values.coarse_tune = cents / 100;
    values.fine_tune = cents % 100;
    values.loop = loop;
    values.sample = sample;

    return sf2::RegionZone(values);
}

// Build a SoundFont instrument and preset for each performance preset. Add each wave's samples only once.
class SoundFontBuilder
{
public:
    SoundFontBuilder(BankSet& banks, const Performance& perf, double quieter)
        : banks_(banks), perf_(perf), quieter_(quieter)
    {
    }

    // Add preset `index` at bank index / 128, program index % 128.
    void Add(uint32_t index, const std::string& name);

    // Copy a preset previously added by Add to bank 128 for MIDI channel 10. Keep its program number; warn if another
    // preset already occupies that slot.
    void AddChannel10Copy(uint32_t index);

    sf2::SoundFont& Result()
    {
        return sf_;
    }

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // Add a wave's sample on first use. Return nothing if the wave cannot be read.
    std::optional<std::pair<uint16_t, bool>> Sample(uint32_t wave_system, uint16_t wave_id);

    BankSet& banks_;
    const Performance& perf_;
    double quieter_;
    std::vector<std::string> warnings_;
    sf2::SoundFont sf_;
    std::map<std::pair<uint32_t, uint16_t>, std::optional<std::pair<uint16_t, bool>>> samples_;
};

std::optional<std::pair<uint16_t, bool>> SoundFontBuilder::Sample(uint32_t wave_system, uint16_t wave_id)
{
    const auto key = std::make_pair(wave_system, wave_id);
    if (auto it = samples_.find(key); it != samples_.end())
    {
        return it->second;
    }

    std::optional<std::pair<uint16_t, bool>> id;
    if (const Pcm* pcm = banks_.GetWave(wave_system, wave_id))
    {
        PreparedWave w = PrepareForSoundFont(*pcm);
        const bool loop = w.loop;
        const std::vector<uint16_t> ids =
            sf2::AddSamples(sf_, std::move(w), "w" + std::to_string(wave_system) + "_" + std::to_string(wave_id),
                            static_cast<uint32_t>(std::lround(pcm->sample_rate)), 60);
        id = std::make_pair(ids.at(0), loop);
    }

    return samples_.emplace(key, id).first->second;
}

void SoundFontBuilder::Add(uint32_t index, const std::string& name)
{
    const PresetKey& key = perf_.presets[index];
    sf2::Instrument inst;
    inst.name = name;
    inst.zones.push_back(GlobalZone(key));
    if (banks_.Archive().FindBank(key.bank) == nullptr)
    {
        warnings_.push_back(name + ": there is no bank " + std::to_string(key.bank));
    }

    for (const PresetZone& z : ZonesOf(key, perf_, banks_))
    {
        const std::optional<std::pair<uint16_t, bool>> sample = Sample(z.wave_system, z.wave_id);
        if (!sample)
        {
            warnings_.push_back(name + ": no wave for keys " + std::to_string(z.low_key) + "-" +
                                std::to_string(z.high_key));
            continue;
        }

        inst.zones.push_back(RegionZone(z, quieter_, sample->first, sample->second));
    }

    sf2::AddInstrumentPreset(sf_, index, name, std::move(inst));
}

void SoundFontBuilder::AddChannel10Copy(uint32_t index)
{
    sf2::AddChannel10Copy(sf_, index, warnings_);
}

// The loudest zone of the presets that `perf` plays, as amplitude: its volume times its envelope's peak.
double LoudestZone(const Performance& perf, BankSet& banks)
{
    double loudest = 0.0;
    for (const PresetKey& key : perf.presets)
    {
        for (const PresetZone& z : ZonesOf(key, perf, banks))
        {
            const Sf2Envelope e = ConvertEnvelope(z.envelope);
            loudest = std::max(loudest, z.volume * std::pow(10.0, e.peak_db / 20.0));
        }
    }

    return loudest;
}

} // namespace

std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys)
{
    std::vector<std::pair<uint32_t, uint32_t>> banks_and_programs;
    for (const PresetKey& k : keys)
    {
        banks_and_programs.emplace_back(k.bank, k.program);
    }

    return sf2::PresetNames(banks_and_programs, true);
}

std::vector<PresetZone> ZonesOf(const PresetKey& key, const Performance& perf, BankSet& banks)
{
    const Bank* bank = banks.Archive().FindBank(key.bank);
    if (bank == nullptr)
    {
        return {};
    }

    const Oscillator* volume =
        key.volume_oscillator >= 0 ? &perf.oscillators[static_cast<std::size_t>(key.volume_oscillator)] : nullptr;
    const Oscillator* pitch =
        key.pitch_oscillator >= 0 ? &perf.oscillators[static_cast<std::size_t>(key.pitch_oscillator)] : nullptr;
    return PresetZones(*bank, banks.Archive().FindWaveSystem(bank->wave_system), key.program, volume, pitch,
                       key.direct_release, perf.default_release);
}

Conversion ConvertSequence(const SequenceSound& sound, BankSet& banks, const PerformOptions& options)
{
    Conversion c;
    c.performance = Perform(sound, banks, options);

    const MidiReport midi = ntdlib::sequence_export::WriteMidiFile(c, sound.name);

    // SoundFont zones can't amplify their samples. If a zone plays above full level, attenuate every zone equally to
    // keep their balance. SFZ keeps the levels.
    const double loudest = LoudestZone(c.performance, banks);
    const double quieter = loudest > 1.0 ? 200.0 * std::log10(loudest) : 0.0;
    if (loudest > 1.0)
    {
        char what[160];
        std::snprintf(what, sizeof(what),
                      "instrument volume above full level (SoundFont attenuated by %.1f dB; SFZ keeps the level)",
                      quieter / 10.0);
        c.performance.approximations[what] = NoteCount(c.performance);
    }

    SoundFontBuilder builder(banks, c.performance, quieter);
    const std::vector<std::string> names = PresetNames(c.performance.presets);
    for (std::size_t i = 0; i < c.performance.presets.size(); i++)
    {
        builder.Add(static_cast<uint32_t>(i), names[i]);
    }

    for (uint32_t index : midi.channel_10_presets)
    {
        builder.AddChannel10Copy(index);
    }

    sf2::SoundFont& sf = builder.Result();
    const std::string comment = "Converted by dolphinsf2 from sequence " + sound.name + ".";
    ntdlib::sequence_export::WriteSoundFont(c, sf, sound.name, comment, "dolphinsf2");
    c.sfz = BuildSfz(c.performance, midi, banks, sound.name, names);

    ntdlib::sequence_export::CollectWarnings(c, midi, builder.Warnings(), {});

    return c;
}

} // namespace dolphinsf2
