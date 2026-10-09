// SPDX-License-Identifier: MIT

// Unit tests for dolphinsf2, without game data. The tests build small JAudio2 archives in memory.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "binary.h"
#include "convert.h"
#include "envelope.h"
#include "game.h"
#include "instruments.h"
#include "jaudio.h"
#include "performance.h"
#include "wave.h"

namespace
{

using namespace dolphinsf2;

int failures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what.c_str());
        failures++;
    }
}

// Big-endian data under construction.
class Bytes
{
public:
    std::size_t Size() const
    {
        return data_.size();
    }

    Bytes& U8(uint32_t v)
    {
        data_.push_back(static_cast<uint8_t>(v));
        return *this;
    }

    Bytes& U16(uint32_t v)
    {
        return U8(v >> 8).U8(v);
    }

    Bytes& U24(uint32_t v)
    {
        return U8(v >> 16).U16(v);
    }

    Bytes& U32(uint32_t v)
    {
        return U16(v >> 16).U16(v);
    }

    Bytes& F32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        return U32(bits);
    }

    Bytes& Tag(const char* tag)
    {
        for (int i = 0; i < 4; i++)
        {
            U8(static_cast<uint8_t>(tag[i]));
        }
        return *this;
    }

    Bytes& Append(const std::vector<uint8_t>& more)
    {
        data_.insert(data_.end(), more.begin(), more.end());
        return *this;
    }

    Bytes& PadTo(std::size_t size)
    {
        data_.resize(std::max(data_.size(), size));
        return *this;
    }

    void Put32(std::size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            data_[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (24 - 8 * i));
        }
    }

    const std::vector<uint8_t>& Data() const
    {
        return data_;
    }

private:
    std::vector<uint8_t> data_;
};

// A wave system with one wave archive, "test.aw", of `waves` PCM16 waves of 64 samples each at 32000 Hz, key 60.
// The first loops over its last 32 samples.
std::vector<uint8_t> MakeWaveSystem(uint32_t id, int waves)
{
    Bytes w;
    w.Tag("WSYS").U32(0).U32(id).U32(0).U32(0x20).U32(0); // size, id, table size, WINF, WBCT (set below)
    w.PadTo(0x20);
    const std::size_t winf = w.Size();
    w.Tag("WINF").U32(1).U32(0); // one archive
    const std::size_t archive = w.Size();
    w.Put32(winf + 8, static_cast<uint32_t>(archive));
    std::vector<uint8_t> name(0x70);
    std::memcpy(name.data(), "test.aw", 7);
    w.Append(name).U32(static_cast<uint32_t>(waves));
    const std::size_t offsets = w.Size();
    for (int i = 0; i < waves; i++)
    {
        w.U32(0);
    }
    for (int i = 0; i < waves; i++)
    {
        w.Put32(offsets + 4 * static_cast<std::size_t>(i), static_cast<uint32_t>(w.Size()));
        const bool loop = i == 0;
        w.U8(0).U8(3).U8(60).U8(0).F32(32000.0f);
        w.U32(static_cast<uint32_t>(128 * i)).U32(128); // offset and size in the AW file
        w.U32(loop ? 0xffffffff : 0).U32(loop ? 32 : 0).U32(loop ? 64 : 0).U32(64);
        w.U16(0).U16(0);
    }

    // WBCT: one scene whose C-DF gives wave i the id i.
    const std::size_t wbct = w.Size();
    w.Put32(0x14, static_cast<uint32_t>(wbct));
    w.Tag("WBCT").U32(0).U32(1).U32(static_cast<uint32_t>(wbct + 16));
    const std::size_t scene = w.Size();
    w.Tag("SCNE").U32(0).U32(0).U32(static_cast<uint32_t>(scene + 16));
    const std::size_t cdf = w.Size();
    w.Tag("C-DF").U32(static_cast<uint32_t>(waves));
    const std::size_t entries = w.Size();
    for (int i = 0; i < waves; i++)
    {
        w.U32(0);
    }
    for (int i = 0; i < waves; i++)
    {
        w.Put32(entries + 4 * static_cast<std::size_t>(i), static_cast<uint32_t>(w.Size()));
        w.U32(static_cast<uint32_t>(id << 16 | static_cast<uint32_t>(i)));
    }
    (void)cdf;
    w.Put32(4, static_cast<uint32_t>(w.Size()));

    return w.Data();
}

