// SPDX-License-Identifier: MIT

// Sequence conversion to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "instruments.h"
#include "performance.h"
#include "sfz.h"

namespace dolphinsf2
{

// Converted files, performance data and statistics for the conversion report.
struct Conversion
{
    Performance performance;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    SfzInstruments sfz;
    std::size_t samples = 0; // SoundFont samples
    int channels = 0;        // MIDI channels used
    std::vector<std::string> warnings;
};

// Perform `sound` and build its MIDI, SoundFont and SFZ files from the banks and waves in `banks`. Throw FormatError if
// the sequence cannot be read.
Conversion ConvertSequence(const SequenceSound& sound, BankSet& banks, const PerformOptions& options = {});

// The vibrato's frequency at a rate of 1, in the SoundFont and the SFZ files: 72 sub-frames a cycle.
constexpr double kVibratoHz = 1.0 / (72.0 * kSubFrameSeconds);

// Preset names for both writers: the bank and program, numbered when a program has several presets.
std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys);

// The zones of preset `key` in `perf`, from the banks that `banks` reads.
std::vector<PresetZone> ZonesOf(const PresetKey& key, const Performance& perf, BankSet& banks);

} // namespace dolphinsf2
