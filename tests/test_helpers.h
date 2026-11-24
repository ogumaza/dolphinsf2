// SPDX-License-Identifier: MIT

// The helpers that the unit tests share: the events of a performance, a DOL with data, and the zones of a SoundFont.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "seqlib/sf2/reader.h"

#include "jaudio.h"

namespace dolphinsf2::testing
{

// The events of `kind` on MIDI track `slot` of `perf`, in order. None if `perf` has no such track.
std::vector<TrackEvent> EventsOf(const Performance& perf, std::size_t slot, EventKind kind);

// The number of events of `kind` on MIDI track `slot` of `perf`.
int Count(const Performance& perf, std::size_t slot, EventKind kind);

// The values of the events of `kind` on MIDI track `slot` of `perf`, in order: CC values, or a note's velocity.
std::vector<int> ValuesOf(const Performance& perf, std::size_t slot, EventKind kind);

// The tick and the key of each event of `kind` on MIDI track `slot` of `perf`, in order.
std::vector<std::pair<uint32_t, int>> KeysOf(const Performance& perf, std::size_t slot, EventKind kind);

// The bends of channel 0 of `perf` other than 0, in semitones, in order.
std::vector<float> BendsOf(const Performance& perf);

// A DOL whose first data section contains `data` at load address `address`.
std::vector<uint8_t> DolWithData(uint32_t address, const std::vector<uint8_t>& data);

// The fields of modulator `m`: its source, destination, amount, amount source and transform.
std::array<int, 5> ModulatorFields(const seqlib::sf2::Modulator& m);

// The fields of the modulators of instrument zone `zone` in `records`, a SoundFont's chunks (seqlib's
// Records::ZoneModulators), in their order.
std::vector<std::array<int, 5>> ZoneModulators(const seqlib::sf2::Records& records, std::size_t zone);

// Whether a zone with generators `gens` plays key 60, the key of the test songs' notes. The test sample plays at the
// DSP's highest pitch from key 84 up. Those keys get zones of their own (TestHighestPitch).
bool PlaysKey60(const std::map<uint16_t, uint16_t>& gens);

// The values of generator `oper` in the zones of `sf2` that play a sample at key 60 (PlaysKey60), in the order of the
// zones.
std::vector<int> SampleZoneValues(const std::vector<uint8_t>& sf2, uint16_t oper);

} // namespace dolphinsf2::testing
