// SPDX-License-Identifier: MIT

// SFZ instruments and WAV samples for a performance.
//
// Each sequence track gets one SFZ file containing its presets, selected by program change. SFZ players ignore MIDI
// channels. Each file serves the matching MIDI track and its layers. Output targets sfizz.

#pragma once

#include <string>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "seqlib/sfz/track_file.h"

#include "performance.h"

namespace dolphinsf2
{

class BankSet;

// What seqlib's MIDI writer reports about a file, such as each track's pitch bend range.
using seqlib::midi::MidiReport;

// A sequence's SFZ files, named after their MIDI tracks, such as "Track 3.sfz", and the samples that they share, named
// "w<wave system>_<wave>.wav" (seqlib/sfz/track_file.h).
using SfzSample = seqlib::sfz::Sample;
using SfzInstruments = seqlib::sfz::Instruments;

// Shared sample directory, beside the sequence directories.
inline const std::string& kSfzSamplesFolder = seqlib::sfz::kSamplesFolder;

// Build SFZ files for active tracks in `perf`. Take the bend ranges from `midi`. `title` and `preset_names` supply
// names for comments.
SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks, const std::string& title,
                        const std::vector<std::string>& preset_names);

} // namespace dolphinsf2
