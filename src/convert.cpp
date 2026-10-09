// SPDX-License-Identifier: MIT

// Sequence conversion (see convert.h). This file builds the SoundFont; midi.cpp and sfz.cpp write the other formats.
//
// Each performance preset is a bank's program with the track's oscillators and settings. Each such preset becomes one
// SoundFont instrument and preset, with a zone for each of the program's regions. Zones contain the region's key and
// velocity ranges, sample, root key, tuning, volume, pan, envelope and loop. Presets used by MIDI channel 10 also get a
// copy in bank 128. Synths look for drum instruments there.

#include "convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

#include "binary.h"
#include "envelope.h"
#include "midi.h"
#include "sf2.h"
#include "wave.h"

namespace dolphinsf2
{

namespace
{

// SoundFont modulator sources: 7-bit index, CC flag (bit 7), direction (bit 8), polarity (bit 9), type (bits 10-15).
constexpr uint16_t kSrcVelocityLinearNegative = 0x0102;
constexpr uint16_t kSrcVelocitySwitch = 0x0C02;
constexpr uint16_t kSrcModWheel = 0x0081;   // CC1, linear, unipolar, positive
constexpr uint16_t kSrcPitchWheel = 0x020E; // linear, bipolar, positive
constexpr uint16_t kSrcPitchWheelSensitivity = 0x0010;

// SoundFont bank for MIDI channel 10 (General MIDI drums). Synths use this bank regardless of bank select.
constexpr uint16_t kChannel10Bank = 128;

// Build the instrument's global zone. JAudio2 doesn't filter by velocity. Override the default velocity-to-filter
// modulator, in both the SoundFont 2.01 and the FluidSynth version.
//
// CC1 holds 128 times the vibrato's depth over kVibratoRangeCents, and the vibrato LFO has the preset's speed and
// delay. A vibrato that doesn't move (a speed of 0) gets no depth.
//
// The default pitch wheel modulator uses 12700 cents times the bend range / 128. This makes a 7-semitone bend set
// through RPN 0 about 5.5 cents flat. Like FluidSynth, replace it with a 12800-cent modulator targeting fineTune. The
// bend then covers the full range.
sf2::Zone GlobalZone(const PresetKey& key)
{
    sf2::Zone global;
    global.modulators.push_back({kSrcVelocityLinearNegative, sf2::kInitialFilterFc, 0, 0});
    global.modulators.push_back({kSrcVelocityLinearNegative, sf2::kInitialFilterFc, 0, kSrcVelocitySwitch});
    global.modulators.push_back({kSrcPitchWheel, sf2::kFineTune, 12800, kSrcPitchWheelSensitivity});

    const double hz = kVibratoHz * key.vibrato_rate;
    const auto depth = static_cast<int16_t>(hz > 0.0 ? kVibratoRangeCents : 0.0);
    global.modulators.push_back({kSrcModWheel, sf2::kVibLfoToPitch, depth, 0});
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
    sf2::Zone zone;

    auto gen = [&](uint16_t oper, int v)
    {
        zone.generators.push_back(sf2::Generator::Value(oper, v));
    };

    zone.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, z.low_key, z.high_key));
    zone.generators.push_back(sf2::Generator::Range(sf2::kVelRange, z.low_velocity, z.high_velocity));
    const Sf2Envelope e = ConvertEnvelope(z.envelope);
    const double level_db = (z.volume > 0.0 ? 20.0 * std::log10(z.volume) : -144.0) + e.peak_db;
    const int attenuation = std::clamp(static_cast<int>(std::lround(quieter - 10.0 * level_db)), 0, 1440);
    if (attenuation != 0)
    {
        gen(sf2::kInitialAttenuation, attenuation);
    }

    // JAudio2's pan law is SoundFont's: the left side plays at the cosine and the right at the sine of a quarter turn
    // times the pan.
    const int pan = std::clamp(static_cast<int>(std::lround((z.pan - 0.5) * 1000.0)), -500, 500);
    if (pan != 0)
    {
        gen(sf2::kPan, pan);
    }

    if (e.delay > -12000)
    {
        gen(sf2::kDelayVolEnv, e.delay);
    }
    gen(sf2::kAttackVolEnv, e.attack);
    gen(sf2::kHoldVolEnv, e.hold);
    gen(sf2::kDecayVolEnv, e.decay);
    gen(sf2::kSustainVolEnv, e.sustain);
    gen(sf2::kReleaseVolEnv, e.release);

    gen(sf2::kOverridingRootKey, z.root_key);
    const int cents = static_cast<int>(std::lround(std::clamp(z.cents, -12000.0, 12000.0)));
    if (cents / 100 != 0)
    {
        gen(sf2::kCoarseTune, cents / 100);
    }
    if (cents % 100 != 0)
    {
        gen(sf2::kFineTune, cents % 100);
    }

    if (loop)
    {
        gen(sf2::kSampleModes, 1);
    }
    gen(sf2::kSampleId, sample);

