// SPDX-License-Identifier: MIT

// The sound data of the Nintendo 64's sound library (see n64_sound.h).

#include "n64_sound.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

namespace dolphinsf2::n64
{
namespace
{

// The size of a table's header and of each entry.
constexpr uint32_t kTableHeader = 16;
constexpr uint32_t kEntrySize = 16;

// The most aliases that a chain of table entries follows.
constexpr int kMaxAliases = 8;

// A bank's header: the offsets of its drums' list and of its effects, then an offset for each instrument.
constexpr uint32_t kBankHeader = 8;

// An instrument's size, and where its envelope and its three tuned samples lie in it.
constexpr uint32_t kInstrumentSize = 0x20;
constexpr uint32_t kInstrumentEnvelope = 4;
constexpr uint32_t kInstrumentSamples = 8;

// A drum's size, and where its tuned sample and its envelope lie in it.
constexpr uint32_t kDrumSize = 0x10;
constexpr uint32_t kDrumSample = 4;
constexpr uint32_t kDrumEnvelope = 0xc;

// A tuned sample's size, and the sizes of a sample's loop without its state and of the state.
constexpr uint32_t kTunedSampleSize = 8;
constexpr uint32_t kLoopSize = 0x10;
constexpr uint32_t kLoopStateSize = 0x20;

// The bits of a sample entry's first word: its codec, the medium that names its sample bank, and its size in bytes.
constexpr int kCodecShift = 28;
constexpr uint32_t kCodecMask = 7;
constexpr int kMediumShift = 26;
constexpr uint32_t kMediumMask = 3;
constexpr uint32_t kSizeMask = 0xffffff;

// The codec of VADPCM, the only codec that the games' banks use.
constexpr uint32_t kCodecVadpcm = 0;

// A loop's count of a loop that repeats without end. A count of 0 means no loop.
constexpr uint32_t kLoopForever = 0xffffffff;

// VADPCM's frames: nine bytes for sixteen samples, in two halves of eight. A predictor has two rows of coefficients
// (order 2), and the coefficients are fractions of 2048.
constexpr std::size_t kFrameBytes = 9;
constexpr std::size_t kFrameSamples = 16;
constexpr std::size_t kHalfSamples = 8;
constexpr std::size_t kOrder = 2;
constexpr int kCoefficientShift = 11;

// The release table's values (MakeReleaseTable): indexes 0xfb to 0xff fall in under a frame, 0x80 to 0xfa in
// 0xfb - index frames, 0x10 to 0x7f in 60 + 4 × (0x80 - index), and 1 to 0xf in 480 + 60 × (0xf - index).
constexpr std::array<double, 5> kFastReleases = {0.75, 0.66, 0.5, 0.33, 0.25};

// Read `N` big-endian floats at `address` in `dol`.
template <std::size_t N>
std::array<float, N> ReadFloats(const ntdlib::Dol& dol, uint32_t address, const char* what)
{
    const std::span<const uint8_t> bytes = dol.Bytes(address, 4 * N, what);
    std::array<float, N> values{};
    for (std::size_t i = 0; i < N; i++)
    {
        values[i] = std::bit_cast<float>(xsflib::LoadBe32(bytes.data() + 4 * i));
    }

    return values;
}

// Read 16 bytes at `address` in `dol`.
std::array<uint8_t, 16> ReadBytes16(const ntdlib::Dol& dol, uint32_t address, const char* what)
{
    const std::span<const uint8_t> bytes = dol.Bytes(address, 16, what);
    std::array<uint8_t, 16> values{};
    std::copy(bytes.begin(), bytes.end(), values.begin());

    return values;
}

// The 32-bit word at `offset` in `data`. Throw FormatError if it runs past the end.
uint32_t Word(std::span<const uint8_t> data, uint64_t offset, const char* what)
{
    if (offset + 4 > data.size())
    {
        throw FormatError(std::string(what) + " runs past the end of its data");
    }

    return xsflib::LoadBe32(data.data() + offset);
}

} // namespace

double ReleaseFrames(uint8_t index)
{
    if (index == 0)
    {
        return 0.0;
    }
    if (index >= 0xfb)
    {
        return kFastReleases[index - 0xfb];
    }
    if (index >= 0x80)
    {
        return 0xfb - index;
    }
    if (index >= 0x10)
    {
        return 60.0 + 4.0 * (0x80 - index);
    }

    return 480.0 + 60.0 * (0xf - index);
}

std::vector<EnvelopePoint> ReadEnvelope(std::span<const uint8_t> data, uint32_t offset)
{
    std::vector<EnvelopePoint> points;
    for (std::size_t i = 0; i < kMaxEnvelopePoints; i++)
    {
        const uint64_t at = uint64_t{offset} + 4 * i;
        if (at + 4 > data.size())
        {
            throw FormatError("an envelope runs past the end of its data");
        }

        const EnvelopePoint point{static_cast<int16_t>(xsflib::LoadBe16(data.data() + at)),
                                  static_cast<int16_t>(xsflib::LoadBe16(data.data() + at + 2))};
        points.push_back(point);
        if (point.delay <= 0 && point.delay != kEnvelopeSetValue)
        {
            break;
        }
    }

    return points;
}

std::vector<int16_t> DecodeVadpcm(std::span<const uint8_t> data, std::span<const int16_t> book, std::size_t count,
                                  int16_t history2, int16_t history1)
{
    const std::size_t predictors = book.size() / (kOrder * kHalfSamples);
    std::vector<int16_t> out;
    out.reserve(count);
    int prev2 = history2;
    int prev1 = history1;
    for (std::size_t frame = 0; out.size() < count; frame++)
    {
        const std::size_t at = frame * kFrameBytes;
        if (at + kFrameBytes > data.size())
        {
            throw FormatError("a sample's data ends early");
        }

        // The header gives the scale of the frame's samples and its predictor.
        const int scale = data[at] >> 4;
        std::size_t predictor = data[at] & 0xf;
        if (predictor >= predictors)
        {
            predictor = 0;
        }
        const std::span<const int16_t> row2 = book.subspan(predictor * kOrder * kHalfSamples, kHalfSamples);
        const std::span<const int16_t> row1 =
            book.subspan(predictor * kOrder * kHalfSamples + kHalfSamples, kHalfSamples);

        // Each half predicts from the last two samples before it. Each sample of the half adds the residuals of the
        // half's earlier samples through the second row.
        for (std::size_t half = 0; half < 2; half++)
        {
            std::array<int, kHalfSamples> residuals{};
            for (std::size_t k = 0; k < kHalfSamples; k++)
            {
                const uint8_t byte = data[at + 1 + half * 4 + k / 2];
                int nibble = (k % 2 == 0) ? byte >> 4 : byte & 0xf;
                if (nibble >= 8)
                {
                    nibble -= 16;
                }
                residuals[k] = nibble * (1 << scale);
            }

            std::array<int, kHalfSamples> samples{};
            for (std::size_t j = 0; j < kHalfSamples; j++)
            {
                int64_t total = int64_t{row2[j]} * prev2 + int64_t{row1[j]} * prev1 +
                                int64_t{residuals[j]} * (1 << kCoefficientShift);
                for (std::size_t k = 0; k < j; k++)
                {
                    total += int64_t{row1[j - k - 1]} * residuals[k];
                }
                samples[j] = static_cast<int>(std::clamp<int64_t>(total >> kCoefficientShift, -32768, 32767));
            }

            prev2 = samples[6];
            prev1 = samples[7];
            for (int s : samples)
            {
                if (out.size() < count)
                {
                    out.push_back(static_cast<int16_t>(s));
                }
            }
        }
    }

    return out;
}

SoundData::SoundData(const ntdlib::Dol& dol, const LibraryTables& tables, std::vector<uint8_t> rom)
    : rom_(std::move(rom)),
      sequences_(ReadTable(dol, tables.sequences)),
      banks_(ReadTable(dol, tables.banks)),
      sample_banks_(ReadTable(dol, tables.sample_banks))
{
    // The segment table's three parts have to lie in the sound data.
    const std::vector<Entry> segments = ReadTable(dol, tables.segments);
    if (segments.size() < segments_.size())
    {
        throw FormatError("main.dol's table of the sound data's parts is too short");
    }
    for (std::size_t i = 0; i < segments_.size(); i++)
    {
        segments_[i] = segments[i];
        if (uint64_t{segments[i].offset} + segments[i].size > rom_.size())
        {
            throw FormatError("audiorom.img is shorter than main.dol's tables say");
        }
    }

    // The map gives a 16-bit offset for each sequence, then at each offset a count of banks and their numbers. It ends
    // after the last list.
    const std::span<const uint8_t> map = dol.At(tables.bank_map);
    std::size_t map_end = 2 * sequences_.size();
    if (map.size() < map_end)
    {
        throw FormatError("main.dol's map of the sequences' banks ends early");
    }
    for (std::size_t i = 0; i < sequences_.size(); i++)
    {
        const std::size_t list = xsflib::LoadBe16(map.data() + 2 * i);
        if (list >= map.size() || list + 1 + map[list] > map.size())
        {
            throw FormatError("main.dol's map of the sequences' banks ends early");
        }
        map_end = std::max(map_end, list + 1 + map[list]);
    }
    bank_map_.assign(map.begin(), map.begin() + static_cast<std::ptrdiff_t>(map_end));

    cents_ = ReadFloats<256>(dol, tables.cents, "the table of bends");
    small_cents_ = ReadFloats<256>(dol, tables.small_cents, "the table of small bends");
    pitches_ = ReadFloats<128>(dol, tables.pitches, "the table of pitches");
    short_velocities_ = ReadBytes16(dol, tables.short_velocities, "the table of short notes' velocities");
    short_gate_times_ = ReadBytes16(dol, tables.short_gate_times, "the table of short notes' gate times");
    const std::span<const uint8_t> envelope_data = dol.At(tables.default_envelope);
    default_envelope_ = ReadEnvelope(envelope_data, 0);
}

std::vector<SoundData::Entry> SoundData::ReadTable(const ntdlib::Dol& dol, uint32_t address)
{
    const std::span<const uint8_t> header = dol.Bytes(address, kTableHeader, "the table of sounds");
    const uint32_t count = xsflib::LoadBe16(header.data());
    const std::span<const uint8_t> table = dol.Bytes(address, kTableHeader + count * kEntrySize, "the table of sounds");
    std::vector<Entry> entries(count);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* e = table.data() + kTableHeader + i * kEntrySize;
        entries[i] = {xsflib::LoadBe32(e), xsflib::LoadBe32(e + 4), static_cast<int16_t>(xsflib::LoadBe16(e + 10)),
                      static_cast<int16_t>(xsflib::LoadBe16(e + 12)), static_cast<int16_t>(xsflib::LoadBe16(e + 14))};
    }

