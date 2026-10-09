// SPDX-License-Identifier: MIT

// SFZ output (see sfz.h).
//
// Each track file has a <master> per preset, selected by loprog/hiprog and, above 128 presets, bank select (CC0).
// Regions match the SoundFont: the program's key and velocity regions, sample, root key, tuning, volume, pan, envelope
// and loops. The vibrato is a sine with its depth from CC1.

#include "sfz.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

#include "convert.h"
#include "envelope.h"
#include "instruments.h"
#include "wave.h"

namespace dolphinsf2
{

namespace
{

// Custom curve IDs; sfizz reserves 0-6.
constexpr int kPanCurve = 7; // CC10 to pan

// sfizz divides pitch wheel values by 8191; the MIDI writer uses 8192 steps per full range. Scale the SFZ bend range by
// 8191/8192.
constexpr double kBendScale = 8191.0 / 8192.0;

// Format `v` with at most `decimals` fractional digits.
std::string Number(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos)
    {
        while (s.back() == '0')
        {
            s.pop_back();
        }
        if (s.back() == '.')
        {
            s.pop_back();
        }
    }

    return s == "-0" ? "0" : s;
}

// Write a <curve> mapping controller values 0-127 through `f`.
template <typename F>
std::string Curve(int index, const char* what, F f)
{
    std::string s = "// " + std::string(what) + "\n<curve> curve_index=" + std::to_string(index);
    for (int v = 0; v < 128; v++)
    {
        char name[8];
        std::snprintf(name, sizeof(name), "v%03d", v);
        s += (v % 8 == 0 ? "\n" : " ") + std::string(name) + "=" + Number(f(v), 6);
    }

    return s + "\n\n";
}

// Make `text` safe for a single-line comment by replacing control characters, including line breaks, with spaces.
std::string CommentText(std::string text)
{
    for (char& c : text)
    {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
        {
            c = ' ';
        }
    }

    return text;
}

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
        const PreparedWave w = PrepareForSfz(*pcm);
        wave.name = "w" + std::to_string(wave_system) + "_" + std::to_string(wave_id) + ".wav";
        samples_.push_back({wave.name, WavFile(w.samples, static_cast<uint32_t>(std::lround(pcm->sample_rate)))});
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
    const int cents = static_cast<int>(std::lround(std::clamp(z.cents, -12000.0, 12000.0)));
    if (cents / 100 != 0)
    {
        s += " transpose=" + std::to_string(cents / 100);
    }
    if (cents % 100 != 0)
    {
        s += " tune=" + std::to_string(cents % 100);
    }

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

    if (e.delay > 0.0)
    {
        s += " ampeg_delay=" + Number(e.delay, 6);
    }
    if (e.attack > 0.0)
    {
        s += " ampeg_attack=" + Number(e.attack, 6);
    }
    if (e.hold > 0.0)
    {
        s += " ampeg_hold=" + Number(e.hold, 6);
    }
    if (e.sustain < 100.0)
    {
        s += " ampeg_decay=" + Number(e.decay, 6) + " ampeg_sustain=" + Number(e.sustain, 6);
    }
    s += " ampeg_release=" + Number(e.release, 6);

