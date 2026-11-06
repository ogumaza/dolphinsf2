// SPDX-License-Identifier: MIT

// A piece of Square's Red driver, played as the driver plays it, and its MIDI, SoundFont and SFZ files. The names in
// the comments are the driver's (_MidiTrackExecute, __MidiCtrl_KeyOnNote and so on).
//
// The driver's levels are lines in amplitude. A MIDI value gives the square of its fraction of 127. Each value that
// dolphinsf2 sends is therefore the square root of the driver's factor. The driver's pan table isn't a sine law. CC10
// gives the ratio of its two sides, and CC11 their total.
//
// A piece can have more tracks with notes than MIDI has channels. Tracks then share channels (ChannelsOf). A channel
// plays a note with the preset of the note's track. A preset change leaves the notes that sound alone, but a controller
// changes them all. Tracks with different controllers that sound at once can't share a channel without a fault.

#include "red_music.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/wave/decode.h"
#include "seqlib/midi/recorder.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/sfz/track_file.h"
#include "xsflib/bytes.h"

#include "fade_fit.h"

namespace dolphinsf2::red
{
namespace
{

namespace sf2 = seqlib::sf2;

using seqlib::midi::Recorder;
using seqlib::midi::TrackOutput;
using seqlib::sfz::Number;

// The folders of the pieces and the banks on the disc, in lower case.
constexpr std::string_view kMusicFolder = "dvd/sound/music/";
constexpr std::string_view kWaveFolder = "dvd/sound/wave/";

// A piece's header: its bank at 6, its tracks at 8, its volume at 0xc and its tracks' data at 0x20. Each track's size
// is in little-endian order.
constexpr std::size_t kPieceHeader = 0x20;

// A bank's header: the bytes of its samples at 4, its instruments at 8 and a table of their first regions at 0x20. The
// samples end the file.
constexpr std::size_t kBankTable = 0x20;
constexpr std::size_t kRegionSize = 0x60;

// A region's flags: the next record holds the right side of a stereo wave, the instrument's last region, and that the
// instrument's first region starts a list of splits. Only the first region's flags decide that (_WaveSplitSelect).
constexpr uint32_t kRegionStereo = 0x1;
constexpr uint32_t kRegionLast = 0x200;
constexpr uint32_t kRegionSplits = 0x30000;

// A region's fixed pan: its byte at 0x1b holds this flag and the pan.
constexpr uint8_t kFixedPan = 0x80;

// The rate of the DSP's mixing in hertz, and the samples of one of AX's frames. A wave plays at this rate at a ratio of
// 1, on key 60 with a pitch of 0.
constexpr double kDspRate = 32028.5;
constexpr double kFrameSamples = 160.0;
constexpr double kFramesPerSecond = kDspRate / kFrameSamples;

// The driver runs a tick each time that the tempos of the frames add up to this value. The pieces have 48 ticks a beat.
constexpr double kTempoPerTick = 250.0;
constexpr double kTicksPerBeat = 48.0;

// The key at which a wave plays at its pitch.
constexpr uint8_t kRootKey = 60;

// The driver's scale of a level: a value of x gives (x + 1) / 128. Its centre pan and its highest pan.
constexpr double kLevelScale = 128.0;
constexpr int kCentre = 64;
constexpr int kHighestPan = 127;

// The longest performance, in seconds: a piece that hasn't repeated or ended by then stops there.
constexpr double kLimitSeconds = 15.0 * 60.0;

// The most channels of a MIDI file.
constexpr std::size_t kChannels = 16;

// The lowest volume that an SFZ region takes, in decibels.
constexpr double kSfzLowestVolumeDb = -144.0;

// The amplitude of a side at `pan`, from 0 at the side itself to 127 at the other side: the driver's table
// (t_PanningData). The table runs in two lines, through 182/256 at the pan 64.
double PanLevel(int pan)
{
    const double value = pan < kCentre ? 256.0 - pan * 74.0 / 63.0 : (kHighestPan - pan) * 182.0 / 63.0;

    return value / 256.0;
}

// The place of `pan` between the sides on a sine law, from 0 at the left to 1 at the right.
double SinePlace(int pan)
{
    return std::atan2(PanLevel(kHighestPan - pan), PanLevel(pan)) * 2.0 / std::numbers::pi;
}

// The amplitude of a level `value`.
double LevelOf(int value)
{
    return value == 0 ? 0.0 : (value + 1) / kLevelScale;
}

// A region of a bank's instrument: the keys that it plays, the wave's place in the bank and how it plays.
struct Region
{
    uint32_t sample = 0; // the byte offset of its wave in the bank's samples
    int32_t loop = -1;   // the nibble at which the loop starts, or less than 0 for no loop
    uint32_t end = 0;    // the last nibble
    int pitch = 0;       // in 1/256 of a semitone
    uint8_t low = 0;
    uint8_t high = 127;
    uint8_t volume = 127;
    int pan = -1; // 0 to 127, or -1 for the track's pan
    int16_t yn1 = 0;
    int16_t yn2 = 0;
    std::array<uint16_t, 4> times{};
    std::array<uint8_t, 4> levels{};
    std::array<int16_t, 16> coefs{};
};

// The sample of a nibble in DSP-ADPCM: a frame holds 16 nibbles, and its first 2 hold the frame's header.
uint32_t SampleOfNibble(uint32_t nibble)
{
    return nibble / 16 * 14 + std::max(nibble % 16, 2u) - 2;
}

// A bank's instruments, and its waves once they're decoded.
class Bank
{
public:
    Bank(int number, std::vector<uint8_t> data) : number_(number), data_(std::move(data))
    {
        Read();
    }