    return zone;
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
        sf2::Sample s;
        s.name = "w" + std::to_string(wave_system) + "_" + std::to_string(wave_id);
        s.data = std::move(w.samples);
        s.sample_rate = static_cast<uint32_t>(std::lround(pcm->sample_rate));
        s.original_key = 60;
        if (w.loop)
        {
            s.loop_start = w.loop_start;
            s.loop_end = w.loop_end;
        }
        else if (s.data.size() > 1)
        {
            s.loop_end = static_cast<uint32_t>(s.data.size()); // some synths validate unused loops too
        }

        id = std::make_pair(static_cast<uint16_t>(sf_.samples.size()), w.loop);
        sf_.samples.push_back(std::move(s));
        if (sf_.samples.size() > 0xffff)
        {
            throw FormatError("more than 65535 samples");
        }
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

    const auto inst_index = static_cast<uint16_t>(sf_.instruments.size());
    sf_.instruments.push_back(std::move(inst));

    sf2::Preset preset;
    preset.name = name;
    preset.bank = static_cast<uint16_t>(index / 128);
    preset.program = static_cast<uint16_t>(index % 128);
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Value(sf2::kInstrument, inst_index));
    preset.zones.push_back(std::move(z));
    sf_.presets.push_back(std::move(preset));
}

void SoundFontBuilder::AddChannel10Copy(uint32_t index)
{
    sf2::Preset copy = sf_.presets[index];
    for (const sf2::Preset& p : sf_.presets)
    {
        if (p.bank == kChannel10Bank && p.program == copy.program)
        {
            warnings_.push_back(copy.name + ": MIDI channel 10 plays " + p.name + " instead");
            return;
        }
    }

    copy.bank = kChannel10Bank;
    sf_.presets.push_back(std::move(copy));
}

// Check whether Windows reserves `stem` (the part before the first dot) as a device name. Trailing spaces are ignored;
// COM and LPT followed by superscript 1, 2 or 3 also count.
bool IsDeviceName(std::string stem)
{
    constexpr std::string_view kDevices[] = {"CON",  "PRN",  "AUX",  "NUL",  "CONIN$", "CONOUT$", "COM0", "COM1",
                                             "COM2", "COM3", "COM4", "COM5", "COM6",   "COM7",    "COM8", "COM9",
                                             "COM¹", "COM²", "COM³", "LPT0", "LPT1",   "LPT2",    "LPT3", "LPT4",
                                             "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",   "LPT¹",    "LPT²", "LPT³"};

    while (!stem.empty() && stem.back() == ' ')
    {
        stem.pop_back();
    }
    for (char& c : stem)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }

    return std::find(std::begin(kDevices), std::end(kDevices), stem) != std::end(kDevices);
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
    std::map<std::pair<uint32_t, uint32_t>, int> count, seen;
    for (const PresetKey& k : keys)
    {
        count[{k.bank, k.program}]++;
    }

    std::vector<std::string> names;
    for (const PresetKey& k : keys)
    {
        std::string n = "b" + std::to_string(k.bank) + " p" + std::to_string(k.program);
        if (count[{k.bank, k.program}] > 1)
        {
            n += " #" + std::to_string(++seen[{k.bank, k.program}]);
        }

        names.push_back(n);
    }

    return names;
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
                       key.direct_release);
}

Conversion ConvertSequence(const SequenceSound& sound, BankSet& banks, const PerformOptions& options)
{
    Conversion c;
    c.performance = Perform(sound, banks, options);

    MidiReport midi;
    c.midi = WriteMidi(c.performance, sound.name, midi);
    c.channels = midi.channels;
    if (midi.cut_notes)
    {
        c.performance.approximations["overlapping same-key notes cut short (all MIDI channels in use)"] +=
            midi.cut_notes;
    }

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
    sf.name = sound.name;
    sf.comment = "Converted by dolphinsf2 from sequence " + sound.name + ".";
    c.samples = sf.samples.size();
    c.sf2 = sf2::Write(sf);
    c.sfz = BuildSfz(c.performance, midi, banks, sound.name, names);

    c.warnings = midi.warnings;
    c.warnings.insert(c.warnings.end(), builder.Warnings().begin(), builder.Warnings().end());
    c.warnings.insert(c.warnings.end(), c.performance.warnings.begin(), c.performance.warnings.end());

    return c;
}

std::string FileName(const std::string& sequence_name)
{
    constexpr std::string_view kNotAllowed = "<>:\"/\\|?*";
    std::string name = sequence_name;
    for (char& c : name)
    {
        if (static_cast<unsigned char>(c) < 0x20 || kNotAllowed.find(c) != std::string_view::npos)
        {
            c = '_';
        }
    }

    // This is also the SFZ directory name. Windows strips trailing dots and spaces from directory names; "." and ".."
    // refer to existing directories.
    if (!name.empty() && (name.back() == '.' || name.back() == ' '))
    {
        name.back() = '_';
    }

    // Windows reserves device names regardless of extension: "NUL.mid" and "NUL.x.mid" are invalid, but "NUL_.x.mid" is
    // allowed.
    const std::size_t stem_end = std::min(name.find('.'), name.size());
    if (IsDeviceName(name.substr(0, stem_end)))
    {
        name.insert(stem_end, "_");
    }

    return name;
}

} // namespace dolphinsf2
