// SPDX-License-Identifier: MIT

// MusyX's sequencer (see musyx_player.h).
//
// The sequencer runs once a millisecond of its clock, five times in each frame of 160 samples. In each run, every
// section of the song applies the tempo changes that are due, handles the events that are due, then releases the notes
// that are due, and its clock moves on by the tempo's ticks. Each track has one event waiting in its section's list at
// a time: its pattern's next event, the next point of the pattern's pitch-wheel or mod-wheel stream, its next entry or
// its loop. The list holds the events in order of time. Of two at the same time, the one that came first goes first. A
// track's next entry comes after its pattern's last event, at the entry's time. That time can have passed: the
// entry's events then come at once. A section loops when its list empties in a run that handled a loop: its clock goes
// back to its loop start, and the tracks that loop start again from their loop entries. The notes that started before
// keep the old clock and end by it. The old clock runs on beside the new one.
//
// The performance's ticks run on from pass to pass. A pass of the main loop starts where a section's clock went back.
//
// MusyX multiplies a note's velocity, its channel's volume and expression, the song's volume and the game's master
// volume. MusyX plays the product on the curve of the note's voice. SoundFont's and SFZ's default curves square the
// velocity, CC7 and CC11 each. The DLS curve squares the product too. A note on the DLS curve keeps its velocity. CC7
// takes the channel's volume times the song's volume and the master volume. In a channel whose notes all play on
// MusyX's volume table, CC7 and CC11 take the values whose squares follow the table. The velocity of a note on the
// table then gives the note the table's level of the product of its velocity and the volumes that CC7 and CC11 stand
// for, at the note's start. A channel whose notes play on both curves keeps the DLS curve's CC7 and CC11. Only the
// velocities of the channel's notes on the table follow the table.

#include "musyx_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/sequence_export/passes.h"
#include "seqlib/midi/performance.h"
#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"

#include "jaudio.h"
#include "musyx_game.h"
#include "musyx_voices.h"

namespace dolphinsf2::musyx
{
namespace
{

using ntdlib::musyx::ChannelSetup;
using ntdlib::musyx::MidiSetup;
using ntdlib::musyx::Page;
using ntdlib::musyx::Pattern;
using ntdlib::musyx::PatternEvent;
using ntdlib::musyx::PatternEventKind;
using ntdlib::musyx::TrackEntry;
using xsflib::FormatError;

// The longest performance. JAudio2's sequences have the same limit.
constexpr double kMaxSeconds = 900.0;

// A run of the sequencer: a millisecond of MusyX's clock. The output's rate makes it a little shorter.
constexpr double kRunSeconds = 0.001 * kNominalRate / kOutputRate;

// The channel that plays the drum pages, and the channels in all.
constexpr uint8_t kDrumChannel = 9;
constexpr std::size_t kChannels = 16;
constexpr std::size_t kSections = 16;

// A time that never comes, for a stream with no more points.
constexpr uint32_t kNoTime = 0x7fffffff;

// The pitch wheel's centre and MusyX's default range in semitones each way. A voice's macro can set another range.
constexpr int kWheelCentre = 0x2000;
constexpr int kBendRange = 2;

// The passes of the main loop that the performance tries to mark, beyond the first.
constexpr std::size_t kMorePasses = 2;

// MusyX's controllers that go as controller events but aren't MIDI's: the synchronised crossfade, the channel's
// priority, MusyX's controller 130, reset all controllers and all notes off.
constexpr uint8_t kCrossfade = 0x68;
constexpr uint8_t kPriority = 0x69;
constexpr uint8_t kPriorityHigh = 0x6a;
constexpr uint8_t kController130 = 0x82;
constexpr uint8_t kResetControllers = 0x79;
constexpr uint8_t kAllNotesOff = 0x7b;

// The report's names for what the volume curves approximate.
constexpr char kBothCurves[] = "notes whose voices play on both volume curves follow the curve of their first voice";
constexpr char kScaledAttacks[] = "velocity scales of attacks on MusyX's volume table take the MIDI velocity";
constexpr char kMixedChannels[] =
    "notes on MusyX's volume table follow volume changes on the DLS curve in channels with both curves";

// What a track waits for.
enum class EventType
{
    kPattern,
    kPitchWheel,
    kModWheel,
    kEntry,
    kLoop,
};

struct Event
{
    uint32_t time = 0;
    EventType type = EventType::kPattern;
    std::size_t track = 0; // in the song's tracks
    std::size_t index = 0; // the entry, the pattern's event or the stream's point
};

// A track's place: its next entry, and the pattern that it plays, with its next event and stream points.
struct TrackState
{
    std::size_t next_entry = 0;
    bool ended = false;
    const Pattern* pattern = nullptr;
    const TrackEntry* entry = nullptr;
    uint32_t base = 0;
    std::size_t event = 0;
    std::size_t pitch = 0;
    std::size_t mod = 0;
};

// A section's clocks: two, in 65536ths of a tick, of which one counts the notes that start. Each has the tempo's ticks
// a run, and the performance's tick of its tick 0.
struct SectionState
{
    std::array<uint64_t, 2> time{};
    std::array<uint64_t, 2> delta{};
    std::array<int64_t, 2> offset{};
    int clock = 0;
    uint32_t bpm = 0; // in 1024ths of a beat a minute
    std::size_t tempo_at = 0;
    std::vector<Event> pending;
    bool has_tracks = false;
    int64_t last_run_tick = -1; // the clock's tick in the run before, or before the clock's start
};

// A note that sounds: when its section's clock releases it, and what it plays.
struct ActiveNote
{
    uint32_t end = 0;
    std::size_t section = 0;
    int clock = 0;
    int64_t offset = 0;
    std::size_t record = 0;
};

// A note as the performance records it.
struct NoteRecord
{
    uint8_t channel = 0;
    uint8_t key = 0;
    uint32_t id = 0;
    int64_t on = 0;
    std::optional<int64_t> off;
    std::vector<KeyVoice> voices;
    std::pair<int, int> bend_range{kBendRange, kBendRange}; // the first voice's pitch wheel ranges up and down
    bool other_range = false;                               // the wheel has bent the note over another note's range
    bool sounds = false;                                    // a voice plays a sample
    bool on_table = false;                                  // the first such voice plays on MusyX's volume table
};

// A channel's CC7 and CC11 in the MIDI file.
struct ChannelLevels
{
    int volume = 127;
    int expression = 127;
};

// The MIDI value from 0 to 127 whose square is `amplitude`. SoundFont's and SFZ's default curves square the
// velocity, CC7 and CC11.
int SquaredValue(double amplitude)
{
    return static_cast<int>(std::lround(127.0 * std::sqrt(std::clamp(amplitude, 0.0, 1.0))));
}

// Where an event goes among a channel's events at one tick: note-offs come first, then settings, then note-ons.
enum class Rank
{
    kNoteOff,
    kSetting,
    kNoteOn,
};

// The rank of an event of `kind`.
Rank RankOf(EventKind kind)
{
    if (kind == EventKind::kNoteOff)
    {
        return Rank::kNoteOff;
    }

    return kind == EventKind::kNoteOn ? Rank::kNoteOn : Rank::kSetting;
}

// An event of a channel, before the events are put in order.
struct ChannelEvent
{
    int64_t tick = 0;
    Rank rank = Rank::kNoteOff;
    uint64_t order = 0;
    TrackEvent event;
};

// The page that each program plays: its index in `pages`, or 0xff if no page has the program. Of the pages that share a
// program, the last plays it.
std::array<uint8_t, 128> PagesByProgram(const std::vector<Page>& pages)
{
    std::array<uint8_t, 128> index;
    index.fill(0xff);
    for (std::size_t i = 0; i < pages.size() && i < 0xff; i++)
    {
        index[pages[i].program & 0x7f] = static_cast<uint8_t>(i);
    }

    return index;
}

// The seconds that `ticks` ticks of the performance last at `bpm` beats a minute.
double TicksToSeconds(int64_t ticks, double bpm)
{
    return static_cast<double>(ticks) * 60.0 / (bpm * ntdlib::musyx::kTicksPerBeat);
}

// The ticks of the performance in `seconds` at `bpm` beats a minute.
double SecondsToTicks(double seconds, double bpm)
{
    return seconds * bpm * ntdlib::musyx::kTicksPerBeat / 60.0;
}

class Player
{
public:
    Player(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments);

