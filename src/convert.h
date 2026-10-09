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

// Perform `sound` and build its MIDI, SoundFont and SFZ files from the banks and waves in `banks`. Throw FormatError
// if the sequence cannot be read.
Conversion ConvertSequence(const SequenceSound& sound, BankSet& banks, const PerformOptions& options = {});

// Make a sequence name safe for file and directory names. Replace characters forbidden by Windows and trailing
// dots/spaces with '_'. Append '_' to Windows device names such as CON or NUL. Used for the MIDI/SoundFont base name
// and the SFZ directory.
std::string FileName(const std::string& sequence_name);

// SoundFont and SFZ settings shared by both writers.

// The vibrato depth that CC1's full value gives, in cents. CC1 holds 128 times the depth over this.
constexpr double kVibratoRangeCents = 100.0;

// The vibrato's frequency at a rate of 1: 72 sub-frames a cycle.
constexpr double kVibratoHz = 1.0 / (72.0 * kSubFrameSeconds);

// Preset names: the bank and program, numbered when a program has several presets.
std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys);

// The zones of preset `key` in `perf`, from the banks that `banks` reads.
std::vector<PresetZone> ZonesOf(const PresetKey& key, const Performance& perf, BankSet& banks);

} // namespace dolphinsf2
