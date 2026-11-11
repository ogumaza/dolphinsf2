// SPDX-License-Identifier: MIT

// The conversion of a piece of music of the Nintendo 64's sound library (see n64_convert.h).
//
// Each preset plays an instrument of a bank, the bank's drums or its effects (PresetVoice). An instrument's preset has
// a zone for each of its samples: the low one, the normal one and the high one, over the keys of their semitones. A
// preset of drums or effects has a zone for each drum or effect, on a key of its own. Each zone plays its sample at the
// library's base rate (kSampleRate) and its tuning, with the preset's envelope and release, or the drum's.

#include "n64_convert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/conversion.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"

#include "instrument_files.h"
#include "output_units.h"

namespace dolphinsf2::n64
{
namespace
{

namespace sf2 = seqlib::sf2;

using seqlib::sfz::Number;

// The milliseconds of one of the library's updates.
constexpr double kUpdateMs = 1000.0 / kUpdatesPerSecond;

// The lowest level that a release counts from.
constexpr double kLeastReleaseLevel = 0.1;

// A zone of a preset: its keys, the key at which it plays its sample at its tuning, its pan (-1 left to 1 right, which
// CC10's pan adds to), and its envelope and release.
struct Zone
{
    int low = 0;
    int high = 127;
    int root = 60;
    const Sample* sample = nullptr;
    float tuning = 1.0f;
    double pan = 0.0;
    std::vector<EnvelopePoint> envelope;
    uint8_t release = 0;
    VolumeShape shape = {}; // the envelope and the release for the median length of the zone's notes (ShapeOf)
};

// The length of each note of a performance in milliseconds, by its preset and its key.
using NoteLengths = std::map<std::pair<uint32_t, int>, std::vector<double>>;

NoteLengths NoteLengthsOf(const Performance& perf)
{
    const auto ms_at = [&perf](uint32_t tick)
    {
        double ms = 0.0;
        for (std::size_t i = 0; i < perf.tempo.size() && perf.tempo[i].tick < tick; i++)
        {
            const uint32_t end = i + 1 < perf.tempo.size() ? std::min(perf.tempo[i + 1].tick, tick) : tick;
            ms += (end - perf.tempo[i].tick) * 60000.0 / (perf.tempo[i].bpm * perf.timebase);
        }

        return ms;
    };

    NoteLengths lengths;
    for (const std::vector<TrackEvent>& track : perf.tracks)
    {
        std::map<uint8_t, uint32_t> presets;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> sounding;
        for (const TrackEvent& e : track)
        {
            if (e.kind == EventKind::kPreset)
            {
                presets[e.part] = e.preset;
            }
            else if (e.kind == EventKind::kNoteOn)
            {
                sounding[e.note] = {e.tick, presets[e.part]};
            }
            else if (e.kind == EventKind::kNoteOff)
            {
                const auto found = sounding.find(e.note);
                if (found != sounding.end())
                {
                    const auto [start, preset] = found->second;
                    lengths[{preset, e.key}].push_back(ms_at(e.tick) - ms_at(start));
                    sounding.erase(found);
                }
            }
        }
    }

    return lengths;
}

// The median length of the notes of preset `preset` on keys `low` to `high`. A zone with no notes takes kUsualNoteMs.
double MedianNoteMs(const NoteLengths& lengths, uint32_t preset, int low, int high)
{
    std::vector<double> all;
    for (auto it = lengths.lower_bound({preset, low}); it != lengths.end() && it->first <= std::make_pair(preset, high);
         ++it)
    {
        all.insert(all.end(), it->second.begin(), it->second.end());
    }
    if (all.empty())
    {
        return kUsualNoteMs;
    }
    const auto middle = all.begin() + static_cast<std::ptrdiff_t>(all.size() / 2);
    std::nth_element(all.begin(), middle, all.end());

    return *middle;
}

// The pan of a note's part (-1 to 1): its pan from 0 to 127, of which the channel's pan weight leaves 128 - `weight`
// 128ths. The channel's CC10 gives the rest. The library's pans run on the same law as RP-036's from 1 to 127.
double NotePan(uint8_t pan, uint8_t weight)
{
    const double share = (static_cast<double>(pan) - 64.0) * (128.0 - weight) / 128.0;
    return share * (126.0 / 127.0) / 64.0;
}

// The zones of `voice`, from `data`'s banks.
std::vector<Zone> ZonesOf(const PresetVoice& voice, SoundData& data)
{
    const Bank* bank = data.GetBank(voice.bank);
    if (bank == nullptr)
    {
        return {};
    }

    std::vector<Zone> zones;
    switch (voice.kind)
    {
    case VoiceKind::kInstrument:
        {
            if (voice.program >= bank->instruments.size() || !bank->instruments[voice.program])
            {
                break;
            }

            // The semitones below `low` play the low sample, those above `high` the high one.
            const Instrument& inst = *bank->instruments[voice.program];
            const double pan = NotePan(voice.note_pan, voice.pan_weight);
            const int normal_low = inst.low == 0 ? 0 : KeyOf(inst.low);
            const int normal_high = inst.high == 127 ? 127 : std::min(KeyOf(inst.high), 127);
            if (inst.low != 0)
            {
                zones.push_back({0, normal_low - 1, 60, inst.low_sample.sample, inst.low_sample.tuning, pan,
                                 voice.envelope, voice.release});
            }
            zones.push_back({normal_low, normal_high, 60, inst.normal_sample.sample, inst.normal_sample.tuning, pan,
                             voice.envelope, voice.release});
            if (inst.high != 127 && normal_high < 127)
            {
                zones.push_back({normal_high + 1, 127, 60, inst.high_sample.sample, inst.high_sample.tuning, pan,
                                 voice.envelope, voice.release});
            }
            break;
        }

    case VoiceKind::kDrums:
        for (std::size_t i = 0; i < bank->drums.size() && i < 128; i++)
        {
            if (!bank->drums[i])
            {
                continue;
            }

            // A drum with a release takes its envelope and release. The others take the channel's.
            const Drum& drum = *bank->drums[i];
            const int key = static_cast<int>(i);
            const bool has_release = drum.release != 0;
            zones.push_back({key, key, key, drum.sample.sample, drum.sample.tuning, NotePan(drum.pan, voice.pan_weight),
                             has_release ? drum.envelope : voice.envelope, has_release ? drum.release : voice.release});
        }
        break;

    case VoiceKind::kEffects:
        for (std::size_t n = 0; n < bank->effects.size(); n++)
        {
            if ((n >> 7) != voice.program)
            {
                continue;
            }

            const int key = EffectKey(static_cast<uint32_t>(n));
            zones.push_back({key, key, key, bank->effects[n].sample, bank->effects[n].tuning,
                             NotePan(voice.note_pan, voice.pan_weight), voice.envelope, voice.release});
        }
        break;
    }

    std::erase_if(zones, [](const Zone& z) { return z.sample == nullptr || z.low > z.high; });

    return zones;
}

// The cents of a zone's tuning, held to SoundFont's range.
int Cents(double tuning)
{
    return ClampedCents(1200.0 * std::log2(tuning));
}

// A zone's name in the SoundFont's samples: the bank and the sample's offset in it.
std::string SampleName(uint8_t bank, const Sample& sample)
{
    return "b" + std::to_string(bank) + "_" + std::to_string(sample.id);
}

// The name of each preset: its bank and its instrument, drums or effects.
std::vector<std::string> PresetNamesOf(const std::vector<PresetVoice>& voices)
{
    std::vector<std::string> names;
    for (std::size_t i = 0; i < voices.size(); i++)
    {
        const PresetVoice& v = voices[i];
        std::string name = "Bank " + std::to_string(v.bank);
        switch (v.kind)
        {
        case VoiceKind::kInstrument:
            name += " instrument " + std::to_string(v.program);
            break;
        case VoiceKind::kDrums:
            name += " drums";
            break;
        case VoiceKind::kEffects:
            name += " effects " + std::to_string(v.program);
            break;
        }
        names.push_back(name + " (" + std::to_string(i) + ")");
    }

    return names;
}

// The rate of `voice`'s vibrato in hertz, and its delay in milliseconds.
double VibratoHz(const PresetVoice& voice)
{
    return voice.vibrato_rate * kUpdatesPerSecond / 65536.0;
}

double VibratoDelayMs(const PresetVoice& voice)
{
    return voice.vibrato_delay * kUpdateMs;
}

// The SoundFont zone of `z` for `voice`, with `sample`, at `quieter` centibels below its level. A voice with a
// vibrato takes its rate and delay. CC1 gives its depth.
sf2::Zone SoundFontZone(const PresetVoice& voice, const Zone& z, const SoundFontSamples::Sample& sample, double quieter)
{
    const VolumeShape& shape = z.shape;
    const int cents = Cents(z.tuning);
    sf2::ZoneValues values;
    values.keys = {z.low, z.high};
    values.attenuation = -200.0 * std::log10(std::max(shape.peak, 1e-9)) + quieter;
    values.pan = static_cast<int>(std::lround(z.pan * kPanRange));
    SetEnvelope(shape, values);
    values.root_key = z.root;
    values.coarse_tune = cents / 100;
    values.fine_tune = cents % 100;
    values.loop = sample.loop;
    values.sample = sample.id;
    sf2::Zone zone = sf2::RegionZone(values);

    // The vibrato, at the voice's rate and after its delay. The library's swing starts down.
    if (voice.vibrato_rate != 0)
    {
        sf2::AddGenerator(zone, sf2::kFreqVibLfo, sf2::LfoFrequency(VibratoHz(voice)));
        if (voice.vibrato_delay != 0)
        {
            sf2::AddGenerator(zone, sf2::kDelayVibLfo,
                              sf2::ToTimecents(VibratoDelayMs(voice), kShortestTimecents, kLongestHoldTimecents));
        }
        zone.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, sf2::ToAmount(-kVibratoRangeCents), 0});
    }