// A version 0 bank whose instrument 0 plays wave 0 on every key, with an instant attack, a hold and a release of
// `release` units. Drum set 0 (program 0xe4) plays wave 1 on key 36, panned right.
std::vector<uint8_t> BankV0(uint32_t number, int16_t release)
{
    Bytes b;
    b.Tag("IBNK").U32(0).U32(number).U32(0).PadTo(0x20);
    b.Tag("BANK").PadTo(0x3b4 + 12 * 4);

    // Oscillator tables.
    const std::size_t attack = b.Size();
    b.U16(0).U16(0).U16(0x7fff).U16(14).U16(0).U16(0);
    const std::size_t release_table = b.Size();
    b.U16(0).U16(static_cast<uint16_t>(release)).U16(0).U16(15).U16(0).U16(0);
    const std::size_t osc = b.Size();
    b.U8(0).U8(0).U16(0).F32(1.0f).U32(static_cast<uint32_t>(attack)).U32(static_cast<uint32_t>(release_table));
    b.F32(1.0f).F32(0.0f);

    // A velocity region, a key region and the instrument.
    const std::size_t vmap = b.Size();
    b.U8(127).U8(0).U16(0).U32(0).F32(1.0f).F32(1.0f);
    const std::size_t keymap = b.Size();
    b.U8(127).U8(0).U16(0).U32(1).U32(static_cast<uint32_t>(vmap));
    const std::size_t inst = b.Size();
    b.Tag("INST").U32(0).F32(1.0f).F32(1.0f).U32(static_cast<uint32_t>(osc)).U32(0);
    b.U32(0).U32(0).U32(0).U32(0).U32(1).U32(static_cast<uint32_t>(keymap));
    b.Put32(0x24, static_cast<uint32_t>(inst));

    // The drum set.
    const std::size_t drum_vmap = b.Size();
    b.U8(127).U8(0).U16(0).U32(1).F32(0.5f).F32(1.0f);
    const std::size_t pmap = b.Size();
    b.F32(1.0f).F32(2.0f).U32(0).U32(0).U32(1).U32(static_cast<uint32_t>(drum_vmap));
    const std::size_t perc = b.Size();
    b.Tag("PER2").PadTo(perc + 0x88 + 4 * 128 + 128 + 256);
    b.Put32(perc + 0x88 + 4 * 36, static_cast<uint32_t>(pmap));
    std::vector<uint8_t> d = b.Data();
    d[perc + 0x288 + 36] = 127;   // pan
    d[perc + 0x308 + 2 * 36] = 0; // release: 200 units
    d[perc + 0x308 + 2 * 36 + 1] = 200;
    Bytes out;
    out.Append(d);
    out.Put32(0x3b4, static_cast<uint32_t>(perc));
    out.Put32(4, static_cast<uint32_t>(out.Size()));

    return out.Data();
}

// A version 1 bank whose program 0 plays wave 0 with a random pitch effect, whose key region ends at key 64, and whose
// second velocity region plays wave 1 above velocity 63. Its envelope rises over `attack` units, then falls to silence
// over `decay` units and holds there. The release takes 30 units.
std::vector<uint8_t> BankV1(uint32_t number, int16_t attack, int16_t decay)
{
    Bytes b;
    b.Tag("IBNK").U32(0).U32(number).U32(1).PadTo(0x20);
    b.Tag("ENVT").U32(30);
    b.U16(0).U16(static_cast<uint16_t>(attack)).U16(0x7fff);
    b.U16(0).U16(static_cast<uint16_t>(decay)).U16(0).U16(14).U16(0).U16(0);
    b.U16(0).U16(30).U16(0).U16(15).U16(0).U16(0); // the release, at 18
    b.PadTo((b.Size() + 3) & ~std::size_t{3});
    b.Tag("OSCT").U32(4 + 0x1c).U32(1);
    b.Tag("Osci").U8(0).U8(0).U16(0).F32(1.0f).U32(0).U32(18).F32(1.0f).F32(0.0f);
    const std::size_t rand = b.Size() + 8;
    b.Tag("RAND").U32(16).Tag("Rand").U8(1).U8(0).U16(0).F32(1.0f).F32(0.02f);
    const std::size_t inst = b.Size() + 8;
    b.Tag("INST").U32(0); // size set below
    const std::size_t inst_start = b.Size();
    b.Tag("Inst").U32(1).U32(0).U32(1).U32(static_cast<uint32_t>(rand)).U32(1);
    b.U8(64).U8(0).U16(0).U32(2);
    b.U8(63).U8(0).U16(0).U32(0).F32(1.0f).F32(1.0f);
    b.U8(127).U8(0).U16(0).U32(1).F32(1.0f).F32(1.0f);
    b.F32(1.0f).F32(1.0f);
    std::vector<uint8_t> d = b.Data();
    const auto inst_size = static_cast<uint32_t>(d.size() - inst_start);
    Bytes out;
    out.Append(d);
    out.Put32(inst - 4, inst_size);
    out.PadTo((out.Size() + 3) & ~std::size_t{3});
    out.Tag("LIST").U32(8).U32(1).U32(static_cast<uint32_t>(inst));
    out.Put32(4, static_cast<uint32_t>(out.Size()));

    return out.Data();
}

// A start-up archive with one wave system (number 0) of `waves` waves, `bank` played with it, and a sequence
// collection of one category that holds `sequences`.
std::vector<uint8_t> Archive(const std::vector<uint8_t>& bank, const std::vector<std::vector<uint8_t>>& sequences)
{
    const std::vector<uint8_t> ws = MakeWaveSystem(0, 2);
    Bytes sc;
    sc.U8('S').U8('C').U16(1).U32(0).U32(12);
    sc.U32(static_cast<uint32_t>(sequences.size()));
    std::size_t at = 12 + 4 + 4 * sequences.size();
    for (const std::vector<uint8_t>& s : sequences)
    {
        sc.U32(static_cast<uint32_t>(at));
        at += s.size();
    }
    for (const std::vector<uint8_t>& s : sequences)
    {
        sc.Append(s);
    }
    sc.Put32(4, static_cast<uint32_t>(sc.Size()));

    // The chunk list, then the chunks.
    Bytes a;
    a.Tag("AA_<");
    constexpr std::size_t kListSize = 4 + 16 + 12 + 12 + 4;
    const auto ws_at = static_cast<uint32_t>(kListSize);
    const auto bank_at = static_cast<uint32_t>(ws_at + ws.size());
    const auto sc_at = static_cast<uint32_t>(bank_at + bank.size());
    a.Tag("ws  ").U32(0).U32(ws_at).U32(0xffffffff);
    a.Tag("bnk ").U32(0).U32(bank_at);
    a.Tag("bsc ").U32(sc_at).U32(static_cast<uint32_t>(sc_at + sc.Size()));
    a.Tag(">_AA");
    a.Append(ws).Append(bank).Append(sc.Data());

    return a.Data();
}

// The AW file: wave i is 64 samples of a square wave of period 16 at i * 128 bytes.
std::vector<uint8_t> AwFile()
{
    Bytes aw;
    for (int wave = 0; wave < 2; wave++)
    {
        for (int i = 0; i < 64; i++)
        {
            aw.U16(static_cast<uint16_t>((i / 8) % 2 ? 0x4000 : 0xc000));
        }
    }

    return aw.Data();
}

