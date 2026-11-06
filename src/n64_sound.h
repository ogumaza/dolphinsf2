// SPDX-License-Identifier: MIT

// The sound data of Nintendo's sound library for the Nintendo 64, as Animal Crossing's port keeps it on the GameCube.
// Tables in main.dol place the sequences, the banks and the sample banks in the file of sound data (audiorom.img). A
// bank holds instruments, drums and effects. Each plays a sample of one of the bank's sample banks, with an envelope.
// The samples are VADPCM: frames of nine bytes that hold sixteen 4-bit samples.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dol.h"

#include "jaudio.h"

namespace dolphinsf2::n64
{

// The places in main.dol of the tables that the library reads, in a game's build.
struct LibraryTables
{
    // The tables of sequences, banks and sample banks: a 16-bit count, then from 16 bytes in an entry of 16 bytes for
    // each: the offset and the size in its part of the sound data, two bytes on how the library loads it, and three
    // 16-bit values. The segment table follows them: three entries, which give the parts of the sound data that hold
    // the sequences, the banks and the samples. The map of each sequence's banks follows the segment table.
    uint32_t sequences = 0;
    uint32_t banks = 0;
    uint32_t sample_banks = 0;
    uint32_t segments = 0;
    uint32_t bank_map = 0;

    // The tables of pitches: PCENTTABLE (256 floats, a bend of 0 at 128), PCENTTABLE2 (the same for small bends) and
    // PITCHTABLE (128 floats, 1 at semitone 39).
    uint32_t cents = 0;
    uint32_t small_cents = 0;
    uint32_t pitches = 0;

    // The tables that a new sequence takes: the 16 velocities and the 16 gate times of short notes (DEFAULT_VTABLE and
    // DEFAULT_GTABLE), and the envelope of a channel that sets none (DEFAULT_ENV).
    uint32_t short_velocities = 0;
    uint32_t short_gate_times = 0;
    uint32_t default_envelope = 0;
};

// A point of an envelope. A delay above 0 fades the envelope to `value` over that many of the library's updates. The
// level is (value / 32767)². The other delays are commands (kEnvelopeEnd and the others).
struct EnvelopePoint
{
    bool operator==(const EnvelopePoint&) const = default;

    int16_t delay = 0;
    int16_t value = 0;
};

// The commands of an envelope: it ends and the voice stops, it holds its level, it goes on at point `value`, or it
// starts again. A point of kEnvelopeSetValue stores its value and the envelope goes on at the next point. The games'
// instruments start their envelopes with one. The conversion leaves the stored value out.
constexpr int16_t kEnvelopeEnd = 0;
constexpr int16_t kEnvelopeHold = -1;
constexpr int16_t kEnvelopeGoTo = -2;
constexpr int16_t kEnvelopeRestart = -3;
constexpr int16_t kEnvelopeSetValue = -4;

// The most points that an envelope reads. Each envelope that the games' banks and sequences hold ends earlier.
constexpr std::size_t kMaxEnvelopePoints = 32;

// A decoded sample of a bank. A looping sample's loop ends at the end of its PCM. A sample that doesn't loop plays to
// its end. The library's base rate of every sample is kSampleRate: a tuning of 1 plays it at that rate at semitone 39.
struct Sample
{
    uint32_t id = 0; // its offset in its bank
    Pcm pcm;
};

// A sample and the tuning that an instrument, a drum or an effect plays it at.
struct TunedSample
{
    const Sample* sample = nullptr;
    float tuning = 1.0f;
};

// An instrument of a bank. Semitones below `low` play the low sample, those above `high` the high one, and the others
// the normal one. An instrument without a low or a high sample leaves `low` at 0 or `high` at 127.
struct Instrument
{
    uint8_t low = 0;
    uint8_t high = 127;
    uint8_t release = 0; // the decay index of a released voice (ReleaseFrames)
    std::vector<EnvelopePoint> envelope;
    TunedSample low_sample, normal_sample, high_sample;
};

// A drum of a bank: its sample, its envelope, its release and its pan (0 left to 127 right).
struct Drum
{
    uint8_t release = 0;
    uint8_t pan = 64;
    std::vector<EnvelopePoint> envelope;
    TunedSample sample;
};

// A bank. An instrument or a drum that the bank lacks is empty. An effect without a sample has none.
struct Bank
{
    std::vector<std::optional<Instrument>> instruments;
    std::vector<std::optional<Drum>> drums;
    std::vector<TunedSample> effects;