    return zone;
}

// The SoundFont's global zone: seqlib's kGlobalZoneModulators, and CC1 with no vibrato of its own.
sf2::Zone GlobalZone()
{
    sf2::Zone global;
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
    global.modulators.push_back({sf2::kSrcModWheel, sf2::kVibLfoToPitch, 0, 0});

    return global;
}

// The SFZ region of `z` with `wave`, `gain_db` above its level.
std::string SfzRegion(const PresetVoice& voice, const Zone& z, const SfzSamples::Wave& wave, double gain_db)
{
    const VolumeShape& shape = z.shape;
    std::string s = "<region> sample=" + wave.name + " lokey=" + std::to_string(z.low) +
                    " hikey=" + std::to_string(z.high) + " pitch_keycenter=" + std::to_string(z.root);
    s += seqlib::sfz::TuneOpcodes(Cents(z.tuning));
    s += SfzVolume(20.0 * std::log10(std::max(shape.peak, 1e-9)) + gain_db);
    if (std::abs(z.pan) >= 0.0005)
    {
        s += " pan=" + Number(z.pan * 100.0, 1);
    }

    s += seqlib::sfz::AmpEnvelopeOpcodes(SfzEnvelope(shape));
    s += seqlib::sfz::LoopOpcodes(wave.loop, wave.loop_start, wave.loop_end);
    if (voice.vibrato_rate != 0)
    {
        s += " lfo1_wave=1 lfo1_freq=" + Number(VibratoHz(voice), 6);
        if (voice.vibrato_delay != 0)
        {
            s += " lfo1_delay=" + Number(VibratoDelayMs(voice) / 1000.0, 6);
        }
        s += " lfo1_pitch_oncc1=" + Number(-kVibratoRangeCents, 3);
    }

    return s + "\n";
}

} // namespace