    // The regions of instrument `instrument`, or nothing if the bank has no such instrument.
    std::span<const Region> Regions(std::size_t instrument) const
    {
        return instrument < instruments_.size() ? std::span<const Region>(instruments_[instrument])
                                                : std::span<const Region>();
    }

    int Number() const
    {
        return number_;
    }

    // The decoded wave of `region`, or nullptr if its wave starts past the end of the bank.
    const Pcm* Wave(const Region& region)
    {
        const auto [found, inserted] = waves_.try_emplace(region.sample);
        if (inserted)
        {
            found->second = Decode(region);
        }

        return found->second ? &*found->second : nullptr;
    }

private:
    uint32_t Word(std::size_t at) const
    {
        if (at + 4 > data_.size())
        {
            throw FormatError("a bank ends inside its header or a region");
        }

        return xsflib::LoadBe32(data_.data() + at);
    }

    Region ReadRegion(std::size_t at) const
    {
        if (at + kRegionSize > data_.size())
        {
            throw FormatError("a bank ends inside a region");
        }

        const uint8_t* p = data_.data() + at;
        Region r;
        r.sample = xsflib::LoadBe32(p + 4);
        r.loop = static_cast<int32_t>(xsflib::LoadBe32(p + 8));
        r.end = xsflib::LoadBe32(p + 0xc);
        r.pitch = static_cast<int32_t>(xsflib::LoadBe32(p + 0x14)) >> 16;
        r.volume = p[0x1a] & 0x7f;
        if ((p[0x1b] & kFixedPan) != 0)
        {
            const int pan = p[0x1b] & 0x7f;
            r.pan = pan == 0 ? kCentre : pan;
        }
        for (std::size_t i = 0; i < r.coefs.size(); i++)
        {
            r.coefs[i] = static_cast<int16_t>(xsflib::LoadBe16(p + 0x22 + 2 * i));
        }
        r.yn1 = static_cast<int16_t>(xsflib::LoadBe16(p + 0x46));
        r.yn2 = static_cast<int16_t>(xsflib::LoadBe16(p + 0x48));
        for (std::size_t i = 0; i < r.times.size(); i++)
        {
            r.times[i] = xsflib::LoadBe16(p + 0x50 + 2 * i);
            r.levels[i] = p[0x58 + i];
        }

        return r;
    }

    void Read()
    {
        if (data_.size() < kBankTable || data_[0] != 'W' || data_[1] != 'D')
        {
            throw FormatError("a bank doesn't start with the magic WD");
        }

        const uint32_t samples = Word(4);
        const uint32_t count = Word(8);
        if (samples > data_.size() || uint64_t{count} * 4 + kBankTable > data_.size())
        {
            throw FormatError("a bank's samples or its table of instruments run past its end");
        }
        samples_ = data_.size() - samples;

        instruments_.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            const std::size_t first = Word(kBankTable + std::size_t{i} * 4);
            if (first != 0)
            {
                ReadInstrument(first, instruments_[i]);
            }
        }
    }

    // The driver takes the first region whose highest key isn't below the key. The last region takes every key above.
    void ReadInstrument(std::size_t at, std::vector<Region>& regions) const
    {
        const bool splits = (Word(at) & kRegionSplits) != 0;
        int low = 0;
        while (low <= 127)
        {
            const uint32_t flags = Word(at);
            const std::size_t records = (flags & kRegionStereo) != 0 ? 2 : 1;
            const bool last = !splits || (flags & kRegionLast) != 0;
            const int high = last ? 127 : static_cast<int8_t>(ReadByte(at + 0x18));
            if (high >= low)
            {
                for (std::size_t side = 0; side < records; side++)
                {
                    Region r = ReadRegion(at + side * kRegionSize);
                    r.low = static_cast<uint8_t>(low);
                    r.high = static_cast<uint8_t>(high);
                    if (records == 2)
                    {
                        r.pan = side == 0 ? 0 : kHighestPan;
                    }
                    if (r.end != 0)
                    {
                        regions.push_back(r);
                    }
                }
                low = high + 1;
            }

            if (last)
            {
                break;
            }

            at += records * kRegionSize;
        }
    }

    uint8_t ReadByte(std::size_t at) const
    {
        if (at >= data_.size())
        {
            throw FormatError("a bank ends inside a region");
        }

        return data_[at];
    }

    std::optional<Pcm> Decode(const Region& region) const
    {
        const uint64_t start = uint64_t{samples_} + region.sample;
        if (start >= data_.size())
        {
            return std::nullopt;
        }

        // The right side of a stereo wave can end past the bank's samples. The wave then ends with the bank.
        const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(region.end, (data_.size() - start) * 2 - 1));

