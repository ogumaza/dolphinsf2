// SPDX-License-Identifier: MIT

// The SoundFont and the SFZ files of a conversion (see instrument_files.h).

#include "instrument_files.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/sfz_files.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/track_file.h"

#include "output_units.h"

namespace dolphinsf2
{

void WriteSoundFontFile(Conversion& c, seqlib::sf2::SoundFont& sf, const std::string& title, const std::string& source)
{
    const std::string comment = "Converted by " + std::string(kProgram) + " from " + source + ".";
    ntdlib::sequence_export::WriteSoundFont(c, sf, title, comment, kProgram);
}

seqlib::sf2::Zone NoModVibratoGlobalZone()
{
    seqlib::sf2::Zone global;
    global.modulators.assign(seqlib::sf2::kGlobalZoneModulatorsNoModVibrato.begin(),
                             seqlib::sf2::kGlobalZoneModulatorsNoModVibrato.end());

    return global;
}

SfzInstruments BuildSfzFiles(const Performance& perf, const MidiReport& midi, const std::string& title,
                             const std::vector<std::string>& names,
                             const std::function<std::string(uint32_t, SfzSamples&)>& group)
{
    // ntdlib's options count the presets of the performance. A performance of Red's pieces lists none. The names give
    // the count of every conversion.
    seqlib::sfz::TrackFileOptions options =
        ntdlib::sequence_export::SfzTrackFileOptions(perf, title, names, std::string(kProgram));
    options.presets = names.size();

    // Each track's presets, with the samples that they share.
    SfzSamples samples;
    const auto text = [&](int)
    {
        seqlib::sfz::TrackText added;
        added.preset = [&](uint32_t p)
        {
            return group(p, samples);
        };

        return added;
    };
    SfzInstruments out;
    out.files = seqlib::sfz::TrackFiles(perf, midi, options, text);
    out.samples = samples.Take();

    return out;
}

void WriteInstrumentFiles(Conversion& c, const MidiReport& midi, const std::string& title, const std::string& source,
                          const std::vector<std::string>& names, const seqlib::sf2::Zone& global,
                          const std::vector<std::vector<InstrumentZone>>& zones)
{
    // Each preset's instrument: the global zone, then a zone for each sample that decodes.
    seqlib::sf2::SoundFont sf;
    SoundFontSamples samples(sf);
    for (std::size_t i = 0; i < zones.size(); i++)
    {
        seqlib::sf2::Instrument inst;
        inst.name = names[i];
        inst.zones.push_back(global);
        for (const InstrumentZone& z : zones[i])
        {
            if (const std::optional<SoundFontSamples::Sample> added = samples.Get(z.sample, z.root_key, z.pcm))
            {
                inst.zones.push_back(z.soundfont(*added));
            }
        }
        seqlib::sf2::AddInstrumentPreset(sf, static_cast<uint32_t>(i), names[i], std::move(inst));
    }

    std::vector<std::string> warnings;
    for (const uint32_t preset : midi.channel_10_presets)
    {
        seqlib::sf2::AddChannel10Copy(sf, preset, warnings);
    }

    WriteSoundFontFile(c, sf, title, source);

    const auto group = [&zones](uint32_t p, SfzSamples& waves)
    {
        std::string s = "<group>\n";
        for (const InstrumentZone& z : zones[p])
        {
            const SfzSamples::Wave& wave = waves.Get(z.sample, z.pcm);
            if (!wave.name.empty())
            {
                s += z.sfz(wave);
            }
        }

        return s;
    };
    c.sfz = BuildSfzFiles(c.performance, midi, title, names, group);

    ntdlib::sequence_export::CollectWarnings(c, midi, warnings, {});
}

std::string SfzVolume(double volume_db)
{
    return seqlib::sfz::VolumeOpcode(std::max(volume_db, kSfzLowestVolumeDb));
}

} // namespace dolphinsf2