VolumeShape ShapeOf(const std::vector<EnvelopePoint>& envelope, uint8_t release, double note_ms)
{
    // The envelope's curve: the level at the end of each fade, from silence at the start.
    FadeCurve curve = {{0.0, 0.0}};
    double time = 0.0;
    bool ends = false;
    for (const EnvelopePoint& p : envelope)
    {
        if (p.delay > 0)
        {
            time += p.delay * kUpdateMs;
            curve.push_back({time, std::pow(static_cast<double>(p.value) / 32767.0, 2.0)});
            continue;
        }

        if (p.delay == kEnvelopeSetValue)
        {
            continue;
        }
        ends = p.delay == kEnvelopeEnd;
        break;
    }
    if (ends)
    {
        curve.push_back({time, 0.0});
    }
    VolumeShape shape = FitFades(curve);

    // The release falls linearly in amplitude, from the note's level to silence in its frames times that level. The
    // fit counts from the level at `note_ms`.
    const double frames = ReleaseFrames(release);
    if (frames > 0.0 && shape.peak > 0.0)
    {
        const double held =
            std::clamp(FadeAmplitude(curve, shape.attack_ms + note_ms) / shape.peak, kLeastReleaseLevel, 1.0);
        shape.release_db_per_ms = FitRelease(held * frames * 1000.0 / kFramesPerSecond);
    }

    return shape;
}