        ntdlib::wave::EncodedChannel channel;
        channel.data = std::span<const uint8_t>(data_).subspan(static_cast<std::size_t>(start));
        channel.coefs = &region.coefs;
        channel.yn1 = region.yn1;
        channel.yn2 = region.yn2;

        ntdlib::wave::EncodedWave wave;
        wave.encoding = ntdlib::wave::Encoding::kDspAdpcm;
        wave.sum = ntdlib::wave::DspAdpcmSum::kAccelerator;
        wave.loop_end = SampleOfNibble(end) + 1;
        if (region.loop >= 0 && SampleOfNibble(static_cast<uint32_t>(region.loop)) < wave.loop_end)
        {
            wave.loop = true;
            wave.loop_start = SampleOfNibble(static_cast<uint32_t>(region.loop));
        }
        wave.channels.push_back(channel);

        ntdlib::wave::Pcm decoded = ntdlib::wave::DecodeWave(wave);
        Pcm pcm;
        pcm.loop = decoded.loop;
        pcm.loop_start = decoded.loop_start;
        pcm.loop_end = decoded.loop_end;
        pcm.channels = std::move(decoded.channels);
        pcm.sample_rate = static_cast<float>(kDspRate);

        return pcm;
    }

    int number_ = 0;
    std::vector<uint8_t> data_;
    std::size_t samples_ = 0;
    std::vector<std::vector<Region>> instruments_;
    std::map<uint32_t, std::optional<Pcm>> waves_;
};

// A piece's file: its bank, its volume and the data of each track.
struct PieceFile
{
    int bank = 0;
    int volume = 0;
    std::vector<std::span<const uint8_t>> tracks;
};

PieceFile ReadPiece(std::span<const uint8_t> data)
{
    if (data.size() < kPieceHeader || std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "BGM ")
    {
        throw FormatError("a piece doesn't start with the magic BGM");
    }

    PieceFile piece;
    piece.bank = xsflib::LoadBe16(data.data() + 6);
    piece.volume = xsflib::LoadBe16(data.data() + 0xc) & 0x7f;
    std::size_t at = kPieceHeader;
    for (int t = 0; t < data[8]; t++)
    {
        if (at + 4 > data.size())
        {
            throw FormatError("a piece ends before its last track");
        }

        const uint32_t size = xsflib::LoadLe32(data.data() + at);
        at += 4;
        if (size > data.size() - at)
        {
            throw FormatError("a track runs past the end of its piece");
        }
        piece.tracks.push_back(data.subspan(at, size));
        at += size;
    }

    return piece;
}

// A note of a track as it sounds, for the choice of the channels: its time, its key, and a number that stands for the
// controllers that it plays with.
struct TrackNote
{
    double start = 0.0;
    double end = 0.0;
    uint8_t key = 0;
    int settings = 0;
};

using TrackNotes = std::vector<TrackNote>;

// The seconds that notes of `a` and `b` sound together and can't share a channel: those with other controllers, or on
// the same key.
double Conflict(const TrackNotes& a, const TrackNotes& b)
{
    double total = 0.0;
    std::size_t first = 0;
    for (const TrackNote& n : a)
    {
        while (first < b.size() && b[first].end <= n.start)
        {
            first++;
        }
        for (std::size_t i = first; i < b.size() && b[i].start < n.end; i++)
        {
            const TrackNote& m = b[i];
            if (m.settings != n.settings || m.key == n.key)
            {
                total += std::max(std::min(m.end, n.end) - std::max(m.start, n.start), 0.0);
            }
        }
    }

    return total;
}

// The channel of each track, and how many pairs of tracks that share a channel conflict.
struct ChannelChoice
{
    std::vector<uint8_t> channels;
    uint32_t conflicts = 0;
};

// Each track with notes takes a channel while channels last. Past that, the two groups of tracks with the least
// conflict share one, until they fit. Tracks without notes take channel 0.
ChannelChoice ChannelsOf(const std::vector<TrackNotes>& tracks)
{
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t t = 0; t < tracks.size(); t++)
    {
        if (!tracks[t].empty())
        {
            groups.push_back({t});
        }
    }

    ChannelChoice choice;
    std::vector<std::vector<double>> conflict(tracks.size(), std::vector<double>(tracks.size(), 0.0));
    if (groups.size() > kChannels)
    {
        for (std::size_t a = 0; a < tracks.size(); a++)
        {
            for (std::size_t b = a + 1; b < tracks.size(); b++)
            {
                conflict[a][b] = conflict[b][a] = Conflict(tracks[a], tracks[b]);
            }
        }
    }

    const auto between = [&conflict](const std::vector<std::size_t>& a, const std::vector<std::size_t>& b)
    {
        double sum = 0.0;
        for (const std::size_t x : a)
        {
            for (const std::size_t y : b)
            {
                sum += conflict[x][y];
            }
        }

        return sum;
    };

    while (groups.size() > kChannels)
    {
        std::size_t best_a = 0;
        std::size_t best_b = 1;
        double best = between(groups[0], groups[1]);
        for (std::size_t a = 0; a < groups.size(); a++)
        {
            for (std::size_t b = a + 1; b < groups.size(); b++)
            {
                const double sum = between(groups[a], groups[b]);
                if (sum < best)
                {
                    best = sum;
                    best_a = a;
                    best_b = b;
                }
            }
        }
        for (const std::size_t x : groups[best_a])
        {
            for (const std::size_t y : groups[best_b])
            {
                choice.conflicts += conflict[x][y] > 0.0 ? 1 : 0;
            }
        }
        groups[best_a].insert(groups[best_a].end(), groups[best_b].begin(), groups[best_b].end());
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(best_b));
    }

    choice.channels.assign(tracks.size(), 0);
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        for (const std::size_t t : groups[g])
        {
            choice.channels[t] = static_cast<uint8_t>(g);
        }
    }

    return choice;
}