    // Run until the song ends, the main loop has played `passes` passes, or the time limit.
    void Run(std::size_t passes);

    // The performance: the recorded events, the presets and the tempo, with the main loop marked.
    Performance Finish(const PerformOptions& options);

private:
    // The section that plays track `track`. A song without sections plays every track in section 0.
    std::size_t SectionOf(std::size_t track) const;

    // The tick that the loop of section `section` goes back to.
    uint32_t LoopStart(std::size_t section) const;

    // Give each channel the program, volume, pan and sends of the song group's MIDI setup for song `song_id`. Without
    // such a setup, the channels keep MusyX's defaults and play nothing until a program change.
    void SetUpChannels(uint16_t song_id);

    // One run of the sequencer. Return false once the song has ended.
    bool Step();

    void ApplyTempos(SectionState& s, bool record);

    // Set the ticks that a run adds to the clock of `s` at its tempo, as the game's build of MusyX works them out.
    void SetTickDelta(SectionState& s) const;

    // Handle the section's events that are due. Return false if the section has none left.
    bool HandleEvents(std::size_t section);

    // Send the clock of section `section` back to its loop start, for the loop at `loop_time` of the clock. The tracks
    // that loop start again.
    void StartLoop(std::size_t section, uint32_t loop_time);

    // Handle event `e` of section `section`, and put its track's next event in the section's list. For a track's loop,
    // return the loop's time instead. The section loops once its list is empty. StartLoop then puts the tracks' next
    // events in it.
    std::optional<uint32_t> HandleEvent(const Event& e, std::size_t section);

    std::optional<Event> NextEvent(std::size_t track);
    static void Insert(SectionState& s, const Event& e);
    bool ReleaseNotes();

    // The performance's tick for an event of section `section` at `time` of its clock.
    int64_t TickOf(std::size_t section, uint32_t time) const;

    void ProgramChange(uint8_t channel, uint8_t program, int64_t tick);

    // The semitones by which pitch-wheel value `value` bends the notes of `channel` over the channel's range.
    float Semitones(uint8_t channel, int value) const;

    // Give `channel` the pitch wheel's range `range` of a note that starts at `tick`. MusyX bends each voice over the
    // voice's range. A MIDI channel bends its notes over one range: its latest note's.
    void SetBendRange(uint8_t channel, std::pair<int, int> range, int64_t tick);

    // Count the held notes of `channel` that the wheel now bends over another note's range. Each note counts once.
    void CountOtherRanges(uint8_t channel);

    void Controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t tick);
    void NoteOn(uint8_t channel, uint8_t key, uint8_t velocity, uint32_t end, std::size_t section, int64_t tick);

    // Add `e` to the events of `channel` at `tick`. The event's kind gives its rank among the tick's events.
    void Record(uint8_t channel, int64_t tick, TrackEvent e);

    // Find the volume curve that note `n` follows, from its voices. Count what that approximates.
    void SetCurve(NoteRecord& n);

    // Pick the curve of each channel's CC7 and CC11: MusyX's volume table for a channel whose sounding notes all play
    // on it, else the DLS curve.
    void ChooseCurves();

    // The product of the channel's volume `value`, the song's volume and the master volume.
    double ChannelVolume(int value) const;

    // The MIDI value of `volume` on channel `channel`'s curve, and the volume that MIDI value `value` stands for.
    int WrittenValue(std::size_t channel, double volume) const;
    double WrittenVolume(std::size_t channel, int value) const;