    // The bank's samples, by their offsets in the bank. Each is decoded once.
    std::map<uint32_t, std::unique_ptr<Sample>> samples;
};

// The library's highest tempo in tatums a minute: the tempo that runs a tick every update. The library works it out
// from the updates of a frame (4), the frames a second of an NTSC display (60, with its deviation of 1.00278) and the
// port's factor of 1.04613 (__Nas_MemoryReconfig).
constexpr int kMaxTempo = static_cast<int>(60.0 * 1000.0 * 4 / (1000.0 * 1.00278 / 60.0) / 1.04613);

// The library's updates a second. A sequence plays at its tempo: kMaxTempo tatums a minute run a tick every update. The
// scripts' ticks, the fades, the envelopes and the vibratos all step once an update.
constexpr double kUpdatesPerSecond = kMaxTempo / 60.0;

// The updates of one of the library's frames, and the samples that its synthesis makes in an update.
constexpr int kUpdatesPerFrame = 4;
constexpr int kSamplesPerUpdate = 200;

// The library's frames a second.
constexpr double kFramesPerSecond = kUpdatesPerSecond / kUpdatesPerFrame;

// The rate in hertz at which the library plays a sample at a tuning of 1 at semitone 39. The synthesis steps through a
// sample by its pitch times 33476.156 / 48000 (the audio parameters' resample_rate) for each sample that it makes. The
// port's updates run slower than the figure of 48000 samples a second gives. The samples of an update come out over the
// time of an update, at kSamplesPerUpdate times kUpdatesPerSecond a second.
constexpr double kSampleRate = 33476.156 / 48000.0 * kSamplesPerUpdate * kUpdatesPerSecond;

// The frames (kFramesPerSecond) that a released voice takes to fall silent from full level with decay index `index`.
// The library's release table (MakeReleaseTable) gives each index its rate. Index 0 has no release: the voice holds.
double ReleaseFrames(uint8_t index);

// Read the envelope at `offset` in `data`: its points up to its first command other than kEnvelopeSetValue, at most
// kMaxEnvelopePoints. Throw FormatError if it runs past the data's end.
std::vector<EnvelopePoint> ReadEnvelope(std::span<const uint8_t> data, uint32_t offset);

// A game's sound data: the library's tables in main.dol, and the file of sound data.
class SoundData
{
public:
    // Read the tables of `dol` at `tables`, and the sound data `rom`. Throw FormatError if a table lies outside
    // main.dol or a part of the sound data outside `rom`.
    SoundData(const ntdlib::Dol& dol, const LibraryTables& tables, std::vector<uint8_t> rom);

    // The sequences in the table.
    std::size_t SequenceCount() const
    {
        return sequences_.size();
    }

    // The number of the sequence whose data sequence `number` plays: a sequence of size 0 plays the one whose number
    // its offset holds. Throw FormatError for a number outside the table or a chain that doesn't end.
    uint32_t Resolve(uint32_t number) const;

    // The data of sequence `number` (Resolve). Throw FormatError if it lies outside the sound data.
    std::span<const uint8_t> Sequence(uint32_t number) const;

    // The banks of sequence `number`, from the map. The library gives a sequence the last of them.
    std::vector<uint8_t> BanksOf(uint32_t number) const;

    // The number of the bank that bank `number` stands for (as Resolve does for sequences), or nothing for a number
    // outside the table.
    std::optional<uint8_t> ResolveBank(uint32_t number) const;

    // Bank `number`, read on first use, or nullptr if it can't be read. Errors() then says why.
    const Bank* GetBank(uint8_t number);

    // The library's tables of pitches and pans (LibraryTables).
    const std::array<float, 256>& Cents() const
    {
        return cents_;
    }

    const std::array<float, 256>& SmallCents() const
    {
        return small_cents_;
    }

    const std::array<float, 128>& Pitches() const
    {
        return pitches_;
    }

    const std::array<uint8_t, 16>& ShortVelocities() const
    {
        return short_velocities_;
    }

    const std::array<uint8_t, 16>& ShortGateTimes() const
    {
        return short_gate_times_;
    }

    const std::vector<EnvelopePoint>& DefaultEnvelope() const
    {
        return default_envelope_;
    }

    // The errors of the banks and samples read so far.
    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    // An entry of one of the tables (LibraryTables).
    struct Entry
    {
        uint32_t offset = 0;
        uint32_t size = 0;
        int16_t value1 = 0;
        int16_t value2 = 0;
        int16_t value3 = 0;
    };

    // Read the table at `address` in `dol`. Throw FormatError if it lies outside main.dol.
    static std::vector<Entry> ReadTable(const ntdlib::Dol& dol, uint32_t address);

    // Read bank `number` (resolved). Throw FormatError if it can't be read.
    std::unique_ptr<Bank> ReadBank(uint8_t number) const;

    // Read and decode the sample whose entry lies at `offset` in `bank_data`, the data of `bank`. `sample_banks` gives
    // the sample bank of each medium. Return the sample that `bank` holds already at that offset. Throw FormatError if
    // it can't be read.
    const Sample* ReadSample(Bank& bank, std::span<const uint8_t> bank_data, uint32_t offset,
                             const std::array<int, 2>& sample_banks) const;

    // Read the tuned sample at `offset` in `bank`'s data: the offset of its sample and its tuning. A tuned sample
    // without a sample has none.
    TunedSample ReadTunedSample(Bank& bank, std::span<const uint8_t> bank_data, uint32_t offset,
                                const std::array<int, 2>& sample_banks) const;

    std::vector<uint8_t> rom_;
    std::vector<Entry> sequences_, banks_, sample_banks_;
    std::array<Entry, 3> segments_{};
    std::vector<uint8_t> bank_map_;
    std::array<float, 256> cents_{}, small_cents_{};
    std::array<float, 128> pitches_{};
    std::array<uint8_t, 16> short_velocities_{}, short_gate_times_{};
    std::vector<EnvelopePoint> default_envelope_;
    std::map<uint8_t, std::unique_ptr<Bank>> bank_cache_;
    std::vector<std::string> errors_;
};

// Decode `count` samples of VADPCM from `data`: frames of nine bytes, each a byte with the scale in its high nibble and
// the predictor in its low one, then sixteen 4-bit samples. `book` holds each predictor's two rows of eight
// coefficients. The decoder's history starts at `history2` and `history1`, the two samples before the first. Throw
// FormatError if the data ends early.
std::vector<int16_t> DecodeVadpcm(std::span<const uint8_t> data, std::span<const int16_t> book, std::size_t count,
                                  int16_t history2 = 0, int16_t history1 = 0);

} // namespace dolphinsf2::n64