// A note that a track holds.
struct Held
{
    uint8_t key = 0;
    uint32_t note = 0;
    std::size_t notes_index = 0; // its place in the track's TrackNotes
};

// A key-on that waits for the end of the tick. The driver starts a note in the next frame (KeyOnReserve), unless a
// key-off of its key comes first (KeyOffSet).
struct Pending
{
    uint8_t key = 0;
    uint8_t velocity = 0;
};

// A track's state.
struct Track
{
    std::span<const uint8_t> data;
    std::size_t at = 0;
    int wait = 0;
    bool finished = false;
    uint8_t key = 0;
    uint8_t velocity = 0;
    int instrument = -1;
    int volume = 127;
    int pan = kCentre;
    int reverb = 0;
    bool reverb_on = false;
    int bend = 0;
    int bend_range = 2;
    std::vector<Pending> pending;
    std::vector<Held> held;
};

// What __MidiCtrl_WholeLoopStart saves of a track. The volume, the pan and the wave stay as they are at the loop's end.
struct Saved
{
    std::size_t at = 0;
    int wait = 0;
    bool finished = false;
    uint8_t key = 0;
    uint8_t velocity = 0;
};

// A performance and the instruments of its presets. Element i belongs to preset i.
struct Played
{
    Performance performance;
    std::vector<int> instruments;
};

// A run of a piece, with each track on the channel that `channels` gives it.
class Performer
{
public:
    Performer(const PieceFile& piece, const Bank& bank, const PerformOptions& options, const ChannelChoice& channels)
        : piece_(piece), bank_(bank), options_(options), channels_(channels.channels), shared_(channels_.size())
    {
        for (std::size_t c = 0; c < kChannels; c++)
        {
            outputs_[c].midi_track = static_cast<uint8_t>(c);
        }

        std::array<std::size_t, kChannels> counts{};
        for (const uint8_t channel : channels_)
        {
            counts[channel]++;
        }

        for (std::size_t t = 0; t < channels_.size(); t++)
        {
            shared_[t] = counts[channels_[t]] > 1;
        }

        if (channels.conflicts != 0)
        {
            approximations_["tracks with different controllers that share a channel"] = channels.conflicts;
        }
    }

    Played Run();

    // The notes of each track in the run.
    const std::vector<TrackNotes>& Notes() const
    {
        return notes_;
    }

private:
    uint8_t Byte(Track& t) const
    {
        if (t.at >= t.data.size())
        {
            throw FormatError("a track runs past its end");
        }

        return t.data[t.at++];
    }

    uint32_t Delta(Track& t) const
    {
        uint32_t value = 0;
        for (;;)
        {
            const uint8_t b = Byte(t);
            value = value << 7 | (b & 0x7f);
            if ((b & 0x80) == 0)
            {
                return value;
            }
        }
    }

    void TrackTick(std::size_t index);
    void Command(std::size_t index, uint8_t command);
    void KeyOn(Track& t);
    void KeyOff(std::size_t index);
    void StartNotes(std::size_t index);
    void SendControllers(std::size_t index);
    void SaveLoop();
    void RestoreLoop();
    uint32_t PresetOf(int instrument);
    int SettingsOf(const Track& t);
    double TailSeconds(const Track& t) const;

    const PieceFile& piece_;
    const Bank& bank_;
    const PerformOptions& options_;
    std::vector<uint8_t> channels_;
    std::vector<bool> shared_;
    std::vector<Track> tracks_;
    std::vector<TrackNotes> notes_;
    std::array<TrackOutput, kChannels> outputs_{};
    Recorder recorder_;
    Played out_;
    std::map<int, uint32_t> presets_;
    std::map<std::tuple<int, int, int, int>, int> settings_;
    std::vector<Saved> saved_;
    uint32_t tick_ = 0;
    double seconds_ = 0.0;
    int tempo_ = 1;
    std::size_t active_ = 0;
    std::optional<uint32_t> loop_start_;
    bool loop_starts_ = false;
    bool loop_ends_ = false;
    std::map<std::string, uint32_t> approximations_;
};