// A test game: its archive and the banks and waves it plays.
struct TestGame
{
    explicit TestGame(const std::vector<uint8_t>& bank, const std::vector<std::vector<uint8_t>>& sequences)
        : archive(SoundArchive::Parse(Archive(bank, sequences))),
          banks(archive, [](const std::string& name) -> std::optional<std::vector<uint8_t>>
                { return name == "test.aw" ? std::optional(AwFile()) : std::nullopt; })
    {
    }

    SequenceSound Sound(uint32_t index) const
    {
        return {"S" + std::to_string(index), archive.Collection(), *archive.CollectionSequence(0, index), 1.0f};
    }

    SoundArchive archive;
    BankSet banks;
};

// Sequence data addresses commands from the start of the collection that holds it. The first sequence of a
// collection of one starts at kFirstSequence.
constexpr uint32_t kFirstSequence = 20;

class Sequence
{
public:
    explicit Sequence(uint32_t base = kFirstSequence) : base_(base)
    {
    }

    Sequence& Byte(uint32_t v)
    {
        b_.U8(v);
        return *this;
    }

    Sequence& OpenTrack(uint32_t track, uint32_t offset)
    {
        b_.U8(0xc1).U8(track).U24(offset);
        return *this;
    }

    Sequence& Timebase(uint32_t tb)
    {
        b_.U8(0xd8).U8(0x62).U16(tb);
        return *this;
    }

    Sequence& Tempo(uint32_t bpm)
    {
        b_.U8(0xe0).U16(bpm);
        return *this;
    }

    Sequence& Bank(uint32_t bank)
    {
        b_.U8(0xe2).U8(bank);
        return *this;
    }

    Sequence& Program(uint32_t program)
    {
        b_.U8(0xe3).U8(program);
        return *this;
    }

    Sequence& NoteOn(uint32_t key, uint32_t id, uint32_t velocity)
    {
        b_.U8(key).U8(id).U8(velocity);
        return *this;
    }

    Sequence& NoteOff(uint32_t id)
    {
        b_.U8(0x80 | id);
        return *this;
    }

    // A gate note of fewer than 128 ticks. Bit 7 of `flags` sweeps from the latest key, and bit 6 ties the next note
    // to this one.
    Sequence& Gate(uint32_t key, uint32_t flags, uint32_t velocity, uint32_t length)
    {
        b_.U8(key).U8(flags).U8(velocity).U8(length);
        return *this;
    }

    Sequence& LatestKey(uint32_t key)
    {
        b_.U8(0xb4).U8(key);
        return *this;
    }

    Sequence& Wait(uint32_t ticks)
    {
        b_.U8(0xf1).U8(ticks);
        return *this;
    }

    Sequence& Volume(uint32_t byte)
    {
        b_.U8(0xb8).U8(0).U8(byte);
        return *this;
    }

    Sequence& Jump(uint32_t offset)
    {
        b_.U8(0xc7).U24(offset);
        return *this;
    }

    Sequence& Finish()
    {
        b_.U8(0xff);
        return *this;
    }

    uint32_t Here() const
    {
        return base_ + static_cast<uint32_t>(b_.Size());
    }

    std::vector<uint8_t> Data() const
    {
        return b_.Data();
    }

private:
    uint32_t base_ = 0;
    Bytes b_;
};

// Count `perf`'s events of `kind` on MIDI track `slot`.
int Count(const Performance& perf, std::size_t slot, EventKind kind)
{
    int n = 0;
    for (const TrackEvent& e : perf.tracks[slot])
    {
        n += e.kind == kind ? 1 : 0;
    }

    return n;
}

// --- Readers for the files the converter writes, independent of its writers (revosf2's) ---

uint32_t Le32(const std::vector<uint8_t>& d, std::size_t o)
{
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}

uint16_t Le16(const std::vector<uint8_t>& d, std::size_t o)
{
    return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}

uint32_t Be32(const std::vector<uint8_t>& d, std::size_t o)
{
    return (static_cast<uint32_t>(d[o]) << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3];
}

// Offsets and sizes of SoundFont list subchunks.
struct Sf2View
{
    bool ok = false;
    std::map<std::string, std::pair<std::size_t, uint32_t>> chunks;
};

// Find SoundFont list subchunks; set `ok` false for invalid RIFF structure.
Sf2View ParseSf2(const std::vector<uint8_t>& d)
{
    Sf2View v;
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "sfbk", 4) != 0 ||
        Le32(d, 4) + 8 != d.size())
    {
        return v;
    }

    std::size_t off = 12;
    while (off + 8 <= d.size())
    {
        const uint32_t size = Le32(d, off + 4);
        if (std::memcmp(d.data() + off, "LIST", 4) != 0 || off + 8 + size > d.size())
        {
            return v;
        }

        std::size_t sub = off + 12;
        const std::size_t end = off + 8 + size;
        while (sub + 8 <= end)
        {
            const uint32_t s = Le32(d, sub + 4);
            v.chunks[std::string(reinterpret_cast<const char*>(d.data() + sub), 4)] = {sub + 8, s};
            sub += 8 + s + (s & 1);
        }
        if (sub != end)
        {
            return v;
        }

        off = end;
    }

    v.ok = off == d.size();

    return v;
}

