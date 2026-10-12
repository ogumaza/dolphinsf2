// SPDX-License-Identifier: MIT

// SFZ output (see sfz.h).
//
// Each track file has a <master> per preset, selected by loprog/hiprog and, above 128 presets, bank select (CC0).
// Regions match the SoundFont: the program's key and velocity regions, sample, root key, tuning, volume, pan, envelope
// and loops. The vibrato is a sine with its depth from CC1.

#include "sfz.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/wav.h"

#include "convert.h"
#include "envelope.h"
#include "instruments.h"
#include "wave.h"

namespace dolphinsf2
{
namespace
{

using seqlib::WavFile;
using seqlib::sfz::Number;

// Shared samples: one WAV file per wave, generated on first use.
class SampleSet
{
public:
    // A sample's name and loop bounds. An empty name stands for an unreadable wave.
    struct Wave
    {
        std::string name;
        bool loop = false;
        uint32_t loop_start = 0, loop_end = 0; // inclusive: SFZ requires that
    };

    explicit SampleSet(BankSet& banks) : banks_(banks)
    {
    }

    // Get a wave. Generate its sample on first use.
    const Wave& Get(uint32_t wave_system, uint16_t wave_id);

    // Take ownership of the generated samples. The collection is left empty.
    std::vector<SfzSample> Take()
    {
        return std::move(samples_);
    }

private:
    BankSet& banks_;
    std::map<std::pair<uint32_t, uint16_t>, Wave> waves_;
    std::vector<SfzSample> samples_;
};

const SampleSet::Wave& SampleSet::Get(uint32_t wave_system, uint16_t wave_id)
{
    const auto key = std::make_pair(wave_system, wave_id);
    if (auto it = waves_.find(key); it != waves_.end())
    {
        return it->second;
    }

    Wave wave;
    if (const Pcm* pcm = banks_.GetWave(wave_system, wave_id))
    {
        const auto crossfade = static_cast<uint32_t>((pcm->sample_rate * 4.0f + 999.0f) / 1000.0f); // 4 ms
        const PreparedWave w = PrepareForSfz(*pcm, crossfade);
        wave.name = "w" + std::to_string(wave_system) + "_" + std::to_string(wave_id) + ".wav";
        samples_.push_back({wave.name, WavFile(w.channels[0], static_cast<uint32_t>(std::lround(pcm->sample_rate)))});
        wave.loop = w.loop;
        if (w.loop)
        {
            wave.loop_start = w.loop_start;
            wave.loop_end = w.loop_end - 1;
        }
    }

    return waves_.emplace(key, std::move(wave)).first->second;
}

// Write a region's opcodes: its sample, keys and velocities, tuning, level, pan, envelope and loop. Add `gain_db` to
// its volume.
std::string RegionLine(const PresetZone& z, const SampleSet::Wave& wave, double gain_db)
{
    std::string s = "<region> sample=" + wave.name + " lokey=" + std::to_string(z.low_key) +
                    " hikey=" + std::to_string(z.high_key) + " lovel=" + std::to_string(z.low_velocity) +
                    " hivel=" + std::to_string(z.high_velocity) + " pitch_keycenter=" + std::to_string(z.root_key);
    s += seqlib::sfz::TuneOpcodes(static_cast<int>(std::lround(std::clamp(z.cents, -12000.0, 12000.0))));

    const SfzEnvelope e = ConvertEnvelopeForSfz(z.envelope);
    const double volume = (z.volume > 0.0 ? 20.0 * std::log10(z.volume) : -144.0) + e.peak_db + gain_db;
    if (std::abs(volume) >= 0.005)
    {
        s += " volume=" + Number(volume, 2);
    }

    // The game's pan law is sfizz's: a quarter turn's cosine and sine.
    const double pan = std::clamp((z.pan - 0.5) * 200.0, -100.0, 100.0);
    if (std::abs(pan) >= 0.05)
    {
        s += " pan=" + Number(pan, 1);
    }

    s += seqlib::sfz::AmpEnvelopeOpcodes({.delay = e.delay,
                                          .attack = e.attack,
                                          .hold = e.hold,
                                          .decay = e.decay,
                                          .sustain = e.sustain,
                                          .release = e.release});
    s += seqlib::sfz::LoopOpcodes(wave.loop, wave.loop_start, wave.loop_end);

    return s + "\n";
}

// Write preset `key`'s vibrato as SFZ LFO 1: a sine at the preset's speed, after its delay, with its depth from CC1.
std::string VibratoOpcodes(const PresetKey& key)
{
    const double hz = kVibratoHz * key.vibrato_rate;
    if (hz <= 0.0)
    {
        return "";
    }

    // CC1 holds 128 times the depth over kVibratoRangeCents, and sfizz divides controller values by 127.
    std::string s = "lfo1_wave=1 lfo1_freq=" + Number(hz, 6);
    if (key.vibrato_delay != 0)
    {
        s += " lfo1_delay=" + Number(key.vibrato_delay * kSubFrameSeconds, 6);
    }
    s += " lfo1_pitch_oncc1=" + Number(kVibratoRangeCents * 127.0 / 128.0, 6);

    return s + "\n";
}

} // namespace

SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks, const std::string& title,
                        const std::vector<std::string>& preset_names)
{
    // Each file is named after its MIDI track.
    seqlib::sfz::TrackFileOptions options;
    options.title = title;
    options.program = "dolphinsf2";
    options.preset_names = preset_names;
    options.presets = perf.presets.size();
    options.level_cut_db = perf.level_cut_db;
    options.track_names = true;

    // Each preset adds its vibrato and a group of its zones.
    SampleSet samples(banks);
    SfzInstruments out;
    auto text = [&](int)
    {
        seqlib::sfz::TrackText added;
        added.preset = [&](uint32_t p)
        {
            const PresetKey& key = perf.presets[p];
            std::string s = VibratoOpcodes(key) + "<group>\n";
            for (const PresetZone& z : ZonesOf(key, perf, banks))
            {
                const SampleSet::Wave& wave = samples.Get(z.wave_system, z.wave_id);
                if (!wave.name.empty())
                {
                    s += RegionLine(z, wave, perf.level_cut_db);
                }
            }

            return s;
        };

        return added;
    };
    out.files = seqlib::sfz::TrackFiles(perf, midi, options, text);
    out.samples = samples.Take();

    return out;
}

} // namespace dolphinsf2