    // Map `e`, an event of channel `channel`, to the voices' curves: its CC7 and CC11, and the velocity of a note on
    // MusyX's volume table. `levels` holds the channel's CC7 and CC11 before `e`. Keep them up to date.
    void MapLevel(std::size_t channel, TrackEvent& e, ChannelLevels& levels) const;

    void SetTempo(int64_t tick, const SectionState& s);

    // The seconds at tick `tick` of the performance, and the tick at `seconds`, from the tempo changes so far.
    double SecondsAt(int64_t tick) const;
    int64_t TickAt(double seconds) const;

    // Record each note's note-off where its voices end, and count its voices' approximations in `perf`. A note still
    // held at the end ends at `last_tick`. Return the seconds where the last sound ends, its release included.
    double EndNotes(Performance& perf, int64_t last_tick);

    // The seconds where voice `v` ends, for a note from `on_seconds` to `off_seconds`. `key` is the key that the voice
    // starts with. A voice that ignores the release plays its sample to the end. A voice that stops by itself ends
    // then. Add to `approximated` what the end approximates.
    double VoiceEnd(const MacroVoice& v, int key, double on_seconds, double off_seconds,
                    std::set<std::string>& approximated);

    // Put each channel's events in `perf`, in order, and the tempo changes.
    void AddEvents(Performance& perf);

    // Mark the main loop of `perf`: the first pass that plays as the next does, played `options.loops` times. Cut the
    // performance after the last pass that it plays.
    void MarkLoop(Performance& perf, const PerformOptions& options) const;

    // End `perf`, the performance of a song that doesn't loop, after its last event and after its last sound, which
    // ends at `sound_end` seconds. A run cut at the time limit ends at `last_tick`.
    void EndAfterLastSound(Performance& perf, int64_t last_tick, double sound_end) const;

    const ntdlib::musyx::Song& song_;
    const SongGroup& group_;
    Instruments& instruments_;
    uint8_t song_volume_ = 127;   // the volume that the game's song table gives the song
    uint8_t master_volume_ = 127; // the game's master volume for music
    TickStep tick_step_ = TickStep::kFloat;
    std::array<uint8_t, 128> page_of_program_{};
    std::array<uint8_t, 128> drum_of_program_{};