// Like check_sf2.py, validate the SoundFont structures used by synths. Return an error description, or an empty string
// on success.
std::string Sf2Problem(const std::vector<uint8_t>& d)
{
    const Sf2View v = ParseSf2(d);
    if (!v.ok)
    {
        return "RIFF layout";
    }
    for (const char* c :
         {"ifil", "isng", "INAM", "smpl", "phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"})
    {
        if (!v.chunks.count(c))
        {
            return std::string("missing ") + c;
        }
    }

    const auto [shdr, shdr_size] = v.chunks.at("shdr");
    const auto [igen, igen_size] = v.chunks.at("igen");
    const auto [ibag, ibag_size] = v.chunks.at("ibag");
    const auto [inst, inst_size] = v.chunks.at("inst");
    const uint32_t smpl_samples = v.chunks.at("smpl").second / 2;
    if (shdr_size % 46 || igen_size % 4 || ibag_size % 4 || inst_size % 22)
    {
        return "record sizes";
    }

    const uint32_t nsamples = shdr_size / 46 - 1;
    for (uint32_t i = 0; i < nsamples; i++)
    {
        const std::size_t o = shdr + i * 46;
        const uint32_t start = Le32(d, o + 20), end = Le32(d, o + 24);
        const uint32_t ls = Le32(d, o + 28), le = Le32(d, o + 32);
        if (!(start < end && end + 46 <= smpl_samples))
        {
            return "sample bounds";
        }
        if (le > ls && (ls < start || le > end))
        {
            return "loop bounds";
        }

        const uint16_t type = Le16(d, o + 44), link = Le16(d, o + 42);
        if (type == 2 || type == 4)
        {
            if (link >= nsamples || Le16(d, shdr + link * 46 + 42) != i || Le16(d, shdr + link * 46 + 44) != 6 - type)
            {
                return "stereo link";
            }
        }
        else if (type != 1)
        {
            return "sample type";
        }
    }

    const uint32_t ninst = inst_size / 22 - 1;
    for (uint32_t i = 0; i < ninst; i++)
    {
        const uint16_t b0 = Le16(d, inst + i * 22 + 20), b1 = Le16(d, inst + (i + 1) * 22 + 20);
        for (uint16_t b = b0; b < b1; b++)
        {
            const uint16_t g0 = Le16(d, ibag + b * 4), g1 = Le16(d, ibag + (b + 1) * 4);
            std::map<uint16_t, uint16_t> gens;
            for (uint16_t g = g0; g < g1; g++)
            {
                const uint16_t oper = Le16(d, igen + g * 4);
                if (oper == 43 && g != g0)
                {
                    return "keyRange not first";
                }
                if (oper == 44 && g != g0 + (Le16(d, igen + g0 * 4) == 43 ? 1 : 0))
                {
                    return "velRange out of place";
                }
                if (oper == 53 && g != g1 - 1)
                {
                    return "sampleID not last";
                }

                gens[oper] = Le16(d, igen + g * 4 + 2);
            }

            if (gens.count(53))
            {
                if (gens[53] >= nsamples)
                {
                    return "sampleID out of range";
                }

                if (gens.count(54) && (gens[54] & 1))
                {
                    const std::size_t o = shdr + gens[53] * 46;
                    const uint32_t start = Le32(d, o + 20), end = Le32(d, o + 24);
                    const uint32_t ls = Le32(d, o + 28), le = Le32(d, o + 32);
                    if (ls - start < 8 || end - le < 8 || le - ls < 32)
                    {
                        return "loop rules";
                    }
                }
            }
        }
    }

    return "";
}

// A parsed MIDI note: track, channel, key and start/end ticks.
struct MidiNote
{
    int track = 0, channel = 0, key = 0;
    uint32_t on = 0, off = 0;
};

// Parsed MIDI track names, end ticks, notes, pitch bends and markers.
struct MidiView
{
    bool ok = false;
    uint16_t division = 0;
    std::vector<std::string> track_names;
    std::vector<uint64_t> track_ends;
    std::vector<MidiNote> notes;
    std::vector<std::pair<uint32_t, int>> bends; // tick and 14-bit value
    std::vector<std::pair<uint32_t, std::string>> markers;
};

// Read generated MIDI. Set `ok` false for malformed data, deltas over four bytes, running status or overlapping
// same-key notes on one channel.
MidiView ParseMidi(const std::vector<uint8_t>& d)
{
    MidiView v;
    if (d.size() < 14 || std::memcmp(d.data(), "MThd", 4) != 0 || Be32(d, 4) != 6)
    {
        return v;
    }

    const uint16_t ntracks = static_cast<uint16_t>((d[10] << 8) | d[11]);
    v.division = static_cast<uint16_t>((d[12] << 8) | d[13]);
    std::size_t off = 14;
    for (uint16_t t = 0; t < ntracks; t++)
    {
        if (off + 8 > d.size() || std::memcmp(d.data() + off, "MTrk", 4) != 0)
        {
            return v;
        }

        const uint32_t size = Be32(d, off + 4);
        std::size_t p = off + 8;
        const std::size_t end = p + size;
        if (end > d.size())
        {
            return v;
        }

        uint64_t tick = 0;
        std::map<std::pair<int, int>, uint32_t> on;
        std::string name;
        bool ended = false;
        while (p < end)
        {
            uint32_t delta = 0;
            uint8_t b;
            int length = 0;
            do
            {
                b = d[p++];
                delta = (delta << 7) | (b & 0x7f);
            } while ((b & 0x80) && ++length < 4);
            if (b & 0x80)
            {
                return v; // a delta time of more than 4 bytes
            }

            tick += delta;

            const uint8_t status = d[p++];
            if (status == 0xff)
            {
                const uint8_t type = d[p++];
                uint32_t len = 0;
                do
                {
                    b = d[p++];
                    len = (len << 7) | (b & 0x7f);
                } while (b & 0x80);

                const std::string text(reinterpret_cast<const char*>(d.data() + p), len);
                if (type == 0x03)
                {
                    name = text;
                }
                if (type == 0x06)
                {
                    v.markers.push_back({static_cast<uint32_t>(tick), text});
                }
                if (type == 0x2f)
                {
                    ended = true;
                }

                p += len;
                continue;
            }

            const int kind = status >> 4, ch = status & 15;
            if (kind < 8)
            {
                return v; // no running status is written
            }

            const int nbytes = (kind == 0xc || kind == 0xd) ? 1 : 2;
            const uint8_t a = d[p], bb = nbytes == 2 ? d[p + 1] : 0;
            p += nbytes;
            if (kind == 9 && bb > 0)
            {
                if (on.count({ch, a}))
                {
                    return v; // a key on twice on one channel
                }

                on[{ch, a}] = static_cast<uint32_t>(tick);
            }
            else if (kind == 8 || (kind == 9 && bb == 0))
            {
                auto it = on.find({ch, a});
                if (it != on.end())
                {
                    v.notes.push_back({t, ch, a, it->second, static_cast<uint32_t>(tick)});
                    on.erase(it);
                }
            }
            else if (kind == 0xe)
            {
                v.bends.push_back({static_cast<uint32_t>(tick), a | (bb << 7)});
            }
        }
        if (!ended || p != end || !on.empty())
        {
            return v;
        }

        v.track_names.push_back(name);
        v.track_ends.push_back(tick);
        off = end;
    }

    v.ok = off == d.size();

    return v;
}

