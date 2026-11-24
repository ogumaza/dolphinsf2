// SPDX-License-Identifier: MIT

#include "musyx_test_helpers.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "seqlib/sfz/track_file.h"

#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"

namespace dolphinsf2::testing
{

ntdlib::musyx::MacroStep Step(ntdlib::musyx::MacroOp op, std::array<uint32_t, 7> b)
{
    ntdlib::musyx::MacroStep step;
    step.word0 = static_cast<uint32_t>(op) | b[0] << 8 | b[1] << 16 | b[2] << 24;
    step.word1 = b[3] | b[4] << 8 | b[5] << 16 | b[6] << 24;

    return step;
}

std::vector<ntdlib::musyx::MacroStep> FadeSteps()
{
    using ntdlib::musyx::MacroOp;
    return {
        Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
        Step(MacroOp::kEnvelope, {80, 0, 0xff, 0xff, 1, 120, 0}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 120, 0}),
        Step(MacroOp::kEnvelope, {0, 0, 0xff, 0xff, 1, 0x20, 0x03}),
        Step(MacroOp::kWaitMs, {0, 0, 0, 0, 0, 0x20, 0x03}),
        Step(MacroOp::kStopSample),
        Step(MacroOp::kEnd),
    };
}

musyx::SongGroup MakeSongGroup()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group;
    group.group.pages.push_back({.object = 1, .priority = 0, .max_voices = 8, .program = 0});
    ntdlib::musyx::MidiSetup setup;
    setup.song = 5;
    setup.channels[0].volume = 100;
    group.group.midi_setups.push_back(setup);

    // The macro, and its envelope's table. The table is little-endian. Its attack and decay of INT32_MIN take no time,
    // its sustain of 0x1000 is full level, its release takes 100 ms, and its scales of INT32_MIN are off.
    group.pool.macros[1] = {
        Step(MacroOp::kModeSelect, {1, 1}),
        Step(MacroOp::kSetAdsr, {2, 0, 1}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
        Step(MacroOp::kKeyOff),
        Step(MacroOp::kEnd),
    };
    group.pool.tables[2] = {0, 0, 0, 0x80, 0, 0, 0, 0x80, 0x00, 0x10, 100, 0, 0, 0, 0, 0x80, 0, 0, 0, 0x80};

    ntdlib::musyx::SampleInfo sample;
    sample.id = 1;
    sample.key = 60;
    sample.rate = 32000;
    sample.format = ntdlib::musyx::SampleFormat::kPcm16;
    sample.length = 3200;
    sample.loop_start = 800;
    sample.loop_length = 2400;
    group.samples.push_back(sample);
    ntdlib::testing::BigEndianBytes data;
    for (uint32_t i = 0; i < sample.length; i++)
    {
        data.U16(static_cast<uint16_t>(std::lround(8000.0 * std::sin(i * 2.0 * std::numbers::pi / 80.0))));
    }

    group.sample_data = data.Data();

    return group;
}

musyx::VolumeTable SampleVolumes()
{
    musyx::VolumeTable table{};
    for (std::size_t i = 0; i < 128; i++)
    {
        const double x = static_cast<double>(i) / 127.0;
        table[i] = static_cast<float>(x * x * (0.5 + 0.5 * x));
    }
    table[128] = 1.0f;

    return table;
}

ntdlib::musyx::Song OneTrackSong(bool loops, bool volume_change)
{
    ntdlib::musyx::Song song;
    song.tempo = 120;
    ntdlib::musyx::Track track;
    track.entries.push_back({.tick = 0, .pattern = 0, .volume = 100});
    track.entries.push_back(
        {.tick = 768, .pattern = loops ? ntdlib::musyx::kLoopBack : ntdlib::musyx::kEndOfTrack, .loop_target = 0});
    song.tracks.push_back(track);

    ntdlib::musyx::Pattern pattern;
    pattern.events.push_back(
        {.tick = 0, .kind = ntdlib::musyx::PatternEventKind::kNote, .key = 60, .velocity = 100, .length = 192});
    if (volume_change)
    {
        pattern.events.push_back(
            {.tick = 192, .kind = ntdlib::musyx::PatternEventKind::kController, .controller = 7, .value = 64});
    }

    pattern.end_tick = 768;
    song.patterns.push_back(pattern);

    return song;
}

std::vector<std::string> OpcodeValues(const Conversion& c, const std::string& opcode)
{
    const std::string key = " " + opcode + "=";
    std::vector<std::string> values;
    for (const seqlib::sfz::File& f : c.sfz.files)
    {
        for (std::size_t at = f.text.find(key); at != std::string::npos; at = f.text.find(key, at + 1))
        {
            const std::size_t region = f.text.rfind("<region>", at);
            const std::size_t lokey = f.text.find(" lokey=", region);
            if (region != std::string::npos && lokey != std::string::npos && lokey < at &&
                std::stoi(f.text.substr(lokey + 7)) > 60)
            {
                continue;
            }

            const std::size_t start = at + key.size();
            values.push_back(f.text.substr(start, f.text.find_first_of(" \n", start) - start));
        }
    }

    return values;
}

} // namespace dolphinsf2::testing