    std::vector<TrackState> tracks_;
    std::array<SectionState, kSections> sections_{};
    std::size_t master_ = 0; // the section whose clock gives the tempo and the passes
    std::array<std::optional<uint16_t>, kChannels> channel_object_{}; // the object that the channel's page plays
    std::array<int, kChannels> wheel_{};                              // the channel's latest pitch-wheel value
    std::array<std::pair<int, int>, kChannels> bend_range_{};         // the channel's range up and down
    std::array<bool, kChannels> on_table_{}; // the channel's CC7 and CC11 follow MusyX's volume table (ChooseCurves)
    std::vector<ActiveNote> active_;
    std::vector<NoteRecord> notes_;
    std::array<std::vector<ChannelEvent>, kChannels> events_;
    uint64_t order_ = 0;
    uint32_t next_note_ = 1;
    std::vector<PresetKey> presets_;
    std::vector<std::pair<int64_t, double>> tempo_; // the performance's tick and the beats a minute
    std::vector<int64_t> jumps_;                    // the tick of each pass after the first
    uint64_t runs_ = 0;
    bool ended_ = false;
    bool truncated_ = false;
    std::map<std::string, uint32_t> approximations_;
};

Player::Player(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments)
    : song_(song),
      group_(instruments.Group()),
      instruments_(instruments),
      song_volume_(listed.volume),
      master_volume_(listed.master_volume),
      tick_step_(listed.tick_step),
      page_of_program_(PagesByProgram(group_.group.pages)),
      drum_of_program_(PagesByProgram(group_.group.drum_pages))
{
    wheel_.fill(kWheelCentre);
    bend_range_.fill({kBendRange, kBendRange});

    // The tempo starts at the song's, in 1024ths of a beat a minute.
    const uint32_t bpm = song.fine_tempo ? song.tempo : song.tempo << 10;
    for (SectionState& s : sections_)
    {
        s.bpm = bpm;
    }

    // Each track's first event waits in its section's list. The first track's section is the master.
    master_ = song.tracks.empty() ? 0 : SectionOf(0);
    tracks_.resize(song.tracks.size());
    for (std::size_t t = 0; t < song.tracks.size(); t++)
    {
        const std::size_t section = SectionOf(t);
        sections_[section].has_tracks = true;
        if (const std::optional<Event> e = NextEvent(t))
        {
            Insert(sections_[section], *e);
        }
    }

    SetUpChannels(listed.id);

    // The performance starts at the master's tempo, after the tempo table's changes at tick 0.
    ApplyTempos(sections_[master_], true);
    SetTempo(0, sections_[master_]);
}

std::size_t Player::SectionOf(std::size_t track) const
{
    return song_.sections ? song_.tracks[track].section : 0;
}

uint32_t Player::LoopStart(std::size_t section) const
{
    return song_.loop_starts[song_.sections ? section : 0];
}

void Player::SetUpChannels(uint16_t song_id)
{
    const std::vector<MidiSetup>& setups = group_.group.midi_setups;
    const auto is_song = [song_id](const MidiSetup& m)
    {
        return m.song == song_id;
    };
    const auto setup = std::find_if(setups.begin(), setups.end(), is_song);
    for (uint8_t c = 0; c < kChannels; c++)
    {
        const ChannelSetup channel = setup != setups.end() ? setup->channels[c] : ChannelSetup{};
        if (setup != setups.end())
        {
            ProgramChange(c, channel.program, 0);
        }
        Controller(c, 7, channel.volume, 0);
        Controller(c, 10, channel.pan, 0);
        Controller(c, 11, 127, 0);
        Controller(c, 91, channel.reverb, 0);
        Controller(c, 93, channel.chorus, 0);
    }
}

void Player::SetTickDelta(SectionState& s) const
{
    const auto clock = static_cast<std::size_t>(s.clock);

    // An early build adds the tempo's ticks in 65/65536 of a second, in whole 65536ths of a tick. It multiplies the
    // tempo in beats a minute by 3072 / 60, then by 65 / 8, and rounds down each time.
    if (tick_step_ == TickStep::kInteger)
    {
        const uint64_t bpm = s.bpm >> 10;
        s.delta[clock] = bpm * 3072 / 60 * 65 / 8;
        return;
    }

    // A run adds the tempo's ticks in a millisecond, in single precision, kept in 65536ths.
    const float ticks = static_cast<float>(s.bpm) * 256.0f * (1.0f / 40960000.0f);
    const auto low = static_cast<uint64_t>(std::fmod(ticks * 65536.0f, 65536.0f));
    const auto high = static_cast<uint64_t>(std::floor(ticks));
    s.delta[clock] = high << 16 | low;
}

void Player::ApplyTempos(SectionState& s, bool record)
{
    const auto clock = static_cast<std::size_t>(s.clock);
    const uint32_t now = static_cast<uint32_t>(s.time[clock] >> 16);
    bool changed = false;
    while (s.tempo_at < song_.tempo_changes.size() && song_.tempo_changes[s.tempo_at].tick <= now)
    {
        const ntdlib::musyx::TempoChange& t = song_.tempo_changes[s.tempo_at];
        s.bpm = song_.fine_tempo ? t.tempo : t.tempo << 10;
        s.tempo_at++;
        changed = true;
    }
    SetTickDelta(s);
    if (changed && record)
    {
        SetTempo(s.offset[clock] + now, s);
    }
}

void Player::SetTempo(int64_t tick, const SectionState& s)
{
    // The clock runs the tempo's ticks in each run, and kRunSeconds make a run.
    const double ticks_per_run = static_cast<double>(s.delta[static_cast<std::size_t>(s.clock)]) / 65536.0;
    const double bpm = ticks_per_run / kRunSeconds * 60.0 / ntdlib::musyx::kTicksPerBeat;
    if (!tempo_.empty() && tempo_.back().first == tick)
    {
        tempo_.back().second = bpm;
    }
    else if (tempo_.empty() || tempo_.back().second != bpm)
    {
        tempo_.emplace_back(tick, bpm);
    }
}

std::optional<Event> Player::NextEvent(std::size_t track)
{
    TrackState& t = tracks_[track];
    if (t.ended)
    {
        return std::nullopt;
    }

    if (t.pattern != nullptr)
    {
        const Pattern& p = *t.pattern;
        const uint32_t pitch_time = t.pitch < p.pitch_wheel.size() ? p.pitch_wheel[t.pitch].tick : kNoTime;
        const uint32_t mod_time = t.mod < p.mod_wheel.size() ? p.mod_wheel[t.mod].tick : kNoTime;
        const uint32_t pattern_time = t.event < p.events.size() ? p.events[t.event].tick : p.end_tick;

        // A stream's point comes before a pattern's event at the same time, and the mod wheel's before the pitch
        // wheel's. The points after the pattern's end don't play.
        if (pattern_time >= pitch_time || pattern_time >= mod_time)
        {
            if (pitch_time < mod_time)
            {
                return Event{t.base + pitch_time, EventType::kPitchWheel, track, t.pitch++};
            }
            return Event{t.base + mod_time, EventType::kModWheel, track, t.mod++};
        }
        if (t.event < p.events.size())
        {
            return Event{t.base + pattern_time, EventType::kPattern, track, t.event++};
        }
        t.pattern = nullptr;
    }

    const std::vector<TrackEntry>& entries = song_.tracks[track].entries;
    if (t.next_entry >= entries.size())
    {
        t.ended = true;
        return std::nullopt;
    }

    const TrackEntry& e = entries[t.next_entry];
    if (e.pattern == ntdlib::musyx::kEndOfTrack)
    {
        t.ended = true;
        return std::nullopt;
    }
    if (e.pattern == ntdlib::musyx::kLoopBack)
    {
        t.next_entry = e.loop_target;
        return Event{e.tick, EventType::kLoop, track, 0};
    }

    return Event{e.tick, EventType::kEntry, track, t.next_entry++};
}

void Player::Insert(SectionState& s, const Event& e)
{
    const auto later = std::upper_bound(s.pending.begin(), s.pending.end(), e.time,
                                        [](uint32_t time, const Event& other) { return time < other.time; });
    s.pending.insert(later, e);
}

int64_t Player::TickOf(std::size_t section, uint32_t time) const
{
    // An event whose time has passed comes at the clock's tick.
    const SectionState& s = sections_[section];
    const auto clock = static_cast<std::size_t>(s.clock);
    const auto now = static_cast<int64_t>(s.time[clock] >> 16);
    return s.offset[clock] + (time > s.last_run_tick ? static_cast<int64_t>(time) : now);
}

bool Player::HandleEvents(std::size_t section)
{
    SectionState& s = sections_[section];
    std::optional<uint32_t> loop_time; // the latest loop of a track since the section's clock last went back
    bool looped = false;
    while (true)
    {
        const uint32_t now = static_cast<uint32_t>(s.time[static_cast<std::size_t>(s.clock)] >> 16);
        if (!s.pending.empty() && s.pending.front().time > now)
        {
            return true;
        }
        if (s.pending.empty())
        {
            if (!loop_time)
            {
                return false;
            }

            // A loop whose events all come at its start would loop for ever in one run.
            if (looped)
            {
                throw FormatError("the song loops without time passing");
            }
            looped = true;
            StartLoop(section, *loop_time);
            loop_time.reset();
            continue;
        }

        const Event e = s.pending.front();
        s.pending.erase(s.pending.begin());
        if (const std::optional<uint32_t> loop_at = HandleEvent(e, section))
        {
            loop_time = std::max(loop_time.value_or(0), *loop_at);
        }
    }
}

void Player::StartLoop(std::size_t section, uint32_t loop_time)
{
    // The other clock starts at the loop start, with this clock's fraction of a tick. The tempo table starts again,
    // and its changes up to the loop start apply.
    SectionState& s = sections_[section];
    const auto clock = static_cast<std::size_t>(s.clock);
    const auto other = clock ^ 1;
    const uint32_t loop_start = LoopStart(section);
    s.offset[other] = s.offset[clock] + static_cast<int64_t>(loop_time) - loop_start;
    s.time[other] = static_cast<uint64_t>(loop_start) << 16 | (s.time[clock] & 0xffff);
    s.clock = static_cast<int>(other);
    s.last_run_tick = static_cast<int64_t>(loop_start) - 1;
    if (!song_.tempo_changes.empty())
    {
        s.tempo_at = 0;
    }
    ApplyTempos(s, false);
    if (section == master_)
    {
        SetTempo(s.offset[other] + loop_start, s);
        jumps_.push_back(s.offset[other] + loop_start);
    }

    // The section's tracks wait for their next events: those that loop, their loop entries.
    for (std::size_t t = 0; t < song_.tracks.size(); t++)
    {
        if (SectionOf(t) != section)
        {
            continue;
        }

        if (const std::optional<Event> e = NextEvent(t))
        {
            Insert(s, *e);
        }
    }
}

std::optional<uint32_t> Player::HandleEvent(const Event& e, std::size_t section)
{
    TrackState& t = tracks_[e.track];
    const uint8_t channel = song_.tracks[e.track].channel;
    const int64_t tick = TickOf(section, e.time);
    switch (e.type)
    {
    case EventType::kEntry:
        {
            const TrackEntry& entry = song_.tracks[e.track].entries[e.index];
            if (entry.pattern >= song_.patterns.size())
            {
                throw FormatError("song plays pattern " + std::to_string(entry.pattern) + " of " +
                                  std::to_string(song_.patterns.size()));
            }

            t.pattern = &song_.patterns[entry.pattern];
            t.entry = &entry;
            t.base = entry.tick;
            t.event = 0;
            t.pitch = 0;
            t.mod = 0;
            if (entry.program != 0xff)
            {
                ProgramChange(channel, entry.program, tick);
            }
            if (entry.volume != 0xff)
            {
                Controller(channel, 7, entry.volume, tick);
            }
            break;
        }

    case EventType::kPattern:
        {
            const PatternEvent& pe = t.pattern->events[e.index];
            if (pe.kind == PatternEventKind::kProgram)
            {
                ProgramChange(channel, pe.value, tick);
            }
            else if (pe.kind == PatternEventKind::kController)
            {
                Controller(channel, pe.controller, pe.value, tick);
            }
            else
            {
                const int key = std::clamp(pe.key + t.entry->transpose, 0, 127);
                const int velocity = std::clamp(pe.velocity + t.entry->velocity_add, 0, 127);
                NoteOn(channel, static_cast<uint8_t>(key), static_cast<uint8_t>(velocity), e.time + pe.length, section,
                       tick);
            }
            break;
        }

    case EventType::kPitchWheel:
        {
            const int value = std::clamp(t.pattern->pitch_wheel[e.index].value, 0, 0x3fff);
            wheel_[channel] = value;
            TrackEvent ev{.kind = EventKind::kPitch};
            ev.semitones = Semitones(channel, value);
            Record(channel, tick, ev);
            if (value != kWheelCentre)
            {
                CountOtherRanges(channel);
            }
            break;
        }

    case EventType::kModWheel:
        {
            const int value = std::clamp(t.pattern->mod_wheel[e.index].value, 0, 0x3fff);
            Controller(channel, 1, static_cast<uint8_t>(value >> 7), tick);
            break;
        }

    case EventType::kLoop:
        return e.time;
    }

    if (const std::optional<Event> next = NextEvent(e.track))
    {
        Insert(sections_[section], *next);
    }

    return std::nullopt;
}

void Player::ProgramChange(uint8_t channel, uint8_t program, int64_t tick)
{
    // A program without a page leaves the channel's program as it was.
    const bool drums = channel == kDrumChannel;
    const uint8_t index = drums ? drum_of_program_[program & 0x7f] : page_of_program_[program & 0x7f];
    if (index == 0xff)
    {
        return;
    }

    const Page& page = drums ? group_.group.drum_pages[index] : group_.group.pages[index];
    const PresetKey key{.bank = drums ? kDrumBank : kPageBank, .program = page.program};
    channel_object_[channel] = page.object;
    TrackEvent e{.kind = EventKind::kPreset};
    e.preset = seqlib::midi::PresetIndex(presets_, key);
    Record(channel, tick, e);
}

void Player::Controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t tick)
{
    TrackEvent e;
    e.value = value;
    switch (controller)
    {
    case 1:
        e.kind = EventKind::kModulation;
        break;

    // The channel's volume as the song sets it. AddEvents maps it to the voices' curve (MapLevel).
    case 7:
        e.kind = EventKind::kVolume;
        break;

    case 10:
        e.kind = EventKind::kPan;
        break;

    case 11:
        e.kind = EventKind::kExpression;
        break;

    case 91:
        e.kind = EventKind::kReverb;
        break;

    case 93:
        e.kind = EventKind::kChorus;
        break;

    // MusyX's commands. All notes off releases every note of the song.
    case kAllNotesOff:
        for (const ActiveNote& n : active_)
        {
            notes_[n.record].off = tick;
        }
        active_.clear();
        return;

    case kCrossfade:
    case kPriority:
    case kPriorityHigh:
    case kController130:
        return;

    default:
        if (controller >= 0x80)
        {
            return;
        }
        if (controller != kResetControllers)
        {
            approximations_["controllers that no MusyX voice reads sent as MIDI's"]++;
        }
        e.kind = seqlib::midi::ControlKind(controller);
        break;
    }

    Record(channel, tick, e);
}