// Convert bytes to a string for comparison.
std::string Text(std::span<const uint8_t> data)
{
    return std::string(data.begin(), data.end());
}

// Check SFZ files against their MIDI file: a file for each MIDI track, named after it, and regions whose samples
// exist, with loops inside them.
std::string SfzProblem(const SfzInstruments& sfz, const MidiView& midi)
{
    std::map<std::string, uint32_t> frames;
    for (const SfzSample& s : sfz.samples)
    {
        const std::vector<uint8_t>& w = s.wav;
        if (w.size() < 44 || Text(std::span(w).first(4)) != "RIFF" || Le32(w, 4) != w.size() - 8 ||
            Le32(w, 40) != w.size() - 44)
        {
            return "broken WAV file " + s.name;
        }

        frames[s.name] = Le32(w, 40) / 2;
    }

    std::set<std::string> names, tracks;
    for (const SfzFile& f : sfz.files)
    {
        names.insert(f.name);
    }
    for (std::size_t t = 1; t < midi.track_names.size(); t++) // skip the conductor track
    {
        const std::string& track = midi.track_names[t];
        tracks.insert(track.substr(0, track.find(" (layer")) + ".sfz");
    }
    if (names != tracks)
    {
        return std::to_string(names.size()) + " files for " + std::to_string(tracks.size()) + " tracks";
    }

    for (const SfzFile& f : sfz.files)
    {
        std::istringstream lines(f.text);
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.starts_with("<region> "))
            {
                continue;
            }

            std::map<std::string, std::string> opcodes;
            std::istringstream words(line.substr(9));
            for (std::string word; words >> word;)
            {
                opcodes[word.substr(0, word.find('='))] = word.substr(word.find('=') + 1);
            }

            const auto it = frames.find(opcodes["sample"]);
            if (it == frames.end())
            {
                return f.name + " plays a sample the sequence doesn't have: " + opcodes["sample"];
            }
            if (opcodes.count("loop_end"))
            {
                const unsigned long start = std::stoul(opcodes["loop_start"]), end = std::stoul(opcodes["loop_end"]);
                if (start >= end || end >= it->second)
                {
                    return f.name + " loops outside " + it->first;
                }
            }
        }
    }

    return "";
}