Conversion ConvertPiece(const Piece& piece, SoundData& data, const PerformOptions& options)
{
    Conversion c;
    Played played = PlayPiece(piece, data, options);
    c.performance = std::move(played.performance);
    const MidiReport midi = ntdlib::sequence_export::WriteMidiFile(c, piece.name);
    const std::vector<std::string> names = PresetNamesOf(played.voices);

    // Each preset's zones. SoundFont zones can't amplify their samples. A zone above full level makes every zone
    // quieter. SFZ keeps the levels.
    const NoteLengths lengths = NoteLengthsOf(c.performance);
    std::vector<std::vector<Zone>> zones;
    double loudest = 0.0;
    for (const PresetVoice& voice : played.voices)
    {
        zones.push_back(ZonesOf(voice, data));
        const auto preset = static_cast<uint32_t>(zones.size() - 1);
        for (Zone& z : zones.back())
        {
            z.shape = ShapeOf(z.envelope, z.release, MedianNoteMs(lengths, preset, z.low, z.high));
            loudest = std::max(loudest, z.shape.peak);
        }
    }
    const double quieter = ntdlib::sequence_export::CutAboveFullLevel(c, loudest);

    // The files: each zone plays its sample as its bank names it.
    const double level_cut_db = c.performance.level_cut_db;
    std::vector<std::vector<InstrumentZone>> files(zones.size());
    for (std::size_t i = 0; i < zones.size(); i++)
    {
        const PresetVoice& voice = played.voices[i];
        for (const Zone& z : zones[i])
        {
            const Sample* sample = z.sample;
            files[i].push_back({.sample = SampleName(voice.bank, *sample),
                                .root_key = static_cast<uint8_t>(z.root),
                                .pcm = [sample] { return &sample->pcm; },
                                .soundfont = [&voice, &z, quieter](const SoundFontSamples::Sample& added)
                                { return SoundFontZone(voice, z, added, quieter); },
                                .sfz = [&voice, &z, level_cut_db](const SfzSamples::Wave& wave)
                                { return SfzRegion(voice, z, wave, level_cut_db); }});
        }
    }
    WriteInstrumentFiles(c, midi, piece.name, piece.name, names, GlobalZone(), files);

    return c;
}

} // namespace dolphinsf2::n64