void Player::NoteOn(uint8_t channel, uint8_t key, uint8_t velocity, uint32_t end, std::size_t section, int64_t tick)
{
    // A channel without a program plays no notes.
    if (!channel_object_[channel])
    {
        return;
    }

    // A velocity of 0 is a note-off in MIDI. MusyX plays the note silent.
    if (velocity == 0)
    {
        approximations_["notes of velocity 0 played at 1"]++;
        velocity = 1;
    }

    NoteRecord n;
    n.channel = channel;
    n.key = key;
    n.id = next_note_++;
    n.on = tick;
    n.voices = VoicesOf(group_.pool, *channel_object_[channel], key);

    // The channel bends over the range of the note's first voice.
    if (!n.voices.empty())
    {
        const MacroVoice& voice = instruments_.Macro(n.voices.front().macro);
        n.bend_range = {voice.bend_up, voice.bend_down};
        SetBendRange(channel, n.bend_range, tick);
    }

    SetCurve(n);

    TrackEvent e{.kind = EventKind::kNoteOn, .key = key, .value = velocity};
    e.note = n.id;
    Record(channel, tick, e);

    const SectionState& s = sections_[section];
    active_.push_back({end, section, s.clock, s.offset[static_cast<std::size_t>(s.clock)], notes_.size()});
    notes_.push_back(std::move(n));
}

