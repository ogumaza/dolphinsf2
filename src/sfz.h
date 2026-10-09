// SPDX-License-Identifier: MIT

// SFZ instruments and WAV samples for a performance.
//
// Each sequence track gets one SFZ file containing its presets, selected by program change. SFZ players ignore MIDI
// channels. Each file serves the matching MIDI track and its layers. Output targets sfizz.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "midi.h"
#include "performance.h"

namespace dolphinsf2
{

class BankSet;

// An SFZ file for one sequence track.
struct SfzFile
{
    std::string name; // the MIDI track's name, such as "Track 3.sfz"
    std::string text;
};

// A WAV sample used by the SFZ files.
struct SfzSample
{
    std::string name; // "w<wave system>_<wave>.wav"
    std::vector<uint8_t> wav;
};

// A sequence's SFZ files and shared samples.
struct SfzInstruments
{
    std::vector<SfzFile> files;
    std::vector<SfzSample> samples;
};

// Shared sample directory, beside the sequence directories.
inline const std::string kSfzSamplesFolder = "samples";

// Build SFZ files for active tracks in `perf`. Take the bend ranges from `midi`. `title` and `preset_names` supply
// names for comments.
SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks, const std::string& title,
                        const std::vector<std::string>& preset_names);

// Returns a mono WAV file of 16-bit samples at `rate` Hz.
std::vector<uint8_t> WavFile(std::span<const int16_t> samples, uint32_t rate);

} // namespace dolphinsf2