int Performer::SettingsOf(const Track& t)
{
    const auto key = std::make_tuple(t.volume, t.pan, t.bend * t.bend_range, t.reverb_on ? t.reverb : 0);

    return settings_.emplace(key, static_cast<int>(settings_.size())).first->second;
}

// The longest that a note of the track's instrument sounds after its release.
double Performer::TailSeconds(const Track& t) const
{
    double frames = 0.0;
    if (t.instrument >= 0)
    {
        for (const Region& r : bank_.Regions(static_cast<std::size_t>(t.instrument)))
        {
            frames = std::max<double>(frames, r.times[3]);
        }
    }

    return frames / kFramesPerSecond;
}

uint32_t Performer::PresetOf(int instrument)
{
    const auto [found, inserted] = presets_.emplace(instrument, static_cast<uint32_t>(out_.instruments.size()));
    if (inserted)
    {
        out_.instruments.push_back(instrument);
    }

    return found->second;
}

void Performer::SendControllers(std::size_t index)
{
    const Track& t = tracks_[index];
    TrackOutput& out = outputs_[channels_[index]];

    // CC11 holds the square root of the track's levels and the total of the pan's two sides.
    const double total = std::hypot(PanLevel(t.pan), PanLevel(kHighestPan - t.pan));
    const double level = (piece_.volume + 1) / kLevelScale * LevelOf(t.volume) * total;
    recorder_.SendSetting(out_.performance, out, tick_, EventKind::kVolume, 127);
    recorder_.SendSetting(out_.performance, out, tick_, EventKind::kExpression,
                          static_cast<uint8_t>(std::clamp(std::lround(127.0 * std::sqrt(level)), 0L, 127L)));
    recorder_.SendSetting(out_.performance, out, tick_, EventKind::kPan,
                          static_cast<uint8_t>(std::lround(1.0 + SinePlace(t.pan) * 126.0)));
    recorder_.SendSetting(out_.performance, out, tick_, EventKind::kReverb,
                          static_cast<uint8_t>(t.reverb_on ? std::min(t.reverb, 127) : 0));
    recorder_.SendPitch(out_.performance, out, tick_, static_cast<float>(t.bend * t.bend_range) / 8192.0f);
}

void Performer::KeyOn(Track& t)
{
    t.pending.push_back({t.key, t.velocity});
}

void Performer::StartNotes(std::size_t index)
{
    Track& t = tracks_[index];
    TrackOutput& out = outputs_[channels_[index]];
    for (const Pending& p : t.pending)
    {
        if (t.instrument < 0 || bank_.Regions(static_cast<std::size_t>(t.instrument)).empty())
        {
            continue;
        }

        // The driver leaves out the velocity's factor at a velocity of 0.
        const double factor = p.velocity == 0 ? 1.0 : (p.velocity + 1) / kLevelScale;
        const auto velocity = static_cast<uint8_t>(std::clamp(std::lround(127.0 * std::sqrt(factor)), 1L, 127L));
        SendControllers(index);
        recorder_.SendPreset(out_.performance, out, tick_, PresetOf(t.instrument));
        Held held;
        held.key = p.key;
        held.note = recorder_.NoteOn(out_.performance, out, tick_, p.key, velocity);
        held.notes_index = notes_[index].size();
        notes_[index].push_back({seconds_, seconds_ + kLimitSeconds, p.key, SettingsOf(t)});
        t.held.push_back(held);
    }
    t.pending.clear();
}

// A key-off cancels the track's key-ons of the key that wait, and releases each of its notes of the key (KeyOffSet).
void Performer::KeyOff(std::size_t index)
{
    Track& t = tracks_[index];
    const TrackOutput& out = outputs_[channels_[index]];
    std::erase_if(t.pending, [&t](const Pending& p) { return p.key == t.key; });

    const double tail = TailSeconds(t);
    const auto release = [&](const Held& h)
    {
        if (h.key != t.key)
        {
            return false;
        }

        recorder_.NoteOff(out_.performance, out.midi_track, tick_, out.part, h.key, h.note);
        notes_[index][h.notes_index].end = seconds_ + tail;

        return true;
    };
    std::erase_if(t.held, release);
}