void TestAdpcm4()
{
    // A frame of predictor 1 and nothing else, and a frame whose first nibble is 1 at a shift of 2 (predictor 0).
    const std::vector<uint8_t> kFrame = {0x01, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<uint8_t> kStep = {0x20, 0x10, 0, 0, 0, 0, 0, 0, 0};

    const std::vector<int16_t> samples = DecodeAdpcm4(kFrame, 16, 100, 0);
    const std::vector<int16_t> stepped = DecodeAdpcm4(kStep, 2, 0, 0);

    Check(samples.size() == 16, "a frame holds 16 samples");
    Check(samples[0] == 100 && samples[15] == 100, "predictor 1 repeats the last sample");
    Check(stepped[0] == 4 && stepped[1] == 0, "a nibble scales by its frame's shift");
}

void TestWaveSystem()
{
    const std::vector<uint8_t> baa = Archive(BankV0(0, 100), {});

    const SoundArchive archive = SoundArchive::Parse(baa);
    const WaveSystem* ws = archive.FindWaveSystem(0);
    const std::optional<Pcm> pcm = ws != nullptr ? std::optional(DecodeWave(ws->waves.at(0), AwFile())) : std::nullopt;

    Check(ws != nullptr && ws->waves.size() == 2, "the wave system has two waves");
    Check(ws && ws->waves.at(0).loop && ws->waves.at(0).loop_start == 32, "wave 0 loops from sample 32");
    Check(ws && ws->waves.at(1).archive == "test.aw", "waves name their AW file");
    Check(pcm && pcm->Length() == 64 && pcm->loop && pcm->samples[8] == 0x4000, "PCM16 decodes big-endian");
}

void TestBanks()
{
    const Bank v0 = Bank::Parse(BankV0(7, 100), 0);
    const Bank v1 = Bank::Parse(BankV1(129, 0, 600), 0);
    const std::optional<NoteSound> drum = FindSound(v0, 0xe4, 36, 100);
    const std::optional<NoteSound> low = FindSound(v1, 0, 60, 40);
    const std::optional<NoteSound> high = FindSound(v1, 0, 60, 100);

    Check(v0.number == 7 && v0.version == 0, "a version 0 bank reads its number");
    Check(v0.programs.contains(0) && v0.programs.contains(0xe4), "drum sets are programs from 0xe4");
    Check(drum && drum->drum && drum->pitch == 2.0f && drum->volume == 0.5f, "a drum plays its region");
    Check(drum && std::abs(drum->pan - 1.0f) < 1e-6f && drum->direct_release == 200, "PER2 gives pan and release");
    Check(!FindSound(v0, 0xe4, 37, 100), "a drum set plays only its keys");
    Check(v1.version == 1 && v1.oscillators.size() == 1, "a version 1 bank reads its oscillators");
    Check(low && high && low->wave_id == 0 && high->wave_id == 1, "velocity regions choose the wave");
    Check(low && low->random, "a random effect is reported");
    Check(!FindSound(v1, 0, 65, 100), "keys above the last region play nothing");
}

void TestOscillator()
{
    // A linear attack to full over 60 units, then a hold, and a linear release over 30 units. Read the oscillator at
    // each step.
    Oscillator osc;
    osc.attack = {{0, 60, 0x7fff}, {14, 0, 0}};
    osc.release = {{0, 30, 0}, {15, 0, 0}};
    OscillatorState s;

    s.Start(osc);
    const float start = s.Value();
    s.Advance(30.0f);
    const float half = s.Value();
    s.Advance(30.0f);
    const bool holds = s.Holding();
    const float full = s.Value();

    s.Release(0);
    s.Advance(15.0f);
    const bool releasing = s.Releasing();
    const float falling = s.Value();
    s.Advance(15.0f);

    Check(start == 0.0f, "an attack starts in silence");
    Check(std::abs(half - 0.5f) < 0.001f, "halfway through a linear attack is half the level");
    Check(holds && std::abs(full - 1.0f) < 0.001f, "the attack ends at its target and holds");
    Check(releasing && std::abs(falling - 0.5f) < 0.001f, "the release falls from the held level");
    Check(s.Stopped(), "the release stops at its end");
}

void TestOscillatorWithoutReleaseTable()
{
    // Without a release table, an oscillator stops at once. A direct release fades it.
    Oscillator bare;
    bare.attack = {{0, 0, 0x7fff}, {14, 0, 0}};
    OscillatorState cut;
    OscillatorState fade;
    cut.Start(bare);
    fade.Start(bare);

    cut.Release(0);
    fade.Release(100);

    Check(cut.Stopped(), "no release table stops at once");
    Check(!fade.Stopped(), "a direct release fades");
}

void TestEnvelopeFit()
{
    Oscillator osc;
    osc.attack = {{0, 0, 0x7fff}, {0, 600, 0x7fff}, {14, 0, 0}};
    osc.release = {{0, 600, 0}, {15, 0, 0}};

    const EnvelopeCurve curve = TraceEnvelope({&osc}, 0);
    const Sf2Envelope e = ConvertEnvelope(curve);

    Check(curve.holds, "the curve holds");
    Check(e.attack == -12000 && e.sustain == 0, "an instant attack to full sustain");
    // A linear fall over a second reaches -20 dB at 0.9 s: 100 dB at 4.5 s.
    Check(std::abs(e.release - ToTimecents(4500.0, -12000, 8000)) <= 20, "a linear release fits at -20 dB");
}

void TestPerformance()
{
    // The first track opens track 0. Track 0 plays two notes and finishes.
    Sequence s;
    const uint32_t child = kFirstSequence + 0x20;
    s.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < child)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).NoteOn(64, 1, 80).Wait(48).NoteOff(1).Finish();
    TestGame game(BankV0(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});

    bool second_at_48 = false;
    for (const TrackEvent& e : perf.tracks[0])
    {
        second_at_48 = second_at_48 || (e.kind == EventKind::kNoteOn && e.key == 64 && e.tick == 48);
    }
    Check(Count(perf, 0, EventKind::kNoteOn) == 2, "the track plays two notes");
    Check(Count(perf, 0, EventKind::kNoteOff) == 2, "and ends both");
    Check(!perf.loop, "a sequence that finishes doesn't loop");
    Check(perf.timebase == 48 && !perf.tempo.empty() && perf.tempo[0].bpm == 120.0, "the tempo is recorded");
    Check(std::abs(perf.seconds - 1.1) < 0.02, "two beats at 120 BPM and a release of 60 units last 1.1 s");
    Check(second_at_48, "the second note starts at tick 48");
}

void TestSweepAndTie()
{
    // Track 0 sweeps up an octave from the latest key over a beat, and ties the next note to it. That note sweeps on
    // from where the first ended, at half the velocity.
    Sequence s;
    const uint32_t child = kFirstSequence + 0x20;
    s.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < child)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).LatestKey(60).Gate(72, 0xc0, 100, 48).Gate(72, 0x80, 50, 48).Finish();
    TestGame game(BankV0(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});

    int key = -1;
    float highest = 0.0f;
    float last = 0.0f;
    bool rises = true;
    bool between = false;
    std::vector<int> expression;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            key = e.key;
        }
        else if (e.kind == EventKind::kPitch)
        {
            highest = std::max(highest, e.semitones);
            rises = rises && e.semitones >= last;
            last = e.semitones;
            between = between || (e.after > 0.0 && e.after < 1.0);
        }
        else if (e.kind == EventKind::kExpression)
        {
            expression.push_back(e.value);
        }
    }

    Check(Count(perf, 0, EventKind::kNoteOn) == 1 && key == 60, "the tie continues the note that starts at key 60");
    Check(std::abs(highest - 12.0f) < 0.01f && rises, "pitch bends sweep the note up an octave");
    Check(between, "the sweep moves between ticks");
    Check(expression.size() >= 2 && std::abs(expression.back() * 2 - expression.front()) <= 2,
          "CC11 halves with the tied note's velocity");

    // A tie that doubles the velocity starts the MIDI note at the higher velocity, with CC11 at half until the tie.
    Sequence rising;
    rising.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Finish();
    while (rising.Here() < child)
    {
        rising.Byte(0xfe);
    }
    rising.Bank(0).Program(0).Gate(60, 0x40, 50, 24).Gate(60, 0x00, 100, 24).Finish();
    TestGame rising_game(BankV0(0, 60), {rising.Data()});

    const Performance up = Perform(rising_game.Sound(0), rising_game.banks, {});

    int velocity = 0;
    std::vector<int> levels;
    for (const TrackEvent& e : up.tracks[0])
    {
        velocity = e.kind == EventKind::kNoteOn ? e.value : velocity;
        if (e.kind == EventKind::kExpression)
        {
            levels.push_back(e.value);
        }
    }
    Check(velocity == 100 && up.level_cut_db == 0.0, "the note starts at the velocity its tie reaches");
    Check(levels.size() >= 2 && std::abs(levels.front() * 2 - levels.back()) <= 2, "and CC11 doubles at the tie");
}