    return entries;
}

uint32_t SoundData::Resolve(uint32_t number) const
{
    for (int chain = 0; chain < kMaxAliases; chain++)
    {
        if (number >= sequences_.size())
        {
            break;
        }
        if (sequences_[number].size != 0)
        {
            return number;
        }
        number = sequences_[number].offset;
    }

    throw FormatError("main.dol's table of sequences lacks sequence " + std::to_string(number));
}

std::span<const uint8_t> SoundData::Sequence(uint32_t number) const
{
    const Entry& e = sequences_[Resolve(number)];
    if (uint64_t{e.offset} + e.size > segments_[0].size)
    {
        throw FormatError("sequence " + std::to_string(number) + " lies outside the sound data");
    }

    return std::span<const uint8_t>(rom_).subspan(segments_[0].offset + e.offset, e.size);
}

std::vector<uint8_t> SoundData::BanksOf(uint32_t number) const
{
    if (number >= sequences_.size())
    {
        return {};
    }

    const std::size_t list = xsflib::LoadBe16(bank_map_.data() + 2 * number);
    return {bank_map_.begin() + static_cast<std::ptrdiff_t>(list + 1),
            bank_map_.begin() + static_cast<std::ptrdiff_t>(list + 1 + bank_map_[list])};
}

std::optional<uint8_t> SoundData::ResolveBank(uint32_t number) const
{
    for (int chain = 0; chain < kMaxAliases; chain++)
    {
        if (number >= banks_.size())
        {
            return std::nullopt;
        }
        if (banks_[number].size != 0)
        {
            return static_cast<uint8_t>(number);
        }
        number = banks_[number].offset;
    }

    return std::nullopt;
}