void Performer::Command(std::size_t index, uint8_t command)
{
    Track& t = tracks_[index];
    switch (command)
    {
    case 0x00: // __MidiCtrl_Stop releases only the note of the track's key
        KeyOff(index);
        t.finished = true;
        active_--;
        break;

    case 0x01: // __MidiCtrl_Sleep does something only while one piece fades into another
    case 0x7f: // __MidiCtrl_Pass
        break;

    case 0x02: // __MidiCtrl_WholeLoopStart
        loop_starts_ = true;
        break;

    case 0x03: // __MidiCtrl_WholeLoopEnd
        loop_ends_ = true;
        break;

    case 0x08: // __MidiCtrl_TempoDirect
        tempo_ = std::max<int>(Byte(t), 1);
        break;

    case 0x0a: // __MidiCtrl_ReverbDepthDirect
        t.reverb = Byte(t);
        break;

    case 0x0c: // __MidiCtrl_TimeSignature
        t.at += 2;
        break;

    case 0x0d: // __MidiCtrl_KeySignature does nothing while the driver's transposition by key is off, as it stays here
        t.at += 1;
        break;

    case 0x10: // __MidiCtrl_KeyOnSame
        KeyOn(t);
        break;

    case 0x11: // __MidiCtrl_KeyOnNoteVelocity
        t.key = Byte(t);
        t.velocity = Byte(t);
        KeyOn(t);
        break;

    case 0x12: // __MidiCtrl_KeyOnNote
        t.key = Byte(t);
        KeyOn(t);
        break;

    case 0x13: // __MidiCtrl_KeyOnVelocity
        t.velocity = Byte(t);
        KeyOn(t);
        break;

    case 0x18: // __MidiCtrl_KeyOffSame
        KeyOff(index);
        break;

    case 0x1a: // __MidiCtrl_KeyOffNote
        t.key = Byte(t);
        KeyOff(index);
        break;

    case 0x20: // __MidiCtrl_Wave
        t.instrument = Byte(t);
        break;

    case 0x22: // __MidiCtrl_VolumeDirect
        t.volume = Byte(t);
        break;

    case 0x26: // __MidiCtrl_PanDirect
        t.pan = std::min<int>(Byte(t), kHighestPan);
        break;

    case 0x4d: // __MidiCtrl_TremoloRateChange does nothing while the track's tremolo is off, as it stays here
        t.at += 2;
        break;

    case 0x5c: // __MidiCtrl_PitchBend
        {
            const int low = Byte(t);
            const int high = Byte(t);
            t.bend = low + (high << 7) - 0x2000;
            break;
        }

    case 0x5d: // __MidiCtrl_PitchBendRange
        t.bend_range = Byte(t);
        break;

    case 0x60: // __MidiCtrl_ReverbOn
        t.reverb_on = true;
        break;

    default:
        throw FormatError("a track holds a command that dolphinsf2 doesn't read");
    }
}

// A tick of a track (_MidiTrackExecute). Each command follows a delta time.
void Performer::TrackTick(std::size_t index)
{
    Track& t = tracks_[index];
    if (t.finished)
    {
        return;
    }

    t.wait--;
    while (!t.finished && t.wait < 1)
    {
        Command(index, Byte(t));
        if (!t.finished)
        {
            t.wait += static_cast<int>(Delta(t));
        }
    }
}

void Performer::SaveLoop()
{
    loop_start_ = tick_;
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        const Track& t = tracks_[i];
        saved_[i] = {t.at, t.wait, t.finished, t.key, t.velocity};
    }
}

void Performer::RestoreLoop()
{
    active_ = 0;
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        Track& t = tracks_[i];
        const Saved& s = saved_[i];
        t.at = s.at;
        t.wait = s.wait;
        t.finished = s.finished;
        t.key = s.key;
        t.velocity = s.velocity;
        active_ += t.finished ? 0 : 1;
    }
}

Played Performer::Run()
{
    tracks_.resize(piece_.tracks.size());
    notes_.resize(piece_.tracks.size());
    saved_.resize(piece_.tracks.size());
    active_ = piece_.tracks.size();
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        Track& t = tracks_[i];
        t.data = piece_.tracks[i];
        t.wait = static_cast<int>(Delta(t)) + 1;
    }

    // The driver saves the tracks at the end of the tick of the loop's start. At the end of the tick of the loop's end,
    // it restores them. The next tick then plays what followed the loop's start. The loop therefore runs from the tick
    // after its start to the tick after its end.
    std::optional<seqlib::midi::Performance::Loop> loop;
    uint32_t passes = 0;
    double bpm = 0.0;
    bool done = false;
    while (!done && active_ > 0 && seconds_ < kLimitSeconds)
    {
        for (std::size_t i = 0; i < tracks_.size(); i++)
        {
            TrackTick(i);
        }

        // The tick's tempo goes first, then the notes. A track's controllers go out while its notes sound and at its
        // next note. A track with a channel to itself sends them at once.
        const double now = tempo_ * kFramesPerSecond * 60.0 / kTempoPerTick / kTicksPerBeat;
        if (now != bpm)
        {
            recorder_.SetTempo(out_.performance, tick_, now);
            bpm = now;
        }
        for (std::size_t i = 0; i < tracks_.size(); i++)
        {
            StartNotes(i);
            if (!shared_[i] || !tracks_[i].held.empty())
            {
                SendControllers(i);
            }
        }

        if (loop_starts_)
        {
            loop_starts_ = false;
            SaveLoop();
        }

        if (loop_ends_)
        {
            loop_ends_ = false;
            if (!loop_start_)
            {
                throw FormatError("a piece's loop ends before it starts");
            }

            if (!loop)
            {
                loop = seqlib::midi::Performance::Loop{*loop_start_ + 1, tick_ + 1};
            }

            passes++;
            done = passes >= static_cast<uint32_t>(std::max(options_.loops, 1));
            if (!done)
            {
                RestoreLoop();
            }
        }

        seconds_ += kTempoPerTick / (tempo_ * kFramesPerSecond);
        tick_++;
    }

    // Every note still sounding ends at the last tick.
    double tail = 0.0;
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        Track& t = tracks_[i];
        tail = std::max(tail, TailSeconds(t));
        while (!t.held.empty())
        {
            t.key = t.held.front().key;
            KeyOff(i);
        }
    }

    Performance& perf = out_.performance;
    perf.end_tick = tick_;

    if (loop)
    {
        perf.loop = *loop;
        perf.loops = options_.loops;
    }
    else if (seconds_ >= kLimitSeconds)
    {
        perf.truncated = true;
    }

    perf.seconds = seconds_ + tail;

    for (const auto& [what, count] : approximations_)
    {
        perf.approximations[what] += count;
    }
    perf.approximations.insert(recorder_.Approximations().begin(), recorder_.Approximations().end());

    return std::move(out_);
}