float Player::Semitones(uint8_t channel, int value) const
{
    const auto [up, down] = bend_range_[channel];
    return static_cast<float>((value - kWheelCentre) * (value >= kWheelCentre ? up : down)) / kWheelCentre;
}

void Player::SetBendRange(uint8_t channel, std::pair<int, int> range, int64_t tick)
{
    if (range == bend_range_[channel])
    {
        return;
    }

    // A wheel off its centre bends by the new range from the note on.
    bend_range_[channel] = range;
    if (wheel_[channel] != kWheelCentre)
    {
        TrackEvent ev{.kind = EventKind::kPitch};
        ev.semitones = Semitones(channel, wheel_[channel]);
        Record(channel, tick, ev);
        CountOtherRanges(channel);
    }
}

void Player::CountOtherRanges(uint8_t channel)
{
    // In the game, a note that still sounds keeps its voice's range. In MIDI, it takes the channel's.
    for (const ActiveNote& a : active_)
    {
        NoteRecord& n = notes_[a.record];
        if (n.channel == channel && !n.voices.empty() && n.bend_range != bend_range_[channel] && !n.other_range)
        {
            n.other_range = true;
            approximations_["pitch wheel ranges that differ among a channel's notes take the latest note's"]++;
        }
    }
}

bool Player::ReleaseNotes()
{
    // Each note ends by the clock it started with.
    std::vector<ActiveNote> still;
    for (const ActiveNote& n : active_)
    {
        const SectionState& s = sections_[n.section];
        if (n.end <= s.time[static_cast<std::size_t>(n.clock)] >> 16)
        {
            NoteRecord& record = notes_[n.record];
            record.off = std::max(record.on, n.offset + n.end);
        }
        else
        {
            still.push_back(n);
        }
    }
    active_ = std::move(still);

    return !active_.empty();
}

void Player::Record(uint8_t channel, int64_t tick, TrackEvent e)
{
    events_[channel].push_back({tick, RankOf(e.kind), order_++, e});
}

void Player::SetCurve(NoteRecord& n)
{
    // The note follows the curve of its first voice that plays a sample.
    const MacroVoice* first = nullptr;
    for (const KeyVoice& kv : n.voices)
    {
        const MacroVoice& voice = instruments_.Macro(kv.macro);
        if (!voice.sample)
        {
            continue;
        }

        if (first == nullptr)
        {
            first = &voice;
        }
        else if (voice.dls_volume != first->dls_volume)
        {
            approximations_[kBothCurves]++;
            break;
        }
    }
    n.sounds = first != nullptr;
    n.on_table = first != nullptr && !first->dls_volume;

    // A note on the table gets another velocity. A SoundFont's attack that the velocity scales takes that one.
    const auto scales_attack = [this](const KeyVoice& kv)
    {
        const MacroVoice& voice = instruments_.Macro(kv.macro);
        return voice.envelope && voice.envelope->dls && voice.envelope->attack_tc_per_velocity != 0.0;
    };
    if (n.on_table && std::any_of(n.voices.begin(), n.voices.end(), scales_attack))
    {
        approximations_[kScaledAttacks]++;
    }
}

void Player::ChooseCurves()
{
    std::array<std::size_t, kChannels> table_notes{};
    std::array<std::size_t, kChannels> dls_notes{};
    for (const NoteRecord& n : notes_)
    {
        if (n.sounds)
        {
            (n.on_table ? table_notes : dls_notes)[n.channel]++;
        }
    }

    for (std::size_t c = 0; c < kChannels; c++)
    {
        on_table_[c] = table_notes[c] > 0 && dls_notes[c] == 0;
        if (table_notes[c] > 0 && dls_notes[c] > 0)
        {
            approximations_[kMixedChannels] += static_cast<uint32_t>(table_notes[c]);
        }
    }
}

double Player::ChannelVolume(int value) const
{
    return value / 127.0 * song_volume_ / 127.0 * master_volume_ / 127.0;
}

int Player::WrittenValue(std::size_t channel, double volume) const
{
    if (on_table_[channel])
    {
        return SquaredValue(TableLevel(instruments_.Volumes(), volume));
    }

    return static_cast<int>(std::lround(127.0 * std::min(volume, 1.0)));
}

double Player::WrittenVolume(std::size_t channel, int value) const
{
    const double amplitude = value / 127.0;
    if (on_table_[channel])
    {
        return TableVolume(instruments_.Volumes(), amplitude * amplitude);
    }

    return amplitude;
}