void TestTieBelowHearing()
{
    // A note at velocity 50 falls to silence over a second. A tie then raises it to velocity 127. That velocity plays
    // another wave. The MIDI note can't start there. A quarter of a second in, the note can still be heard. CC11 rises
    // past 127 there. The whole sequence plays quieter. At 1.25 seconds, the note is below hearing. Its tie raises
    // nothing. A tie 50 ms into an attack of ten seconds comes before the note can be heard. That note still rises.
    auto perform = [](int16_t attack, uint32_t ticks)
    {
        Sequence s;
        const uint32_t child = kFirstSequence + 0x20;
        s.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Finish();
        while (s.Here() < child)
        {
            s.Byte(0xfe);
        }
        s.Bank(0).Program(0).Gate(60, 0x40, 50, ticks).Gate(60, 0x00, 127, 24).Finish();
        TestGame game(BankV1(0, attack, 600), {s.Data()});

        return Perform(game.Sound(0), game.banks, {});
    };

    const Performance heard = perform(0, 24);
    const Performance below = perform(0, 120);
    const Performance rising = perform(6000, 5);

    int velocity = 0;
    for (const TrackEvent& e : below.tracks[0])
    {
        velocity = e.kind == EventKind::kNoteOn ? e.value : velocity;
    }
    Check(heard.level_cut_db > 10.0, "a tie into another wave that can be heard lowers the sequence");
    Check(below.level_cut_db == 0.0, "a tie on a note below hearing lowers nothing");
    Check(Count(below, 0, EventKind::kNoteOn) == 1 && velocity == 50, "and its note starts at its first velocity");
    Check(rising.level_cut_db > 10.0, "a tie in an attack from silence still raises the note");
}

void TestGateTickRate()
{
    // The first track opens track 0, then halves the timebase. Track 0's first gate note starts in that tick, so its
    // length goes by the default tick rate: 24 ticks last as long as 12 at the new rate. The second note starts in a
    // later tick, at the new rate.
    Sequence s;
    const uint32_t child = kFirstSequence + 0x20;
    s.OpenTrack(0, child).Timebase(24).Tempo(120).Wait(200).Finish();
    while (s.Here() < child)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).Gate(60, 0x00, 100, 24).Gate(62, 0x00, 100, 24).Finish();
    TestGame game(BankV0(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});

    std::vector<uint32_t> ends;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOff)
        {
            ends.push_back(e.tick);
        }
    }
    Check(ends.size() == 2 && ends[0] >= 11 && ends[0] <= 12,
          "a note in the tick that opens its track keeps the old rate");
    Check(ends.size() == 2 && ends[1] >= 47, "a later note takes the first track's rate");
}

void TestLoop()
{
    // Track 0 loops two notes for ever.
    Sequence s;
    const uint32_t child = kFirstSequence + 0x20;
    s.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Wait(200);
    s.Jump(s.Here() - 4);
    while (s.Here() < child)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0);
    const uint32_t loop = s.Here();
    s.NoteOn(60, 1, 100).Wait(48).NoteOff(1).NoteOn(62, 1, 100).Wait(48).NoteOff(1).Jump(loop);
    TestGame game(BankV0(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});

    Check(perf.loop.has_value(), "an endless loop of notes is the main loop");
    Check(perf.loop && perf.loop->end - perf.loop->start == 96, "the loop is one pass of two beats");
    Check(perf.seconds >= 10.0, "a short loop plays for at least ten seconds");
}

void TestPortsAndRegisters()
{
    // Track 0 reads its parent's port 0, which the game writes, into register 0; if it's 0 it plays key 60, otherwise
    // key 72.
    Sequence s;
    const uint32_t child = kFirstSequence + 0x20;
    s.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < child)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).Byte(0xd6).Byte(0).Byte(0); // read the first track's port 0 into register 0
    const uint32_t jump = s.Here();
    s.Byte(0xc8).Byte(2).Byte(0).Byte(0).Byte(0); // jump if register 3 isn't 0 (target set below)
    s.NoteOn(60, 1, 100).Wait(10).NoteOff(1).Finish();
    const uint32_t other = s.Here();
    s.NoteOn(72, 1, 100).Wait(10).NoteOff(1).Finish();
    std::vector<uint8_t> data = s.Data();
    const uint32_t at = jump - kFirstSequence;
    data[at + 2] = static_cast<uint8_t>(other >> 16);
    data[at + 3] = static_cast<uint8_t>(other >> 8);
    data[at + 4] = static_cast<uint8_t>(other);
    TestGame game(BankV0(0, 60), {data});
    PerformOptions options;
    options.ports[0] = 1;

    const Performance plain = Perform(game.Sound(0), game.banks, {});
    const Performance set = Perform(game.Sound(0), game.banks, options);

    auto first_key = [](const Performance& p)
    {
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                return static_cast<int>(e.key);
            }
        }
        return -1;
    };
    Check(first_key(plain) == 60, "an unset port reads 0");
    Check(first_key(set) == 72, "--port sets a port before the sequence starts");
}