// The region's envelope. It starts at its first level and takes the first of the three stages with a time. A stage
// without a time then jumps to its level (_AdsrStart, _AdsrDataCompute). The release falls in a line in amplitude to
// silence in its time, from whatever level it starts at.
FadeShape ShapeOf(const Region& r)
{
    FadeCurve curve = {{0.0, LevelOf(r.levels[0])}};
    double time = 0.0;
    bool started = false;

    for (std::size_t i = 0; i < 3; i++)
    {
        const double level = LevelOf(r.levels[i + 1]);
        if (r.times[i] != 0)
        {
            started = true;
            time += r.times[i] * 1000.0 / kFramesPerSecond;
            curve.push_back({time, level});
        }
        else if (started)
        {
            curve.push_back({time, level});
        }
    }

    if (!started)
    {
        curve = {{0.0, LevelOf(r.levels[3])}};
    }

    FadeShape shape = FitFades(curve);
    shape.release_db_per_ms = FitRelease(r.times[3] * 1000.0 / kFramesPerSecond);

    return shape;
}

// The region's pitch in cents.
int CentsOf(const Region& r)
{
    return static_cast<int>(std::lround(r.pitch * 100.0 / 256.0));
}

std::string SampleName(const Bank& bank, const Region& r)
{
    return "w" + std::to_string(bank.Number()) + "_" + std::to_string(r.sample);
}

sf2::Zone SoundFontZone(const Region& r, const SoundFontSamples::Sample& sample)
{
    const FadeShape shape = ShapeOf(r);
    const int cents = CentsOf(r);

    sf2::ZoneValues values;
    values.keys = {r.low, r.high};
    values.attenuation = -200.0 * std::log10(std::max(shape.peak * LevelOf(r.volume), 1e-9));
    if (r.pan >= 0)
    {
        values.pan = static_cast<int>(std::lround((SinePlace(r.pan) * 2.0 - 1.0) * 500.0));
    }

    SetEnvelope(shape, values);
    values.root_key = kRootKey;
    values.coarse_tune = cents / 100;
    values.fine_tune = cents % 100;
    values.loop = sample.loop;
    values.sample = sample.id;
    sf2::Zone zone = sf2::RegionZone(values);

    // A fixed pan leaves out the track's. A modulator of this kind in a zone replaces the global zone's.
    if (r.pan >= 0)
    {
        sf2::Modulator fixed = sf2::kCc10ToPan;
        fixed.amount = 0;
        zone.modulators.push_back(fixed);
    }

    return zone;
}

std::string SfzRegion(const Region& r, const SfzSamples::Wave& wave)
{
    const FadeShape shape = ShapeOf(r);
    std::string s = "<region> sample=" + wave.name + " lokey=" + std::to_string(r.low) +
                    " hikey=" + std::to_string(r.high) + " pitch_keycenter=" + std::to_string(kRootKey);
    s += seqlib::sfz::TuneOpcodes(CentsOf(r));

    const double volume =
        std::max(20.0 * std::log10(std::max(shape.peak * LevelOf(r.volume), 1e-9)), kSfzLowestVolumeDb);
    if (std::abs(volume) >= 0.005)
    {
        s += " volume=" + Number(volume, 2);
    }

    if (r.pan >= 0)
    {
        s += " pan=" + Number((SinePlace(r.pan) * 2.0 - 1.0) * 100.0, 1) + " pan_oncc10=0";
    }

    s += seqlib::sfz::AmpEnvelopeOpcodes(SfzEnvelope(shape));
    s += seqlib::sfz::LoopOpcodes(wave.loop, wave.loop_start, wave.loop_end);

    return s + "\n";
}