void Player::MapLevel(std::size_t channel, TrackEvent& e, ChannelLevels& levels) const
{
    if (e.kind == EventKind::kVolume)
    {
        levels.volume = WrittenValue(channel, ChannelVolume(e.value));
        e.value = static_cast<uint8_t>(levels.volume);
    }
    else if (e.kind == EventKind::kExpression)
    {
        levels.expression = WrittenValue(channel, e.value / 127.0);
        e.value = static_cast<uint8_t>(levels.expression);
    }
    else if (e.kind == seqlib::midi::ControlKind(kResetControllers))
    {
        // A synth's reset of all controllers sets CC11 to 127.
        levels.expression = 127;
    }
    else if (e.kind == EventKind::kNoteOn && notes_[e.note - 1].on_table)
    {
        // The velocity whose square times the squares of CC7 and CC11 is the table's level of the product. The
        // product takes the volumes that the written CC7 and CC11 stand for. Two passes that write the same CC7 and
        // CC11 therefore give a note the same velocity.
        const VolumeTable& table = instruments_.Volumes();
        const double volume =
            e.value / 127.0 * WrittenVolume(channel, levels.volume) * WrittenVolume(channel, levels.expression);
        const double written = levels.volume / 127.0 * levels.expression / 127.0;
        const int velocity = written > 0.0 ? SquaredValue(TableLevel(table, volume) / (written * written))
                                           : SquaredValue(TableLevel(table, e.value / 127.0));
        e.value = static_cast<uint8_t>(std::max(velocity, 1));
    }
}

bool Player::Step()
{
    bool events = false;
    for (std::size_t i = 0; i < (song_.sections ? kSections : 1); i++)
    {
        SectionState& s = sections_[i];
        if (!s.has_tracks)
        {
            continue;
        }

        ApplyTempos(s, i == master_);
        events = HandleEvents(i) || events;
    }
    const bool notes = ReleaseNotes();

    // Both clocks of each section move on, each by its tempo.
    for (SectionState& s : sections_)
    {
        s.last_run_tick = static_cast<int64_t>(s.time[static_cast<std::size_t>(s.clock)] >> 16);
        for (std::size_t c = 0; c < 2; c++)
        {
            s.time[c] += s.delta[c];
        }
    }
    runs_++;

    return events || notes;
}

void Player::Run(std::size_t passes)
{
    constexpr auto kMostRuns = static_cast<uint64_t>(kMaxSeconds / kRunSeconds);
    while (!ended_ && jumps_.size() < passes)
    {
        if (runs_ >= kMostRuns)
        {
            truncated_ = true;
            return;
        }
        ended_ = !Step();
    }
}

double Player::SecondsAt(int64_t tick) const
{
    double seconds = 0.0;
    int64_t at = 0;
    double bpm = tempo_.empty() ? 120.0 : tempo_.front().second;
    for (const auto& [t, b] : tempo_)
    {
        if (t >= tick)
        {
            break;
        }

        seconds += TicksToSeconds(t - at, bpm);
        at = t;
        bpm = b;
    }

    return seconds + TicksToSeconds(tick - at, bpm);
}

int64_t Player::TickAt(double seconds) const
{
    double at_seconds = 0.0;
    int64_t at = 0;
    double bpm = tempo_.empty() ? 120.0 : tempo_.front().second;
    for (const auto& [t, b] : tempo_)
    {
        const double next = at_seconds + TicksToSeconds(t - at, bpm);
        if (next >= seconds)
        {
            break;
        }

        at_seconds = next;
        at = t;
        bpm = b;
    }

    return at + static_cast<int64_t>(std::ceil(SecondsToTicks(seconds - at_seconds, bpm)));
}

Performance Player::Finish(const PerformOptions& options)
{
    ChooseCurves();

    Performance perf;
    perf.timebase = ntdlib::musyx::kTicksPerBeat;
    perf.presets = presets_;
    for (const auto& [what, n] : approximations_)
    {
        perf.approximations[what] += n;
    }

    const int64_t last_tick = TickAt(static_cast<double>(runs_) * kRunSeconds);
    const double sound_end = EndNotes(perf, last_tick);
    AddEvents(perf);
    if (!jumps_.empty())
    {
        MarkLoop(perf, options);
    }
    else
    {
        EndAfterLastSound(perf, last_tick, sound_end);
    }

    return perf;
}

// The seconds that voice `v` sounds after its end: its release, or the fade of a voice that stops.
double TailOf(const MacroVoice& v)
{
    if (v.key_off == KeyOff::kRelease && v.envelope)
    {
        return v.envelope->release_ms / 1000.0;
    }
    if (v.key_off == KeyOff::kCut || v.stop_ms)
    {
        return kCutMs / 1000.0;
    }

    return 0.0;
}

double Player::EndNotes(Performance& perf, int64_t last_tick)
{
    // Each note ends where its voices do.
    double sound_end = 0.0;
    for (NoteRecord& n : notes_)
    {
        const int64_t off = n.off.value_or(last_tick);
        const double on_seconds = SecondsAt(n.on);
        const double off_seconds = SecondsAt(off);
        std::optional<double> latest;
        std::optional<double> earliest;
        double tail = 0.0;
        std::set<std::string> approximated;
        for (const KeyVoice& kv : n.voices)
        {
            const MacroVoice& v = instruments_.Macro(kv.macro);
            approximated.insert(v.approximations.begin(), v.approximations.end());
            if (!v.sample)
            {
                continue;
            }

            const double end = VoiceEnd(v, kv.key, on_seconds, off_seconds, approximated);
            latest = std::max(latest.value_or(end), end);
            earliest = std::min(earliest.value_or(end), end);
            tail = std::max(tail, TailOf(v));
        }
        for (const std::string& a : approximated)
        {
            perf.approximations[a]++;
        }
        if (latest && earliest && *latest - *earliest > 0.001)
        {
            perf.approximations["layered voices that end at different times end together"]++;
        }

        const double end_seconds = latest.value_or(off_seconds);
        const int64_t end = std::max(n.on, end_seconds == off_seconds ? off : TickAt(end_seconds));
        TrackEvent e{.kind = EventKind::kNoteOff, .key = n.key};
        e.note = n.id;
        Record(n.channel, end, e);
        sound_end = std::max(sound_end, end_seconds + tail);
    }

    return sound_end;
}