    if (wave.loop)
    {
        s += " loop_mode=loop_continuous loop_start=" + std::to_string(wave.loop_start) +
             " loop_end=" + std::to_string(wave.loop_end);
    }
    else
    {
        s += " loop_mode=no_loop";
    }

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

// Write track `t`'s SFZ file.
std::string TrackFile(const Performance& perf, int t, int bend_range, BankSet& banks, SampleSet& samples,
                      const std::string& title, const std::vector<std::string>& preset_names)
{
    std::set<uint32_t> presets;
    for (const TrackEvent& e : perf.tracks[static_cast<std::size_t>(t)])
    {
        if (e.kind == EventKind::kPreset)
        {
            presets.insert(e.preset);
        }
    }

    const std::string& track = perf.track_names[static_cast<std::size_t>(t)];
    const std::string track_name = track.empty() ? "Track " + std::to_string(t) : track;
    std::string s = "// " + CommentText(track_name) + " of " + CommentText(title) +
                    ", converted by dolphinsf2. Load for MIDI track \"" + CommentText(track_name) +
                    "\" and its layers.\n";
    if (perf.level_cut_db > 0.0)
    {
        s += "// MIDI gain was reduced by " + Number(perf.level_cut_db, 2) + " dB. Region volumes restore that gain.\n";
    }
    s += "\n";
    s += "<control>\ndefault_path=../" + kSfzSamplesFolder + "/\nhint_ram_based=1\n\n";

    // Map CC10 to (value - 64) / 64. That's the SoundFont pan convention that the MIDI writer uses.
    s += Curve(kPanCurve, "CC10 to pan", [](int v) { return (v - 64) / 64.0; });

    const std::string bend = Number(bend_range * 100.0 * kBendScale, 6);
    s += "<global>\nbend_up=" + bend + " bend_down=-" + bend + "\n";
    // Like the game's settings, CC7, CC10 and CC11 act at once. sfizz would otherwise smooth them over 10 ms, from CC7
    // 100 at first. A note whose track sets its volume or pan in the tick it starts would then begin at the old value.
    // Curve 4 squares CC7 and CC11. JAudio2 squares the volumes too.
    s += "pan_oncc10=100 pan_curvecc10=" + std::to_string(kPanCurve) + " pan_smoothcc10=0\n";
    s += "amplitude_oncc7=100 amplitude_curvecc7=4 amplitude_smoothcc7=0 amplitude_oncc11=100 amplitude_curvecc11=4 "
         "amplitude_smoothcc11=0\n";

    const bool banks_selected = perf.presets.size() > 128;
    for (uint32_t p : presets)
    {
        const PresetKey& key = perf.presets[p];
        s += "\n// " + CommentText(preset_names[p]) + "\n<master> loprog=" + std::to_string(p % 128) +
             " hiprog=" + std::to_string(p % 128);
        if (banks_selected)
        {
            s += " locc0=" + std::to_string(p / 128) + " hicc0=" + std::to_string(p / 128);
        }
        s += "\n" + VibratoOpcodes(key) + "<group>\n";
        for (const PresetZone& z : ZonesOf(key, perf, banks))
        {
            const SampleSet::Wave& wave = samples.Get(z.wave_system, z.wave_id);
            if (!wave.name.empty())
            {
                s += RegionLine(z, wave, perf.level_cut_db);
            }
        }
    }

    return s;
}

} // namespace

SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks, const std::string& title,
                        const std::vector<std::string>& preset_names)
{
    SfzInstruments out;
    SampleSet samples(banks);
    for (int t = 0; t < 16; t++)
    {
        if (midi.bend_ranges[static_cast<std::size_t>(t)] == 0)
        {
            continue; // no MIDI channel assigned to this track
        }

        // Each file is named after its MIDI track.
        const std::string& track = perf.track_names[static_cast<std::size_t>(t)];
        out.files.push_back(
            {FileName(track.empty() ? "Track " + std::to_string(t) : track) + ".sfz",
             TrackFile(perf, t, midi.bend_ranges[static_cast<std::size_t>(t)], banks, samples, title, preset_names)});
    }

    out.samples = samples.Take();

    return out;
}

std::vector<uint8_t> WavFile(std::span<const int16_t> samples, uint32_t rate)
{
    const auto data_size = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> out;
    out.reserve(44 + data_size);

    auto u32 = [&](uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    };
    auto u16 = [&](uint16_t v)
    {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto tag = [&](const char* t)
    {
        out.insert(out.end(), t, t + 4);
    };

    tag("RIFF");
    u32(36 + data_size);
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    tag("data");
    u32(data_size);
    for (int16_t v : samples)
    {
        u16(static_cast<uint16_t>(v));
    }

    return out;
}

} // namespace dolphinsf2