Conversion ConvertPiece(const std::string& name, const PieceFile& piece, Bank& bank, const PerformOptions& options)
{
    // Each track takes a channel. A piece with more tracks runs once more, to find what they play.
    ChannelChoice channels;
    if (piece.tracks.size() > kChannels)
    {
        ChannelChoice one;
        one.channels.assign(piece.tracks.size(), 0);
        Performer first(piece, bank, options, one);
        first.Run();
        channels = ChannelsOf(first.Notes());
    }
    else
    {
        for (std::size_t t = 0; t < piece.tracks.size(); t++)
        {
            channels.channels.push_back(static_cast<uint8_t>(t));
        }
    }
    Performer performer(piece, bank, options, channels);
    Played played = performer.Run();

    Conversion c;
    c.performance = std::move(played.performance);
    const MidiReport midi = ntdlib::sequence_export::WriteMidiFile(c, name);
    std::vector<std::string> names;
    for (const int instrument : played.instruments)
    {
        names.push_back("Bank " + std::to_string(bank.Number()) + " instrument " + std::to_string(instrument));
    }

    sf2::SoundFont sf;
    SoundFontSamples samples(sf);
    std::vector<std::string> warnings;

    for (std::size_t i = 0; i < played.instruments.size(); i++)
    {
        sf2::Instrument inst;
        inst.name = names[i];
        sf2::Zone global;
        global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
        inst.zones.push_back(std::move(global));
        for (const Region& r : bank.Regions(static_cast<std::size_t>(played.instruments[i])))
        {
            const std::optional<SoundFontSamples::Sample> added =
                samples.Get(SampleName(bank, r), kRootKey, [&bank, &r] { return bank.Wave(r); });
            if (added)
            {
                inst.zones.push_back(SoundFontZone(r, *added));
            }
        }
        sf2::AddInstrumentPreset(sf, static_cast<uint32_t>(i), names[i], std::move(inst));
    }

    for (const uint32_t preset : midi.channel_10_presets)
    {
        sf2::AddChannel10Copy(sf, preset, warnings);
    }

    const std::string comment = "Converted by " + std::string(kProgram) + " from " + name + ".";
    ntdlib::sequence_export::WriteSoundFont(c, sf, name, comment, kProgram);

    // The SFZ files: each track's presets, with the samples that they share.
    seqlib::sfz::TrackFileOptions sfz;
    sfz.title = name;
    sfz.program = kProgram;
    sfz.preset_names = names;
    sfz.presets = played.instruments.size();
    sfz.level_cut_db = c.performance.level_cut_db;
    sfz.track_names = true;
    SfzSamples waves;
    const auto text = [&](int)
    {
        seqlib::sfz::TrackText added;
        added.preset = [&](uint32_t p)
        {
            std::string s = "<group>\n";
            for (const Region& r : bank.Regions(static_cast<std::size_t>(played.instruments[p])))
            {
                const SfzSamples::Wave& wave = waves.Get(SampleName(bank, r), [&bank, &r] { return bank.Wave(r); });
                if (!wave.name.empty())
                {
                    s += SfzRegion(r, wave);
                }
            }

            return s;
        };

        return added;
    };
    c.sfz.files = seqlib::sfz::TrackFiles(c.performance, midi, sfz, text);
    c.sfz.samples = waves.Take();

    ntdlib::sequence_export::CollectWarnings(c, midi, warnings, {});

    return c;
}

std::string Lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return out;
}

} // namespace

GameInput::GameInput(std::shared_ptr<ntdlib::disc::GameFiles> disc, std::vector<std::string> names,
                     std::vector<std::string> paths)
    : disc_(std::move(disc)), names_(std::move(names)), paths_(std::move(paths))
{
}

std::unique_ptr<GameInput> GameInput::Open(const std::shared_ptr<ntdlib::disc::GameFiles>& disc)
{
    std::map<std::string, std::string> found;
    for (const ntdlib::disc::GameFile& file : disc->files)
    {
        const std::string lower = Lower(file.path);
        if (lower.starts_with(kMusicFolder) && lower.ends_with(".bgm"))
        {
            found.emplace(lower.substr(kMusicFolder.size(), lower.size() - kMusicFolder.size() - 4), file.path);
        }
    }
    if (found.empty())
    {
        return nullptr;
    }

    std::vector<std::string> names;
    std::vector<std::string> paths;
    for (auto& [name, path] : found)
    {
        names.push_back(name);
        paths.push_back(std::move(path));
    }

    return std::unique_ptr<GameInput>(new GameInput(disc, std::move(names), std::move(paths)));
}

Conversion GameInput::Convert(std::size_t index, const PerformOptions& perform)
{
    const auto read = [this](const std::string& path)
    {
        std::vector<uint8_t> data;
        try
        {
            if (!disc_->read_file(path, data))
            {
                throw FormatError("can't read " + path);
            }
        }
        catch (const ntdlib::disc::Error& e)
        {
            throw FormatError(e.what());
        }

        return data;
    };

    const std::vector<uint8_t> file = read(paths_[index]);
    const PieceFile piece = ReadPiece(file);

    // The piece's bank, in the wave folder. The number in the file's name has four digits.
    std::string digits = std::to_string(piece.bank);
    digits.insert(0, 4 - std::min<std::size_t>(digits.size(), 4), '0');
    const std::string wanted = std::string(kWaveFolder) + "wave" + digits + ".wd";
    for (const ntdlib::disc::GameFile& f : disc_->files)
    {
        if (Lower(f.path) == wanted)
        {
            Bank bank(piece.bank, read(f.path));

            return ConvertPiece(names_[index], piece, bank, perform);
        }
    }

    throw FormatError("no file on the disc holds the piece's bank");
}

} // namespace dolphinsf2::red
