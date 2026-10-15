// SPDX-License-Identifier: MIT

// What an instrument or drum plays for a key and velocity, and the decoded waves of a sequence's banks.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "envelope.h"
#include "jaudio.h"
#include "wave.h"

namespace dolphinsf2
{

// What a note plays (JASInstParam): the wave and the instrument's settings for its key and velocity, with the
// instrument's effects applied.
struct NoteSound
{
    uint32_t bank = 0;
    uint32_t program = 0;
    uint32_t wave_system = 0;
    uint16_t wave_id = 0;
    const WaveInfo* wave = nullptr;
    float volume = 1.0f;                        // amplitude, before the velocity
    float pitch = 1.0f;                         // a ratio, before the key
    float pan = 0.5f;                           // 0 left, 1 right
    std::vector<const Oscillator*> oscillators; // at most two
    uint16_t direct_release = 0;                // drums: their release
    bool drum = false;                          // plays its wave at its rate whatever the key
    bool random = false;                        // a random effect would have varied the note
    bool velocity_sense = false;                // an effect follows the velocity
};

// The region of `bank`'s `program` that `key` and `velocity` play. Like JAudio2, take the first key region whose high
// key is at or above the key, then the first velocity region of it whose high velocity is at or above the velocity. A
// drum set plays the drum of the key. Return nothing if the program plays nothing there. Random effects take their base
// value: dolphinsf2 can't vary a preset note by note. The result reports that one was there.
std::optional<NoteSound> FindSound(const Bank& bank, uint32_t program, int key, int velocity);

// The keys and velocities with which one region plays, for building presets: a run of keys that find the same key
// region, and the velocity ranges of its regions.
struct PlayedRegion
{
    int low_key = 0, high_key = 127;
    int low_velocity = 0, high_velocity = 127;
    uint16_t wave_id = 0;
};

// The regions of `program` in `bank`, in key order. A drum set gives one region per key that has a drum.
std::vector<PlayedRegion> ProgramRegions(const Bank& bank, uint32_t program);

// A zone of a preset: the keys and velocities a region plays, its wave, and how it plays it.
struct PresetZone
{
    int low_key = 0, high_key = 127;
    int low_velocity = 0, high_velocity = 127;
    uint32_t wave_system = 0;
    uint16_t wave_id = 0;
    uint8_t root_key = 60; // the key that plays the wave at its rate
    double cents = 0.0;    // tuning
    double volume = 1.0;   // amplitude, before the velocity
    double pan = 0.5;      // 0 left, 1 right
    EnvelopeCurve envelope;
};

// The zones of `program` in `bank`, whose waves are in `waves`, played with `volume_override` and `pitch_override` in
// place of the instrument's oscillators (nullptr for none), with the track's `direct_release` (0 for none) and with the
// game's `default_release` (TraceEnvelope). A drum plays its wave at its rate whatever the key: its zone's root key is
// its key. An instrument whose effects follow the key gets a zone per key.
std::vector<PresetZone> PresetZones(const Bank& bank, const WaveSystem* waves, uint32_t program,
                                    const Oscillator* volume_override, const Oscillator* pitch_override,
                                    uint16_t direct_release, uint16_t default_release);

// A sequence's banks and their waves, decoded on demand. Read failures are collected in Errors() and silence only the
// notes they affect.
class BankSet
{
public:
    // Read an AW file by name from where the game keeps them. Return nothing if it can't be found.
    using AwReader = std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;

    BankSet(const SoundArchive& archive, AwReader read_aw);

    const SoundArchive& Archive() const
    {
        return archive_;
    }

    // Decode and cache wave `wave_id` of wave system `wave_system`, or return nullptr and record why.
    const Pcm* GetWave(uint32_t wave_system, uint16_t wave_id);

    // Read failures for the waves.
    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    const SoundArchive& archive_;
    AwReader read_aw_;
    std::map<std::string, std::optional<std::vector<uint8_t>>> aws_;
    std::map<std::pair<uint32_t, uint16_t>, std::unique_ptr<Pcm>> waves_;
    std::vector<std::string> errors_;
};

} // namespace dolphinsf2
