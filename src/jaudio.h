// SPDX-License-Identifier: MIT

// Readers for JAudio2's sound data: the start-up archive (BAA) with its sound table, sound names, sequence collection,
// wave systems (WSYS) and instrument banks (IBNK), and the sequence archive (an RARC of BMS files) beside it. The
// layouts are the ones that JAudio2 reads in Four Swords Adventures (2004), the earliest game known to use it: banks of
// both versions, instruments with effects and velocity regions, and wave systems that name their AW files.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace dolphinsf2
{

// Sample formats of a wave (JASWaveInfo's format).
enum class WaveFormat : uint8_t
{
    kAdpcm4 = 0, // 4-bit ADPCM, 9 bytes per 16 samples (AFC)
    kAdpcm2 = 1, // 2-bit ADPCM, 5 bytes per 16 samples
    kPcm8 = 2,
    kPcm16 = 3,
};

// A wave of a wave system: its format, the key it plays at its rate, and where its samples are in its AW file.
struct WaveInfo
{
    WaveFormat format = WaveFormat::kAdpcm4;
    uint8_t base_key = 60;
    float sample_rate = 32000.0f;
    uint32_t offset = 0; // in the AW file
    uint32_t size = 0;
    bool loop = false;
    uint32_t loop_start = 0; // first sample of the loop
    uint32_t loop_end = 0;   // one past its last sample
    uint32_t sample_count = 0;

    // An ADPCM decoder's history at the loop start: the samples before it. The DSP program takes them up again at each
    // loop.
    int16_t loop_last = 0;
    int16_t loop_penult = 0;

    std::string archive; // the AW file's name
};

// A wave system (WSYS): the waves a bank plays, by wave id.
struct WaveSystem
{
    uint32_t number = 0; // the number that banks name it by
    std::map<uint16_t, WaveInfo> waves;
};

// A point of an oscillator's table (JASOscillator::Point). Modes 0-3 move to the value along a curve over the time
// (linear, square, square root or the "sample cell" curve). Mode 13 loops to the point that the value gives, 14 holds
// and 15 stops.
struct OscPoint
{
    int16_t mode = 0;
    int16_t time = 0;
    int16_t value = 0;
};

// An oscillator (JASOscillator::Data): an envelope that moves a note's volume, pitch, pan or effect send over time.
struct Oscillator
{
    uint8_t target = 0;            // 0 volume, 1 pitch, 2 pan, 3 effect send, 4 surround
    float rate = 1.0f;             // the table's speed
    std::vector<OscPoint> attack;  // played from note-on; empty for none
    std::vector<OscPoint> release; // played from note-off; empty for none

    // The release table is the attack table (a version 1 bank names the same offset for both). A release then carries
    // on along the table from where the attack is.
    bool release_is_attack = false;

    float scale = 1.0f;
    float vertex = 0.0f;
};

// An effect of an instrument or a drum: a value that multiplies a note's volume or pitch, or moves its pan, effect send
// or surround (JASInstRand and JASInstSense).
struct InstEffect
{
    enum class Kind : uint8_t
    {
        kRandom, // base plus a random amount up to width either way
        kSense,  // from `low` at 0 to `high` at 127 of the key or velocity, through 1 at `center`
    };

    Kind kind = Kind::kSense;
    uint8_t target = 0; // 0 volume, 1 pitch, 2 pan, 3 effect send, 4 surround

    // kRandom
    float base = 1.0f;
    float width = 0.0f;

    // kSense
    uint8_t trigger = 0; // 1 velocity, 2 key
    uint8_t center = 0;
    float low = 1.0f;
    float high = 1.0f;
};

// The wave that a range of velocities plays, up to `high_velocity`.
struct VelocityRegion
{
    uint8_t high_velocity = 127;
    uint16_t wave_id = 0;
    float volume = 1.0f;
    float pitch = 1.0f;
};

// The velocity regions that a range of keys plays, up to `high_key`.
struct KeyRegion
{
    uint8_t high_key = 127;
    std::vector<VelocityRegion> velocities;
};

// An instrument (JASBasicInst).
struct Instrument
{
    float volume = 1.0f;
    float pitch = 1.0f;
    std::vector<std::size_t> oscillators; // into Bank::oscillators, at most two
    std::vector<InstEffect> effects;
    std::vector<KeyRegion> keys;
};

// A drum set's sound for one key (JASDrumSet::TPerc). A drum plays its wave at its rate whatever the key.
struct Drum
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    uint16_t release = 1000; // a direct release: its time in the low 14 bits, its curve in the top two
    std::vector<InstEffect> effects;
    std::vector<VelocityRegion> velocities;
};

// A drum set (JASDrumSet).
struct DrumSet
{
    std::array<std::optional<Drum>, 128> keys;
};

// An instrument bank (IBNK).
struct Bank
{
    // Read the bank in `file`. The bank plays the waves of wave system `wave_system`.
    static Bank Parse(std::span<const uint8_t> file, uint32_t wave_system);

    uint32_t number = 0;      // the number a sequence selects the bank by
    uint32_t wave_system = 0; // the number of the wave system it plays
    uint32_t version = 0;
    std::vector<Oscillator> oscillators;
    std::map<uint32_t, std::variant<Instrument, DrumSet>> programs; // drum sets from 0xe4 in version 0 banks
};

// A sound of the sound table and the sequence it plays.
struct SoundEntry
{
    enum class Source : uint8_t
    {
        kCollection, // a sequence of the archive's sequence collection
        kArchive,    // a file of the sequence archive
    };

    std::string name; // from the name table, or made up from the sound's place
    uint8_t section = 0, group = 0;
    uint16_t index = 0;
    uint8_t volume = 127;
    Source source = Source::kCollection;
    uint32_t resource = 0; // the collection's sequence, or the archive's file id
};

// The start-up archive (BAA): the sound table and its names, the sequence collection, the wave systems and the banks.
class SoundArchive
{
public:
    // Read a BAA file. Throw FormatError if it isn't one, or is damaged.
    static SoundArchive Parse(std::vector<uint8_t> baa);

    // The archive's sounds that play sequences, in the sound table's order.
    const std::vector<SoundEntry>& Sounds() const
    {
        return sounds_;
    }

    // The bank with number `number`, or nullptr.
    const Bank* FindBank(uint32_t number) const;

    // The wave system with number `number`, or nullptr.
    const WaveSystem* FindWaveSystem(uint32_t number) const;

    // The sequence collection's data, which sequences address from its start, and the offset of a sequence in it.
    std::span<const uint8_t> Collection() const;
    std::optional<uint32_t> CollectionSequence(uint32_t category, uint32_t index) const;

    // Problems found while reading that leave the rest readable, such as a bank that can't be read.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    void ReadSoundTable(std::span<const uint8_t> bst, std::span<const uint8_t> bstn);

    std::vector<uint8_t> data_;
    std::vector<SoundEntry> sounds_;
    std::vector<WaveSystem> wave_systems_;
    std::vector<Bank> banks_;
    std::size_t collection_offset_ = 0;
    std::size_t collection_size_ = 0;
    std::vector<std::vector<uint32_t>> collection_; // each category's sequence offsets
    std::vector<std::string> warnings_;
};

// A file of a sequence archive (RARC).
struct ArchiveFile
{
    uint16_t id = 0;
    std::string name;
    std::span<const uint8_t> data;
};

// Read the files of an RARC archive. The spans point into `rarc`. Throw FormatError if it isn't one.
std::vector<ArchiveFile> ReadRarc(std::span<const uint8_t> rarc);

} // namespace dolphinsf2