const Bank* SoundData::GetBank(uint8_t number)
{
    const std::optional<uint8_t> resolved = ResolveBank(number);
    if (!resolved)
    {
        errors_.push_back("bank " + std::to_string(number) + " isn't in main.dol's table of banks");
        return nullptr;
    }

    auto found = bank_cache_.find(*resolved);
    if (found == bank_cache_.end())
    {
        std::unique_ptr<Bank> bank;
        try
        {
            bank = ReadBank(*resolved);
        }
        catch (const FormatError& e)
        {
            errors_.push_back("bank " + std::to_string(*resolved) + ": " + e.what());
        }
        found = bank_cache_.emplace(*resolved, std::move(bank)).first;
    }

    return found->second.get();
}

std::unique_ptr<Bank> SoundData::ReadBank(uint8_t number) const
{
    const Entry& e = banks_[number];
    if (uint64_t{e.offset} + e.size > segments_[1].size)
    {
        throw FormatError("it lies outside the sound data");
    }

    const std::span<const uint8_t> data =
        std::span<const uint8_t>(rom_).subspan(segments_[1].offset + e.offset, e.size);

    // The entry's first value names the bank's two sample banks, 0xff for none. Its second gives the counts of
    // instruments and drums, and its third the count of effects.
    const std::array<int, 2> sample_banks = {(e.value1 >> 8) & 0xff, e.value1 & 0xff};
    const uint32_t instruments = (e.value2 >> 8) & 0xff;
    const uint32_t drums = e.value2 & 0xff;
    const uint32_t effects = static_cast<uint16_t>(e.value3);
    auto bank = std::make_unique<Bank>();

    const uint32_t drum_list = Word(data, 0, "the bank's header");
    if (drum_list != 0)
    {
        for (uint32_t i = 0; i < drums; i++)
        {
            const uint32_t at = Word(data, uint64_t{drum_list} + 4 * i, "the bank's list of drums");
            if (at == 0)
            {
                bank->drums.emplace_back();
                continue;
            }
            if (uint64_t{at} + kDrumSize > data.size())
            {
                throw FormatError("a drum runs past the end of the bank");
            }

            Drum drum;
            drum.release = data[at];
            drum.pan = data[at + 1];
            drum.sample = ReadTunedSample(*bank, data, at + kDrumSample, sample_banks);
            drum.envelope = ReadEnvelope(data, Word(data, at + kDrumEnvelope, "a drum"));
            bank->drums.push_back(std::move(drum));
        }
    }

    const uint32_t effect_list = Word(data, 4, "the bank's header");
    if (effect_list != 0)
    {
        for (uint32_t i = 0; i < effects; i++)
        {
            bank->effects.push_back(ReadTunedSample(*bank, data, effect_list + kTunedSampleSize * i, sample_banks));
        }
    }

    for (uint32_t i = 0; i < instruments; i++)
    {
        const uint32_t at = Word(data, kBankHeader + 4 * i, "the bank's list of instruments");
        if (at == 0)
        {
            bank->instruments.emplace_back();
            continue;
        }
        if (uint64_t{at} + kInstrumentSize > data.size())
        {
            throw FormatError("an instrument runs past the end of the bank");
        }

        // An instrument without a low sample has a low end of 0, and one without a high sample a high end of 127.
        Instrument inst;
        inst.low = data[at + 1];
        inst.high = data[at + 2];
        inst.release = data[at + 3];
        inst.envelope = ReadEnvelope(data, Word(data, at + kInstrumentEnvelope, "an instrument"));
        if (inst.low != 0)
        {
            inst.low_sample = ReadTunedSample(*bank, data, at + kInstrumentSamples, sample_banks);
        }
        inst.normal_sample = ReadTunedSample(*bank, data, at + kInstrumentSamples + kTunedSampleSize, sample_banks);
        if (inst.high != 127)
        {
            inst.high_sample =
                ReadTunedSample(*bank, data, at + kInstrumentSamples + 2 * kTunedSampleSize, sample_banks);
        }
        bank->instruments.push_back(std::move(inst));
    }

    return bank;
}