void TestDrumAndVolume()
{
    // Track 0 plays the drum set at volume 0x40.
    Sequence s;
    const uint32_t child = kFirstSequence + 0x20;
    s.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < child)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0xe4).Volume(0x40).NoteOn(36, 1, 127).Wait(10).NoteOff(1).Finish();
    TestGame game(BankV0(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});
    const std::vector<PresetZone> zones =
        perf.presets.empty() ? std::vector<PresetZone>{} : ZonesOf(perf.presets[0], perf, game.banks);

    int volume = -1;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volume = e.value;
        }
    }
    // 0x40 spreads to 0x4080: 0.5039 of 127.
    Check(volume == 64, "CC7 holds the track's volume");
    Check(perf.presets.size() == 1 && perf.presets[0].program == 0xe4, "the drum set is a preset");
    Check(zones.size() == 1 && zones[0].root_key == 36 && zones[0].low_key == 36, "a drum plays on its key");
    Check(zones.size() == 1 && std::abs(zones[0].cents - 1200.0) < 0.01, "a drum's pitch tunes its zone");
}

void TestConversion()
{
    Sequence s;
    const uint32_t child = kFirstSequence + 0x20;
    s.OpenTrack(0, child).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < child)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    TestGame game(BankV0(0, 60), {s.Data()});

    const Conversion c = ConvertSequence(game.Sound(0), game.banks, {});

    const MidiView m = ParseMidi(c.midi);
    Check(c.midi.size() > 14 && std::memcmp(c.midi.data(), "MThd", 4) == 0, "the MIDI file has a header");
    Check(
        c.sf2.size() > 12 && std::memcmp(c.sf2.data(), "RIFF", 4) == 0 && std::memcmp(c.sf2.data() + 8, "sfbk", 4) == 0,
        "the SoundFont is a RIFF sfbk");
    Check(c.samples == 1, "the SoundFont holds the one wave it plays");
    Check(c.sfz.files.size() == 1 && c.sfz.samples.size() == 1, "one SFZ file and its sample");
    Check(m.ok && m.notes.size() == 1, "an independent reader reads the MIDI file's note");
    Check(Sf2Problem(c.sf2).empty(), "an independent reader accepts the SoundFont: " + Sf2Problem(c.sf2));
    Check(SfzProblem(c.sfz, m).empty(), "the SFZ files match the MIDI file: " + SfzProblem(c.sfz, m));
}

void TestFileNames()
{
    Check(FileName("CON") == "CON_", "device names get an underscore");
    Check(FileName("a/b:c") == "a_b_c", "forbidden characters become underscores");
    Check(FileName("end.") == "end_", "a trailing dot becomes an underscore");
}

// Convert every sequence of the game at `path`, a start-up archive or a disc image, and validate each file.
void TestGameConvertsEverySequence(const char* path)
{
    std::optional<Game> game;
    try
    {
        const std::filesystem::path p(path);
        const std::string ext = p.extension().string();
        game.emplace(ext == ".baa" ? Game::FromFolder(p) : Game::FromDisc(p));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: %s\n", path, e.what());
        failures++;
        return;
    }

    BankSet banks(game->Archive(), [&](const std::string& name) { return game->ReadWaveArchive(name); });
    int looping = 0;
    for (const SequenceSound& sound : game->Sequences())
    {
        Conversion c;
        try
        {
            c = ConvertSequence(sound, banks);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", sound.name.c_str(), e.what());
            failures++;
            continue;
        }

        const MidiView m = ParseMidi(c.midi);
        const std::string sf2_problem = Sf2Problem(c.sf2);
        const std::string sfz_problem = SfzProblem(c.sfz, m);
        if (!m.ok || !sf2_problem.empty() || !sfz_problem.empty())
        {
            std::fprintf(stderr, "%s: midi %s, sf2 %s, sfz %s\n", sound.name.c_str(), m.ok ? "ok" : "broken",
                         sf2_problem.empty() ? "ok" : sf2_problem.c_str(),
                         sfz_problem.empty() ? "ok" : sfz_problem.c_str());
            failures++;
        }

        // Events of a tick go no later than the tick, and no earlier than the sub-frame of the tick before.
        const std::vector<double>& frames = c.performance.frame_ticks;
        for (std::size_t k = 0; k < frames.size(); k++)
        {
            if (frames[k] > static_cast<double>(k) || (k > 0 && frames[k] < frames[k - 1]))
            {
                std::fprintf(stderr, "%s: tick %zu's sub-frame starts at tick %g\n", sound.name.c_str(), k, frames[k]);
                failures++;
                break;
            }
        }
        looping += c.performance.loop ? 1 : 0;
    }

    std::printf("%s: %zu sequences converted and checked, %d loop\n", path, game->Sequences().size(), looping);
}

} // namespace

int main(int argc, char** argv)
{
    TestAdpcm4();
    TestWaveSystem();
    TestBanks();
    TestOscillator();
    TestOscillatorWithoutReleaseTable();
    TestEnvelopeFit();
    TestPerformance();
    TestSweepAndTie();
    TestTieBelowHearing();
    TestGateTickRate();
    TestLoop();
    TestPortsAndRegisters();
    TestDrumAndVolume();
    TestConversion();
    TestFileNames();
    if (argc > 1)
    {
        TestGameConvertsEverySequence(argv[1]);
    }
    if (failures == 0)
    {
        std::puts("all tests passed");
    }

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
