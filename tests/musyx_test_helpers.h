// SPDX-License-Identifier: MIT

// The pieces of MusyX song groups that the MusyX tests build: SoundMacro steps, a song group with one program, a volume
// table, a song of one track, and the opcodes of the SFZ files.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/song.h"

#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"

namespace dolphinsf2::testing
{

// A SoundMacro step with command `op` and bytes 1 to 7 of its two words.
ntdlib::musyx::MacroStep Step(ntdlib::musyx::MacroOp op, std::array<uint32_t, 7> b = {});

// The steps of a fade after the note's release, as the NDDEMO disc's macros make it: wait for the release, ramp the
// volume to 80/128 of itself in 120 ms, ramp it to 0 in 800 ms, then stop the sample.
std::vector<ntdlib::musyx::MacroStep> FadeSteps();

// A song group with one program. Program 0 plays macro 1. The macro picks the DLS volume curve without ITD, sets DLS
// envelope 2, starts sample 1, waits for the note's release and releases the envelope. Envelope 2 starts at full level
// at once, holds it and releases over 100 ms. Sample 1 is a PCM16 sine of 3200 samples at 32000 Hz, at key 60, that
// loops from sample 800 to its end. Song 5's MIDI setup gives channel 0 a volume of 100.
musyx::SongGroup MakeSongGroup();

// A volume table for the tests. Its entries rise from 0 to 1. The table isn't a power of the volume: the level of a
// product isn't the product of the levels.
musyx::VolumeTable SampleVolumes();

// A song of one track on channel 0. The track plays pattern 0 at tick 0 with a volume of 100, then loops back to it or
// ends at tick 768. The pattern plays key 60 for half a beat. With `volume_change`, it then sets the channel's volume
// to 64.
ntdlib::musyx::Song OneTrackSong(bool loops, bool volume_change);

// The values of `opcode`, such as "volume", in the SFZ files of `c`, in the order of the files and their regions. The
// regions from key 84 up play the test sample at the DSP's highest pitch. Every region whose lowest key is above 60 is
// left out.
std::vector<std::string> OpcodeValues(const Conversion& c, const std::string& opcode);

} // namespace dolphinsf2::testing
