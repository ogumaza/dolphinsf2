// SPDX-License-Identifier: MIT

// The SoundFont and the SFZ files of a conversion. The conversions of the MusyX games, the games of the N64 library and
// the pieces of Red build them here.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "seqlib/sf2/soundfont.h"

#include "jaudio.h"

namespace dolphinsf2
{

// A zone of a preset: its sample, and how the SoundFont's zone and the SFZ region play it. The two files share the
// sample's name.
struct InstrumentZone
{
    std::string sample;
    uint8_t root_key = 60;           // the key at which the SoundFont's sample plays at its pitch
    std::function<const Pcm*()> pcm; // the decoded sample, or nullptr if it can't be decoded
    std::function<seqlib::sf2::Zone(const SoundFontSamples::Sample&)> soundfont;
    std::function<std::string(const SfzSamples::Wave&)> sfz; // the line of the region
};

// Write the SoundFont `sf` of conversion `c`, titled `title`. Its comment says that the files come from `source`, such
// as "song title_song".
void WriteSoundFontFile(Conversion& c, seqlib::sf2::SoundFont& sf, const std::string& title, const std::string& source);

// The SFZ files of `perf`, titled `title`. `midi` describes the MIDI file of `perf`. Preset p takes the name
// `names[p]`. `group(p, samples)` gives the text of its group, with the samples that it takes from `samples`.
SfzInstruments BuildSfzFiles(const Performance& perf, const MidiReport& midi, const std::string& title,
                             const std::vector<std::string>& names,
                             const std::function<std::string(uint32_t, SfzSamples&)>& group);

// Build the SoundFont and the SFZ files of conversion `c`, titled `title`. `midi` describes the MIDI file of `c`. The
// comment of the SoundFont says that the files come from `source`. Preset i takes the name `names[i]`, the global zone
// `global` and the zones `zones[i]`. Collect the warnings of the files too.
void WriteInstrumentFiles(Conversion& c, const MidiReport& midi, const std::string& title, const std::string& source,
                          const std::vector<std::string>& names, const seqlib::sf2::Zone& global,
                          const std::vector<std::vector<InstrumentZone>>& zones);

// The volume opcode of an SFZ region that plays `volume_db` above its sample, at least kSfzLowestVolumeDb. A volume
// that rounds to 0 takes none.
std::string SfzVolume(double volume_db);

} // namespace dolphinsf2
