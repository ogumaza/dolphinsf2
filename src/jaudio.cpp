// SPDX-License-Identifier: MIT

// JAudio2's sound data (see jaudio.h). Four Swords Adventures' main.dol reads the start-up archive with an interpreter
// of its chunks (0x8003a5e4), and its banks with a parser for each version: version 1's at 0x8002f610 creates 'Rand'
// and 'Sens' effects for its instruments.

#include "jaudio.h"

#include <algorithm>
#include <set>
#include <utility>

#include "binary.h"

namespace dolphinsf2
{

namespace
{

// Drum sets of a version 0 bank are programs from 0xe4 on.
constexpr uint32_t kFirstDrumProgram = 0xe4;

// Version 0 banks: the instrument and drum set tables.
constexpr std::size_t kBankChunk = 0x20;
constexpr std::size_t kInstrumentTable = 0x24;
constexpr std::size_t kDrumTable = 0x3b4;

// Read an oscillator's table at `offset`: its points up to the first that holds, loops or stops (a mode above 10). The
// bank parsers stop there too (getOscTableEndPtr).
std::vector<OscPoint> ReadPoints(const Reader& r, std::size_t offset)
{
    std::vector<OscPoint> points;
    for (;;)
    {
        const OscPoint p{r.S16(offset), r.S16(offset + 2), r.S16(offset + 4)};
        points.push_back(p);
        if (p.mode > 10)
        {
            return points;
        }

        offset += 6;
        if (points.size() > 0x10000)
        {
            throw FormatError("oscillator table has no end");
        }
    }
}

// Read a velocity region: its high velocity in the top byte of the first word, the wave id in the low half of the
// second, then its volume and pitch.
VelocityRegion ReadVelocityRegion(const Reader& r, std::size_t offset)
{
    return {r.U8(offset), static_cast<uint16_t>(r.U32(offset + 4) & 0xffff), r.F32(offset + 8), r.F32(offset + 12)};
}

InstEffect RandomEffect(uint8_t target, float base, float width)
{
    InstEffect e;
    e.kind = InstEffect::Kind::kRandom;
    e.target = target;
    e.base = base;
    e.width = width;

    return e;
}

InstEffect SenseEffect(uint8_t target, uint8_t trigger, uint8_t center, float low, float high)
{
    InstEffect e;
    e.kind = InstEffect::Kind::kSense;
    e.target = target;
    e.trigger = trigger;
    e.center = center;
    e.low = low;
    e.high = high;

    return e;
}

// A version 0 oscillator: its target, rate, tables (offsets from the bank's start), scale and vertex.
Oscillator ReadOscillatorV0(const Reader& r, std::size_t offset)
{
    Oscillator osc;
    osc.target = r.U8(offset);
    osc.rate = r.F32(offset + 4);
    if (const uint32_t attack = r.U32(offset + 8))
    {
        osc.attack = ReadPoints(r, attack);
    }
    if (const uint32_t release = r.U32(offset + 12))
    {
        osc.release = ReadPoints(r, release);
    }
    osc.scale = r.F32(offset + 16);
    osc.vertex = r.F32(offset + 20);

    return osc;
}

// Read a version 0 bank: 128 instrument offsets and 12 drum set offsets (The Wind Waker's layout). Banks share
// oscillators between instruments by offset.
void ParseVersion0(const Reader& r, Bank& bank)
{
    if (!r.Magic(kBankChunk, "BANK"))
    {
        throw FormatError("bank has no BANK chunk");
    }

    std::map<uint32_t, std::size_t> oscillators; // by offset
    auto oscillator = [&](uint32_t offset)
    {
        const auto [it, added] = oscillators.try_emplace(offset, bank.oscillators.size());
        if (added)
        {
            bank.oscillators.push_back(ReadOscillatorV0(r, offset));
        }

        return it->second;
    };

    for (uint32_t program = 0; program < 128; program++)
    {
        const uint32_t at = r.U32(kInstrumentTable + 4 * program);
        if (at == 0)
        {
            continue;
        }

        Instrument inst;
        inst.volume = r.F32(at + 8);
        inst.pitch = r.F32(at + 12);
        for (int k = 0; k < 2; k++)
        {
            if (const uint32_t osc = r.U32(at + 16 + 4 * k))
            {
                inst.oscillators.push_back(oscillator(osc));
            }
        }

        // The effects keep the parser's order: the two random ones, then the two that follow the key or velocity.
        for (int k = 0; k < 2; k++)
        {
            if (const uint32_t e = r.U32(at + 24 + 4 * k))
            {
                inst.effects.push_back(RandomEffect(r.U8(e), r.F32(e + 4), r.F32(e + 8)));
            }
        }
        for (int k = 0; k < 2; k++)
        {
            if (const uint32_t e = r.U32(at + 32 + 4 * k))
            {
                inst.effects.push_back(SenseEffect(r.U8(e), r.U8(e + 1), r.U8(e + 2), r.F32(e + 4), r.F32(e + 8)));
            }
        }

        const uint32_t keys = r.U32(at + 40);
        for (uint32_t k = 0; k < keys; k++)
        {
            const uint32_t keymap = r.U32(at + 44 + 4 * k);
            KeyRegion region;
            region.high_key = r.U8(keymap);
            const uint32_t velocities = r.U32(keymap + 4);
            for (uint32_t v = 0; v < velocities; v++)
            {
                region.velocities.push_back(ReadVelocityRegion(r, r.U32(keymap + 8 + 4 * v)));
            }
            inst.keys.push_back(std::move(region));
        }
        bank.programs[program] = std::move(inst);
    }

    for (uint32_t set = 0; set < 12; set++)
    {
        const uint32_t at = r.U32(kDrumTable + 4 * set);
        if (at == 0)
        {
            continue;
        }

        // A drum set of the second kind (PER2) adds a pan and a release for each key.
        const bool per2 = r.Magic(at, "PER2");
        if (!per2 && !r.Magic(at, "PERC"))
        {
            throw FormatError("bank has a drum set of unknown kind");
        }

        DrumSet drums;
        for (int key = 0; key < 128; key++)
        {
            const uint32_t pmap = r.U32(at + 0x88 + 4 * key);
            if (pmap == 0)
            {
                continue;
            }

            Drum drum;
            drum.volume = r.F32(pmap);
            drum.pitch = r.F32(pmap + 4);
            if (per2)
            {
                drum.pan = static_cast<float>(static_cast<int8_t>(r.U8(at + 0x288 + key))) / 127.0f;
                drum.release = r.U16(at + 0x308 + 2 * key);
            }
            for (int k = 0; k < 2; k++)
            {
                if (const uint32_t e = r.U32(pmap + 8 + 4 * k))
                {
                    drum.effects.push_back(RandomEffect(r.U8(e), r.F32(e + 4), r.F32(e + 8)));
                }
            }

            const uint32_t velocities = r.U32(pmap + 16);
            for (uint32_t v = 0; v < velocities; v++)
            {
                drum.velocities.push_back(ReadVelocityRegion(r, r.U32(pmap + 20 + 4 * v)));
            }
            drums.keys[static_cast<std::size_t>(key)] = std::move(drum);
        }
        bank.programs[kFirstDrumProgram + set] = std::move(drums);
    }
}

// Read a version 1 effect: a 'Rand' or 'Sens' object at `offset` from the bank's start.
InstEffect ReadEffectV1(const Reader& r, std::size_t offset)
{
    if (r.Magic(offset, "Rand"))
    {
        return RandomEffect(r.U8(offset + 4), r.F32(offset + 8), r.F32(offset + 12));
    }
    if (r.Magic(offset, "Sens"))
    {
        return SenseEffect(r.U8(offset + 4), r.U8(offset + 5), r.U8(offset + 6), r.F32(offset + 8), r.F32(offset + 12));
    }

    throw FormatError("bank has an effect of unknown kind");
}

// Read a version 1 bank: chunks of envelopes (ENVT), oscillators (OSCT) and a list of instruments and drum sets (LIST),
// with the effects and drum maps they point to (Twilight Princess's layout, with Four Swords Adventures' effects).
void ParseVersion1(const Reader& r, Bank& bank)
{
    std::map<std::string, std::pair<std::size_t, uint32_t>> chunks; // tag: data offset and size
    for (std::size_t at = 0x20; at + 8 <= r.Size();)
    {
        const uint32_t size = r.U32(at + 4);
        const std::string tag(reinterpret_cast<const char*>(r.Sub(at, 4).data()), 4);
        chunks.try_emplace(tag, at + 8, size);
        at = (at + 0xb + size) & ~std::size_t{3};
    }
    for (const char* tag : {"ENVT", "OSCT", "LIST"})
    {
        if (!chunks.contains(tag))
        {
            throw FormatError(std::string("bank has no ") + tag + " chunk");
        }
    }

    // Oscillators: their tables are offsets into the envelope chunk's data.
    const std::size_t envelopes = chunks["ENVT"].first;
    const std::size_t osct = chunks["OSCT"].first;
    const uint32_t oscillators = r.U32(osct);
    for (uint32_t i = 0; i < oscillators; i++)
    {
        const std::size_t at = osct + 4 + 0x1c * i;
        if (!r.Magic(at, "Osci"))
        {
            throw FormatError("bank has a damaged oscillator");
        }

        Oscillator osc;
        osc.target = r.U8(at + 4);
        osc.rate = r.F32(at + 8);
        osc.attack = ReadPoints(r, envelopes + r.U32(at + 12));
        osc.release = ReadPoints(r, envelopes + r.U32(at + 16));
        osc.release_is_attack = r.U32(at + 12) == r.U32(at + 16);
        osc.scale = r.F32(at + 20);
        osc.vertex = r.F32(at + 24);
        bank.oscillators.push_back(std::move(osc));
    }

    const std::size_t list = chunks["LIST"].first;
    const uint32_t programs = r.U32(list);
    for (uint32_t program = 0; program < programs; program++)
    {
        std::size_t at = r.U32(list + 4 + 4 * program);
        if (at == 0)
        {
            continue;
        }

        if (r.Magic(at, "Inst"))
        {
            Instrument inst;
            at += 4;
            const uint32_t oscs = r.U32(at);
            at += 4;
            for (uint32_t k = 0; k < oscs; k++, at += 4)
            {
                const uint32_t index = r.U32(at);
                if (index >= bank.oscillators.size())
                {
                    throw FormatError("instrument names a missing oscillator");
                }
                inst.oscillators.push_back(index);
            }

            const uint32_t effects = r.U32(at);
            at += 4;
            for (uint32_t k = 0; k < effects; k++, at += 4)
            {
                inst.effects.push_back(ReadEffectV1(r, r.U32(at)));
            }

            const uint32_t keys = r.U32(at);
            at += 4;
            for (uint32_t k = 0; k < keys; k++)
            {
                KeyRegion region;
                region.high_key = r.U8(at);
                const uint32_t velocities = r.U32(at + 4);
                at += 8;
                for (uint32_t v = 0; v < velocities; v++, at += 16)
                {
                    region.velocities.push_back(ReadVelocityRegion(r, at));
                }
                inst.keys.push_back(std::move(region));
            }
            inst.volume = r.F32(at);
            inst.pitch = r.F32(at + 4);
            bank.programs[program] = std::move(inst);
        }
        else if (r.Magic(at, "Perc"))
        {
            DrumSet drums;
            const uint32_t count = std::min<uint32_t>(r.U32(at + 4), 128);
            for (uint32_t key = 0; key < count; key++)
            {
                std::size_t pmap = r.U32(at + 8 + 4 * key);
                if (pmap == 0)
                {
                    continue;
                }
                if (!r.Magic(pmap, "Pmap"))
                {
                    throw FormatError("drum set has a damaged key");
                }

                Drum drum;
                drum.volume = r.F32(pmap + 4);
                drum.pitch = r.F32(pmap + 8);
                drum.pan = static_cast<float>(r.U8(pmap + 12)) / 127.0f;
                drum.release = r.U16(pmap + 14);
                pmap += 16;
                const uint32_t effects = r.U32(pmap);
                pmap += 4;
                for (uint32_t k = 0; k < effects; k++, pmap += 4)
                {
                    drum.effects.push_back(ReadEffectV1(r, r.U32(pmap)));
                }

                const uint32_t velocities = r.U32(pmap);
                pmap += 4;
                for (uint32_t v = 0; v < velocities; v++, pmap += 16)
                {
                    drum.velocities.push_back(ReadVelocityRegion(r, pmap));
                }
                drums.keys[key] = std::move(drum);
            }
            bank.programs[program] = std::move(drums);
        }
        else
        {
            throw FormatError("bank lists a program of unknown kind");
        }
    }
}

// Read a wave system: its archives (WINF) and the wave ids that its scenes give their waves (WBCT). JAudio2 gives each
// wave the id in the low half of its scene's entry (JASWSParser).
WaveSystem ParseWaveSystem(std::span<const uint8_t> file, uint32_t number)
{
    const Reader r(file);
    if (!r.Magic(0, "WSYS"))
    {
        throw FormatError("wave system has no WSYS header");
    }

    WaveSystem ws;
    ws.number = number;
    const uint32_t winf = r.U32(0x10);
    const uint32_t wbct = r.U32(0x14);
    const uint32_t archives = r.U32(winf + 4);
    const uint32_t groups = r.U32(wbct + 8);
    for (uint32_t g = 0; g < std::min(archives, groups); g++)
    {
        const uint32_t archive = r.U32(winf + 8 + 4 * g);
        const std::string name = r.CString(archive, 0x70);
        const uint32_t scene = r.U32(wbct + 12 + 4 * g);
        const uint32_t ids = r.U32(scene + 12);
        const uint32_t waves = std::min(r.U32(archive + 0x70), r.U32(ids + 4));
        for (uint32_t w = 0; w < waves; w++)
        {
            const uint32_t at = r.U32(archive + 0x74 + 4 * w);
            WaveInfo info;
            info.format = static_cast<WaveFormat>(r.U8(at + 1));
            if (r.U8(at + 1) > 3)
            {
                throw FormatError("wave system has a wave of unknown format");
            }
            info.base_key = r.U8(at + 2);
            info.sample_rate = r.F32(at + 4);
            info.offset = r.U32(at + 8);
            info.size = r.U32(at + 12);
            info.loop = r.U32(at + 16) != 0;
            info.loop_start = r.U32(at + 20);
            info.loop_end = r.U32(at + 24);
            info.sample_count = r.U32(at + 28);
            info.loop_last = r.S16(at + 32);
            info.loop_penult = r.S16(at + 34);
            info.archive = name;
            const auto id = static_cast<uint16_t>(r.U32(r.U32(ids + 8 + 4 * w)) & 0xffff);
            ws.waves[id] = std::move(info);
        }
    }

    return ws;
}

} // namespace

Bank Bank::Parse(std::span<const uint8_t> file, uint32_t wave_system)
{
    const Reader r(file);
    if (!r.Magic(0, "IBNK"))
    {
        throw FormatError("bank has no IBNK header");
    }

    Bank bank;
    bank.number = r.U32(8);
    bank.version = r.U32(12);
    bank.wave_system = wave_system;
    const Reader in_bank(file.first(std::min<std::size_t>(file.size(), r.U32(4))));
    switch (bank.version)
    {
    case 0:
        ParseVersion0(in_bank, bank);
        break;
    case 1:
        ParseVersion1(in_bank, bank);
        break;
    default:
        throw FormatError("bank of unknown version " + std::to_string(bank.version));
    }

    return bank;
}

SoundArchive SoundArchive::Parse(std::vector<uint8_t> baa)
{
    SoundArchive archive;
    archive.data_ = std::move(baa);
    const Reader r(archive.data_);
    if (r.Size() < 8 || !r.Magic(0, "AA_<"))
    {
        throw FormatError("not a JAudio2 start-up archive (BAA)");
    }

    // The chunk list ends at ">_AA". Each chunk's arguments are offsets into the archive (JAUAudioArcInterpreter).
    std::span<const uint8_t> bst, bstn;
    std::vector<std::pair<uint32_t, uint32_t>> banks; // wave system number, offset
    std::size_t at = 4;
    for (;;)
    {
        const std::string tag(reinterpret_cast<const char*>(r.Sub(at, 4).data()), 4);
        at += 4;
        if (tag == ">_AA")
        {
            break;
        }

        if (tag == "ws  ")
        {
            const uint32_t number = r.U32(at), offset = r.U32(at + 4);
            at += 12;
            const Reader ws(r.Sub(offset, 8));
            archive.wave_systems_.push_back(ParseWaveSystem(r.Sub(offset, ws.U32(4)), number));
        }
        else if (tag == "bnk ")
        {
            banks.emplace_back(r.U32(at), r.U32(at + 4));
            at += 8;
        }
        else if (tag == "bst " || tag == "bstn" || tag == "bsc ")
        {
            const uint32_t start = r.U32(at), end = r.U32(at + 4);
            at += 8;
            if (end < start)
            {
                throw FormatError("archive has a chunk that ends before it starts");
            }
            const std::span<const uint8_t> chunk = r.Sub(start, end - start);
            if (tag == "bst ")
            {
                bst = chunk;
            }
            else if (tag == "bstn")
            {
                bstn = chunk;
            }
            else
            {
                archive.collection_offset_ = start;
                archive.collection_size_ = end - start;
            }
        }
        else if (tag == "bl_<" || tag == "vbnk")
        {
            at += 8;
        }
        else if (tag == "bms ")
        {
            at += 12;
        }
        else if (tag == "bmsa" || tag == "dsqb" || tag == "bsft" || tag == "sect")
        {
            at += 4;
        }
        else if (tag != ">_bl")
        {
            throw FormatError("archive has a chunk of unknown kind");
        }
    }

    // Banks: each plays the wave system that its chunk names.
    for (const auto& [wave_system, offset] : banks)
    {
        try
        {
            const Reader header(r.Sub(offset, 8));
            archive.banks_.push_back(Bank::Parse(r.Sub(offset, header.U32(4)), wave_system));
        }
        catch (const FormatError& e)
        {
            archive.warnings_.push_back("bank at offset " + std::to_string(offset) + ": " + e.what());
        }
    }

    // The sequence collection: "SC", a category count, its size, then each category's table of sequence offsets.
    if (archive.collection_size_ > 0)
    {
        const Reader sc(archive.Collection());
        if (sc.U8(0) != 'S' || sc.U8(1) != 'C')
        {
            throw FormatError("archive's sequence collection has no SC header");
        }
        const uint16_t categories = sc.U16(2);
        for (uint16_t c = 0; c < categories; c++)
        {
            const uint32_t table = sc.U32(8 + 4 * c);
            std::vector<uint32_t> offsets;
            const uint32_t count = sc.U32(table);
            for (uint32_t i = 0; i < count; i++)
            {
                offsets.push_back(sc.U32(table + 4 + 4 * i));
            }
            archive.collection_.push_back(std::move(offsets));
        }
    }

    if (!bst.empty())
    {
        archive.ReadSoundTable(bst, bstn);
    }

    return archive;
}

void SoundArchive::ReadSoundTable(std::span<const uint8_t> bst, std::span<const uint8_t> bstn)
{
    // The table: sections of groups of sounds. Each sound's word holds its type in the top byte and the offset of its
    // entry below. The name table has the same shape, with a name before each section's and group's list.
    const Reader table(bst);
    const std::optional<Reader> names = bstn.empty() ? std::nullopt : std::optional<Reader>(Reader(bstn));
    const uint32_t root = table.U32(12);
    const std::optional<uint32_t> names_root = names ? std::optional<uint32_t>(names->U32(12)) : std::nullopt;
    const uint32_t sections = table.U32(root);
    for (uint32_t s = 0; s < sections; s++)
    {
        const uint32_t section = table.U32(root + 4 + 4 * s);
        const std::optional<uint32_t> names_section =
            names ? std::optional<uint32_t>(names->U32(*names_root + 4 + 4 * s)) : std::nullopt;
        const uint32_t groups = table.U32(section);
        for (uint32_t g = 0; g < groups; g++)
        {
            const uint32_t group = table.U32(section + 4 + 4 * g);
            const std::optional<uint32_t> names_group =
                names_section ? std::optional<uint32_t>(names->U32(*names_section + 8 + 4 * g)) : std::nullopt;
            const uint32_t items = table.U32(group);
            for (uint32_t i = 0; i < items; i++)
            {
                const uint32_t word = table.U32(group + 8 + 4 * i);
                const uint32_t entry = word & 0xffffff;
                SoundEntry sound;
                sound.section = static_cast<uint8_t>(s);
                sound.group = static_cast<uint8_t>(g);
                sound.index = static_cast<uint16_t>(i);
                // The entry's third byte is the sound's volume in 127ths.
                sound.volume = table.U8(entry + 2);
                if (names_group)
                {
                    sound.name = names->CString(names->U32(*names_group + 8 + 4 * i), 0x100);
                }

                // A sound of the BGM type plays a file of the sequence archive. The low half of the entry's third word
                // names the file. The others play the collection's sequence at their place in their group, which is
                // their category in the collection.
                if (word >> 24 == 0x11)
                {
                    sound.source = SoundEntry::Source::kArchive;
                    sound.resource = table.U32(entry + 8) & 0xffff;
                }
                else
                {
                    sound.source = SoundEntry::Source::kCollection;
                    sound.resource = i;
                }
                sounds_.push_back(std::move(sound));
            }
        }
    }
}

const Bank* SoundArchive::FindBank(uint32_t number) const
{
    for (const Bank& bank : banks_)
    {
        if (bank.number == number)
        {
            return &bank;
        }
    }

    return nullptr;
}

const WaveSystem* SoundArchive::FindWaveSystem(uint32_t number) const
{
    for (const WaveSystem& ws : wave_systems_)
    {
        if (ws.number == number)
        {
            return &ws;
        }
    }

    return nullptr;
}

std::span<const uint8_t> SoundArchive::Collection() const
{
    return std::span<const uint8_t>(data_).subspan(collection_offset_, collection_size_);
}

std::optional<uint32_t> SoundArchive::CollectionSequence(uint32_t category, uint32_t index) const
{
    if (category >= collection_.size() || index >= collection_[category].size())
    {
        return std::nullopt;
    }

    return collection_[category][index];
}

std::vector<ArchiveFile> ReadRarc(std::span<const uint8_t> rarc)
{
    const Reader r(rarc);
    if (r.Size() < 0x40 || !r.Magic(0, "RARC"))
    {
        throw FormatError("not an RARC archive");
    }

    // Offsets in the info block count from its start at 0x20, and file data from the data offset.
    const std::size_t data = std::size_t{r.U32(0x0c)} + 0x20;
    const uint32_t entries = r.U32(0x28);
    const std::size_t entry_table = std::size_t{r.U32(0x2c)} + 0x20;
    const std::size_t strings = std::size_t{r.U32(0x34)} + 0x20;
    std::vector<ArchiveFile> files;
    for (uint32_t i = 0; i < entries; i++)
    {
        const std::size_t entry = entry_table + 20 * i;
        const uint8_t kind = r.U8(entry + 4);
        if (kind & 0x02)
        {
            continue; // a folder
        }

        ArchiveFile file;
        file.id = r.U16(entry);
        file.name = r.CString(strings + r.U16(entry + 6), 0x100);
        file.data = r.Sub(data + r.U32(entry + 8), r.U32(entry + 12));
        files.push_back(std::move(file));
    }

    return files;
}

} // namespace dolphinsf2