double Player::VoiceEnd(const MacroVoice& v, int key, double on_seconds, double off_seconds,
                        std::set<std::string>& approximated)
{
    double end = off_seconds;
    if (v.key_off == KeyOff::kIgnored)
    {
        // The voice's key, moved by the macro, against the sample's.
        const Sample* sample = instruments_.GetSample(*v.sample);
        const int voice_key = v.fixed_key.value_or(key + v.key_shift);
        const double ratio =
            sample != nullptr ? std::pow(2.0, (voice_key - sample->key) / 12.0 + v.detune_cents / 1200.0) : 1.0;
        if (const std::optional<double> length = instruments_.SampleSeconds(*v.sample, ratio))
        {
            end = on_seconds + *length;
        }
        else
        {
            approximated.insert("voices that loop and ignore the release stop at their note's end");
        }
    }
    if (v.stop_ms)
    {
        end = std::min(end, on_seconds + *v.stop_ms / 1000.0);
    }

    return end;
}

void Player::AddEvents(Performance& perf)
{
    // The events of each channel, in order, mapped to the voices' curves, without settings that repeat the last value.
    for (std::size_t c = 0; c < kChannels; c++)
    {
        std::vector<ChannelEvent>& events = events_[c];
        const auto earlier = [](const ChannelEvent& a, const ChannelEvent& b)
        {
            return std::tie(a.tick, a.rank, a.order) < std::tie(b.tick, b.rank, b.order);
        };
        std::sort(events.begin(), events.end(), earlier);

        std::map<EventKind, int> last;
        float pitch = 0.0f;
        ChannelLevels levels;
        for (ChannelEvent& ce : events)
        {
            TrackEvent e = ce.event;
            e.tick = static_cast<uint32_t>(std::max<int64_t>(ce.tick, 0));
            MapLevel(c, e, levels);

            if (e.kind == EventKind::kPitch)
            {
                if (e.semitones == pitch)
                {
                    continue;
                }
                pitch = e.semitones;
            }
            else if (seqlib::midi::IsSetting(e.kind) || e.kind == EventKind::kPreset)
            {
                const int value = e.kind == EventKind::kPreset ? static_cast<int>(e.preset) : e.value;
                const auto it = last.find(e.kind);
                if (it != last.end() && it->second == value)
                {
                    continue;
                }
                last[e.kind] = value;
            }

            perf.tracks[c].push_back(e);
        }
    }

    for (const auto& [tick, bpm] : tempo_)
    {
        perf.tempo.push_back({static_cast<uint32_t>(tick), bpm});
    }
}

void Player::MarkLoop(Performance& perf, const PerformOptions& options) const
{
    // The passes of the main loop, from the loop start to each jump back.
    std::vector<std::pair<int64_t, int64_t>> passes;
    int64_t start = LoopStart(master_);
    for (const int64_t jump : jumps_)
    {
        passes.emplace_back(start, jump);
        start = jump;
    }

    // The first pass that plays as the next does.
    const ntdlib::sequence_export::LoopRules rules;
    const auto loops = static_cast<std::size_t>(std::max(options.loops, 1));
    std::optional<std::size_t> marked;
    for (std::size_t k = 0; k + 1 < passes.size() && k + loops <= passes.size(); k++)
    {
        const auto [s, e] = passes[k];
        if (ntdlib::sequence_export::RepeatsAsTheGame(perf, {}, static_cast<uint32_t>(s), static_cast<uint32_t>(e),
                                                      rules))
        {
            marked = k;
            break;
        }
    }

    // That pass is the loop, played `options.loops` times. Without one, the performance plays as many passes.
    const std::size_t last = marked ? *marked + loops - 1 : std::min(loops, passes.size()) - 1;
    const auto end = static_cast<uint32_t>(passes[last].second);
    if (marked)
    {
        const auto [s, e] = passes[*marked];
        perf.loop = Performance::Loop{static_cast<uint32_t>(s), static_cast<uint32_t>(e)};
        perf.loops = static_cast<int>(loops);
        const ntdlib::sequence_export::PassStart next =
            ntdlib::sequence_export::PassStartAt(perf, {}, static_cast<uint32_t>(e));
        ntdlib::sequence_export::RestoreAtLoopStart(perf, {}, next, rules);
    }

    // The run went on past the end to find the loop. CutAt leaves out the tracks' events after the end. The tempo
    // changes after it go too. A tempo table that starts again in each pass makes some.
    seqlib::midi::ClampNoteOffs(perf, end);
    seqlib::midi::CutAt(perf, end);
    std::erase_if(perf.tempo, [end](const Performance::Tempo& t) { return t.tick > end; });
    perf.end_tick = end;
    perf.seconds = SecondsAt(end);
}

void Player::EndAfterLastSound(Performance& perf, int64_t last_tick, double sound_end) const
{
    perf.truncated = truncated_;
    uint32_t last_event = 0;
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        for (const TrackEvent& e : events)
        {
            last_event = std::max(last_event, e.tick);
        }
    }

    perf.seconds = truncated_ ? SecondsAt(last_tick) : std::max(sound_end, SecondsAt(last_event));
    perf.end_tick = std::max(last_event, static_cast<uint32_t>(TickAt(perf.seconds)));
    if (truncated_)
    {
        seqlib::midi::ClampNoteOffs(perf, static_cast<uint32_t>(last_tick));
        perf.end_tick = static_cast<uint32_t>(last_tick);
    }
}

} // namespace

std::optional<uint16_t> PresetObject(const SongGroup& group, const PresetKey& preset)
{
    const std::vector<Page>& pages = preset.bank == kDrumBank ? group.group.drum_pages : group.group.pages;
    std::optional<uint16_t> object;
    for (const Page& p : pages)
    {
        if (p.program == preset.program)
        {
            object = p.object;
        }
    }

    return object;
}

Performance PlaySong(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments,
                     const PerformOptions& options)
{
    Player player(song, listed, instruments);

    // The passes that mark the loop: the loops asked for, and more to find one that repeats as the next.
    player.Run(static_cast<std::size_t>(std::max(options.loops, 1)) + kMorePasses + 1);

    return player.Finish(options);
}

} // namespace dolphinsf2::musyx