TunedSample SoundData::ReadTunedSample(Bank& bank, std::span<const uint8_t> bank_data, uint32_t offset,
                                       const std::array<int, 2>& sample_banks) const
{
    const uint32_t sample = Word(bank_data, offset, "a tuned sample");
    const float tuning = std::bit_cast<float>(Word(bank_data, uint64_t{offset} + 4, "a tuned sample"));
    if (sample == 0)
    {
        return {nullptr, tuning};
    }

    return {ReadSample(bank, bank_data, sample, sample_banks), tuning};
}

const Sample* SoundData::ReadSample(Bank& bank, std::span<const uint8_t> bank_data, uint32_t offset,
                                    const std::array<int, 2>& sample_banks) const
{
    if (const auto found = bank.samples.find(offset); found != bank.samples.end())
    {
        return found->second.get();
    }

    // The entry: its codec, medium and size, the offset of its data in its sample bank, and the offsets of its loop
    // and its book in the bank.
    const uint32_t bits = Word(bank_data, offset, "a sample");
    const uint32_t address = Word(bank_data, uint64_t{offset} + 4, "a sample");
    const uint32_t loop = Word(bank_data, uint64_t{offset} + 8, "a sample");
    const uint32_t book = Word(bank_data, uint64_t{offset} + 12, "a sample");
    const uint32_t codec = (bits >> kCodecShift) & kCodecMask;
    const uint32_t medium = (bits >> kMediumShift) & kMediumMask;
    const uint32_t size = bits & kSizeMask;
    if (codec != kCodecVadpcm)
    {
        throw FormatError("a sample has codec " + std::to_string(codec) + ", which dolphinsf2 doesn't read");
    }
    if (medium > 1 || sample_banks[medium] == 0xff)
    {
        throw FormatError("a sample's medium names no sample bank");
    }

    // The sample bank's data, which an entry of size 0 names by number.
    uint32_t sample_bank = static_cast<uint32_t>(sample_banks[medium]);
    for (int chain = 0;
         chain < kMaxAliases && sample_bank < sample_banks_.size() && sample_banks_[sample_bank].size == 0; chain++)
    {
        sample_bank = sample_banks_[sample_bank].offset;
    }
    if (sample_bank >= sample_banks_.size())
    {
        throw FormatError("a sample's sample bank isn't in main.dol's table");
    }
    const Entry& bank_entry = sample_banks_[sample_bank];
    if (uint64_t{bank_entry.offset} + bank_entry.size > segments_[2].size || uint64_t{address} + size > bank_entry.size)
    {
        throw FormatError("a sample lies outside its sample bank");
    }
    const std::span<const uint8_t> sample_data =
        std::span<const uint8_t>(rom_).subspan(segments_[2].offset + bank_entry.offset + address, size);

    // The book: the order, the count of predictors, then the coefficients.
    const uint32_t order = Word(bank_data, book, "a sample's book");
    const uint32_t predictors = Word(bank_data, uint64_t{book} + 4, "a sample's book");
    if (order != kOrder || predictors == 0 || predictors > 16)
    {
        throw FormatError("a sample's book has order " + std::to_string(order) + " and " + std::to_string(predictors) +
                          " predictors");
    }
    const uint64_t coefficients = uint64_t{order} * predictors * kHalfSamples;
    if (book + 8 + 2 * coefficients > bank_data.size())
    {
        throw FormatError("a sample's book runs past the end of the bank");
    }
    std::vector<int16_t> codebook(coefficients);
    for (std::size_t i = 0; i < codebook.size(); i++)
    {
        codebook[i] = static_cast<int16_t>(xsflib::LoadBe16(bank_data.data() + book + 8 + 2 * i));
    }

    // The loop: its start, its end, its count, and with a count the decoder's state at the start. A sample that doesn't
    // loop ends at the loop's end.
    if (uint64_t{loop} + kLoopSize > bank_data.size())
    {
        throw FormatError("a sample's loop runs past the end of the bank");
    }
    const uint32_t loop_start = Word(bank_data, loop, "a sample's loop");
    const uint32_t loop_end = Word(bank_data, uint64_t{loop} + 4, "a sample's loop");
    const uint32_t loop_count = Word(bank_data, uint64_t{loop} + 8, "a sample's loop");
    const std::size_t frames = size / kFrameBytes;
    const std::size_t length = std::min<std::size_t>(loop_end, frames * kFrameSamples);
    std::vector<int16_t> pcm = DecodeVadpcm(sample_data, codebook, length);

    auto added = std::make_unique<Sample>();
    added->id = offset;
    added->pcm.sample_rate = static_cast<float>(kSampleRate);

    if (loop_count != 0 && loop_start < length)
    {
        if (uint64_t{loop} + kLoopSize + kLoopStateSize > bank_data.size())
        {
            throw FormatError("a sample's loop runs past the end of the bank");
        }

        // The state holds the samples of the frame that holds the loop's start. A repeat takes them from the state, and
        // decodes from the next frame with the state's last two samples as the decoder's history. If that gives other
        // samples than the first pass, a loop without end plays once unrolled. Each later repeat then gives the same
        // samples as the first. A loop with a count keeps the samples of the first pass.
        const std::size_t next_frame = loop_start / kFrameSamples + 1;
        std::vector<int16_t> state(kFrameSamples);
        for (std::size_t i = 0; i < kFrameSamples; i++)
        {
            state[i] = static_cast<int16_t>(xsflib::LoadBe16(bank_data.data() + loop + kLoopSize + 2 * i));
        }

        std::vector<int16_t> repeat(state.begin() + static_cast<std::ptrdiff_t>(loop_start % kFrameSamples),
                                    state.end());
        if (length > next_frame * kFrameSamples)
        {
            const std::vector<int16_t> rest =
                DecodeVadpcm(sample_data.subspan(next_frame * kFrameBytes), codebook,
                             length - next_frame * kFrameSamples, state[kFrameSamples - 2], state[kFrameSamples - 1]);
            repeat.insert(repeat.end(), rest.begin(), rest.end());
        }
        repeat.resize(length - loop_start);

        const bool same =
            std::equal(repeat.begin(), repeat.end(), pcm.begin() + static_cast<std::ptrdiff_t>(loop_start));
        added->pcm.loop = true;
        if (same || loop_count != kLoopForever)
        {
            added->pcm.loop_start = loop_start;
            added->pcm.loop_end = static_cast<uint32_t>(pcm.size());
        }
        else
        {
            added->pcm.loop_start = static_cast<uint32_t>(pcm.size());
            pcm.insert(pcm.end(), repeat.begin(), repeat.end());
            added->pcm.loop_end = static_cast<uint32_t>(pcm.size());
        }
    }

    added->pcm.channels.push_back(std::move(pcm));

    const Sample* result = added.get();
    bank.samples.emplace(offset, std::move(added));

    return result;
}

} // namespace dolphinsf2::n64
