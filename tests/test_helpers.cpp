// SPDX-License-Identifier: MIT

#include "test_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "ntdlib/testing/dol_files.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"

#include "jaudio.h"

namespace dolphinsf2::testing
{

std::vector<TrackEvent> EventsOf(const Performance& perf, std::size_t slot, EventKind kind)
{
    std::vector<TrackEvent> events;
    if (slot >= perf.tracks.size())
    {
        return events;
    }

    for (const TrackEvent& e : perf.tracks[slot])
    {
        if (e.kind == kind)
        {
            events.push_back(e);
        }
    }

    return events;
}

int Count(const Performance& perf, std::size_t slot, EventKind kind)
{
    return static_cast<int>(EventsOf(perf, slot, kind).size());
}

std::vector<int> ValuesOf(const Performance& perf, std::size_t slot, EventKind kind)
{
    std::vector<int> values;
    for (const TrackEvent& e : EventsOf(perf, slot, kind))
    {
        values.push_back(e.value);
    }

    return values;
}

std::vector<std::pair<uint32_t, int>> KeysOf(const Performance& perf, std::size_t slot, EventKind kind)
{
    std::vector<std::pair<uint32_t, int>> keys;
    for (const TrackEvent& e : EventsOf(perf, slot, kind))
    {
        keys.emplace_back(e.tick, e.key);
    }

    return keys;
}

std::vector<float> BendsOf(const Performance& perf)
{
    std::vector<float> bends;
    for (const TrackEvent& e : EventsOf(perf, 0, EventKind::kPitch))
    {
        if (e.semitones != 0.0f)
        {
            bends.push_back(e.semitones);
        }
    }

    return bends;
}

std::vector<uint8_t> DolWithData(uint32_t address, const std::vector<uint8_t>& data)
{
    constexpr std::size_t kHeader = 0x100;
    std::vector<uint8_t> dol(kHeader);
    ntdlib::testing::SetDolSection(dol, 7, static_cast<uint32_t>(kHeader), address, static_cast<uint32_t>(data.size()));
    dol.insert(dol.end(), data.begin(), data.end());

    return dol;
}

std::array<int, 5> ModulatorFields(const seqlib::sf2::Modulator& m)
{
    return {m.source, m.dest, m.amount, m.amount_source, m.transform};
}

std::vector<std::array<int, 5>> ZoneModulators(const seqlib::sf2::Records& records, std::size_t zone)
{
    std::vector<std::array<int, 5>> modulators;
    for (const seqlib::sf2::Modulator& m : records.ZoneModulators(zone))
    {
        modulators.push_back(ModulatorFields(m));
    }

    return modulators;
}

bool PlaysKey60(const std::map<uint16_t, uint16_t>& gens)
{
    return !gens.contains(seqlib::sf2::kKeyRange) ||
           ((gens.at(seqlib::sf2::kKeyRange) & 0xff) <= 60 && (gens.at(seqlib::sf2::kKeyRange) >> 8) >= 60);
}

std::vector<int> SampleZoneValues(const std::vector<uint8_t>& sf2, uint16_t oper)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<int> values;
    for (std::size_t zone = 0; zone + 1 < records.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(zone);
        if (gens.contains(seqlib::sf2::kSampleId) && gens.contains(oper) && PlaysKey60(gens))
        {
            values.push_back(static_cast<int16_t>(gens.at(oper)));
        }
    }

    return values;
}

} // namespace dolphinsf2::testing
