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
#include <cmath>
#include <iterator>
#include <limits>

#include "musyx_player_parts.h"

namespace dolphinsf2::musyx::player
{

namespace
{

// A channel's controllers below 128 after MusyX's cold reset, which starts each song (seqStartPlay, inpResetMidiCtrl):
// CC7 and CC11 at 127, CC10 at 64, and controllers 98 to 101 at 127. The rest start at 0.
std::array<uint8_t, 128> ColdControllers()
{
    std::array<uint8_t, 128> c{};
    c[kVolumeController] = 127;
    c[kPanController] = 64;
    c[kExpressionController] = 127;
    for (int i = 98; i <= 101; i++)
    {
        c[i] = 127;
    }

    return c;
}

// MusyX's warm reset of channel controllers `c`, for a song's reset of all controllers: the mod wheel and its low
// bits, the pedals from 64 to 67 and 69 at 0, and CC11 and its low bits at 127. The others keep their values.
void WarmReset(std::array<uint8_t, 128>& c)
{
    constexpr int kLowBits = 32;
    for (const int i : {int{kModWheel}, kModWheel + kLowBits, 64, 65, 66, 67, 69})
    {
        c[i] = 0;
    }
    c[kExpressionController] = 127;
    c[kExpressionController + kLowBits] = 127;
}

// The volume or the pan that the game starts an effect with, `given`, or the value of the effect's row, `row`, where
// the game gives none. sndFXStartEx uses the value of the row for 0xFF too.
uint8_t GivenOrRow(std::optional<uint8_t> given, uint8_t row)
{
    constexpr uint8_t kRowValue = 0xff;
    return given && *given != kRowValue ? *given : row;
}

// The rank of an event of `kind`.
Rank RankOf(EventKind kind)
{
    if (kind == EventKind::kNoteOff)
    {
        return Rank::kNoteOff;
    }

    return kind == EventKind::kNoteOn ? Rank::kNoteOn : Rank::kSetting;
}

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

} // namespace

Player::Player(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments)
    : song_(song),
      group_(instruments.Group()),
      instruments_(instruments),
      song_volume_(listed.volume),
      master_volume_(listed.master_volume),
      fade_in_ms_(listed.fade_in_ms),
      controls_(listed.controls),
      controls_ms_(listed.controls_ms),
      tick_step_(listed.tick_step),
      page_of_program_(PagesByProgram(group_.group.pages)),
      drum_of_program_(PagesByProgram(group_.group.drum_pages))
{
    wheel_.fill(kWheelCentre);
    bend_range_.fill({kBendRange, kBendRange});
    controllers_.fill(ColdControllers());

    // The tempo starts at the song's, in 1024ths of a beat a minute.
    const uint32_t bpm = song.fine_tempo ? song.tempo : song.tempo << 10;
    for (SectionState& s : sections_)
    {
        s.bpm = bpm;
    }

    // Each track's first event waits in its section's list.
    master_ = MasterSection();
    looping_ = LoopingSections();
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

Player::Player(const GameEffect& effect, Instruments& instruments)
    : song_(EffectClock()),
      group_(instruments.Group()),
      instruments_(instruments),
      song_volume_(effect.group_volume),
      master_volume_(effect.master_volume),
      page_of_program_(PagesByProgram(group_.group.pages)),
      drum_of_program_(PagesByProgram(group_.group.drum_pages)),
      effect_(true)
{
    wheel_.fill(kWheelCentre);
    bend_range_.fill({kBendRange, kBendRange});
    controllers_.fill(ColdControllers());
    for (SectionState& s : sections_)
    {
        s.bpm = song_.tempo;
    }

    ApplyTempos(sections_[master_], true);
    SetTempo(0, sections_[master_]);
}

std::vector<EffectNote> Player::StartEffect(const GameEffect& effect)
{
    // MusyX starts an effect's controllers at the defaults of a channel. The game sets some of them. They apply to
    // every voice of the effect.
    const ntdlib::musyx::Effect& row = effect.row;
    const int pan = EffectPan(effect);
    const uint8_t velocity = GivenOrRow(effect.volume, row.volume) & 0x7f;
    std::set<std::string> left_out;
    std::vector<EffectNote> notes = EffectNotes(effect, instruments_, left_out);
    for (const std::string& what : left_out)
    {
        approximations_[what]++;
    }

    // A note lasts until its voices fall silent by themselves. A note whose voices sound on stops kHeldEffectSeconds
    // after the effect's last change: the last start of a note, or the last end of a note that falls silent.
    std::vector<std::optional<double>> silences;
    double last_change = 0.0;
    for (const EffectNote& note : notes)
    {
        silences.push_back(HeldSilence(note.voices));
        last_change = std::max(last_change, note.start_ms / 1000.0 + silences.back().value_or(0.0));
    }

    for (std::size_t i = 0; i < notes.size(); i++)
    {
        const EffectNote& note = notes[i];
        const uint8_t channel = kEffectChannels[i];
        TrackEvent preset{.kind = EventKind::kPreset};
        preset.preset =
            seqlib::midi::PresetIndex(presets_, PresetKey{.bank = kEffectBank, .program = static_cast<uint32_t>(i)});
        Record(channel, 0, preset);
        Controller(channel, kVolumeController, effect.volume_controller, 0);
        Controller(channel, kPanController, static_cast<uint8_t>(pan), 0);
        Controller(channel, kExpressionController, 127, 0);
        Controller(channel, kReverbController, NoteReverb(note, effect), 0);
        Controller(channel, kChorusController, NoteSend(note, &MacroVoice::chorus_controller).value_or(effect.chorus),
                   0);
        wheel_[channel] = std::clamp<int>(effect.pitch_wheel, 0, kWheelHighest);

        const double start = note.start_ms / 1000.0;
        const double end = silences[i] ? start + *silences[i] : last_change + kHeldEffectSeconds;
        NoteOfVoices(channel, note.key, velocity, static_cast<uint32_t>(TickAt(end)), 0, TickAt(start), note.voices);
        BendNote(note, channel, start, end);
        PanNote(note, channel, pan, start, end);
    }

    return notes;
}

std::size_t Player::SectionOf(std::size_t track) const
{
    return song_.sections ? song_.tracks[track].section : 0;
}

std::size_t Player::MasterSection() const
{
    // Each section's loop runs from its loop start to the latest of its tracks' loop entries.
    std::optional<std::size_t> master;
    uint32_t longest = 0;
    for (std::size_t t = 0; t < song_.tracks.size(); t++)
    {
        const std::vector<ntdlib::musyx::TrackEntry>& entries = song_.tracks[t].entries;
        if (entries.empty() || entries.back().pattern != ntdlib::musyx::kLoopBack)
        {
            continue;
        }

        const std::size_t section = SectionOf(t);
        const uint32_t start = LoopStart(section);
        const uint32_t length = entries.back().tick > start ? entries.back().tick - start : 0;
        if (!master || length > longest)
        {
            master = section;
            longest = length;
        }
    }

    return master.value_or(song_.tracks.empty() ? 0 : SectionOf(0));
}

std::array<bool, kSections> Player::LoopingSections() const
{
    std::array<bool, kSections> looping{};
    for (std::size_t t = 0; t < song_.tracks.size(); t++)
    {
        const std::vector<ntdlib::musyx::TrackEntry>& entries = song_.tracks[t].entries;
        if (!entries.empty() && entries.back().pattern == ntdlib::musyx::kLoopBack)
        {
            looping[SectionOf(t)] = true;
        }
    }

    return looping;
}

std::size_t Player::LeastLoops() const
{
    std::size_t least = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < kSections; i++)
    {
        if (looping_[i])
        {
            least = std::min(least, sections_[i].loops.size());
        }
    }

    return least;
}

int64_t Player::PlaceAt(std::size_t section, int64_t tick) const
{
    // The first pass starts at the performance's start. Each pass after it starts at the loop start.
    const std::vector<int64_t>& loops = sections_[section].loops;
    const auto after = std::upper_bound(loops.begin(), loops.end(), tick);
    return after == loops.begin() ? tick : LoopStart(section) + (tick - *std::prev(after));
}

bool Player::StateRepeats(int64_t start, int64_t end) const
{
    for (std::size_t i = 0; i < kSections; i++)
    {
        if (looping_[i] && PlaceAt(i, start) != PlaceAt(i, end))
        {
            return false;
        }
    }

    return true;
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
        Controller(c, kVolumeController, channel.volume, 0);
        Controller(c, kPanController, channel.pan, 0);
        Controller(c, kExpressionController, 127, 0);
        Controller(c, kReverbController, channel.reverb, 0);
        Controller(c, kChorusController, channel.chorus, 0);
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

    // MusyX sends the clock back once the section's list is empty, at the tick that the clock has reached. A loop
    // therefore comes after the last loop event of its tracks. That event may come in this run or a later one.
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
            if (!s.loop_due)
            {
                return false;
            }

            // A loop whose events all come at its start would loop for ever in one run.
            if (looped)
            {
                throw FormatError("the song loops without time passing");
            }
            looped = true;
            StartLoop(section);
            s.loop_due = false;
            continue;
        }

        const Event e = s.pending.front();
        s.pending.erase(s.pending.begin());
        if (HandleEvent(e, section))
        {
            s.loop_due = true;
        }
    }
}

void Player::StartLoop(std::size_t section)
{
    // The other clock starts at the loop start where this clock stands, with its fraction of a tick. The tempo table
    // starts again, and its changes up to the loop start apply.
    SectionState& s = sections_[section];
    const auto clock = static_cast<std::size_t>(s.clock);
    const auto other = clock ^ 1;
    const uint32_t loop_start = LoopStart(section);
    s.offset[other] = s.offset[clock] + static_cast<int64_t>(s.time[clock] >> 16) - loop_start;
    s.time[other] = static_cast<uint64_t>(loop_start) << 16 | (s.time[clock] & 0xffff);
    s.clock = static_cast<int>(other);
    s.last_run_tick = static_cast<int64_t>(loop_start) - 1;
    if (!song_.tempo_changes.empty())
    {
        s.tempo_at = 0;
    }
    ApplyTempos(s, false);
    s.loops.push_back(s.offset[other] + loop_start);
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

bool Player::HandleEvent(const Event& e, std::size_t section)
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
                Controller(channel, kVolumeController, entry.volume, tick);
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
            const int value = std::clamp(t.pattern->pitch_wheel[e.index].value, 0, kWheelHighest);
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
            const int value = std::clamp(t.pattern->mod_wheel[e.index].value, 0, kWheelHighest);
            Controller(channel, kModWheel, static_cast<uint8_t>(value >> 7), tick);
            break;
        }

    case EventType::kLoop:
        return true;
    }

    if (const std::optional<Event> next = NextEvent(e.track))
    {
        Insert(sections_[section], *next);
    }

    return false;
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
    page_key_[channel] = key;
    variant_controls_[channel].reset();

    // A page whose voices read the channel's controllers plays a variant preset, which the next note picks
    // (TakeControls).
    if (!instruments_.ObjectControllers(page.object).empty())
    {
        return;
    }

    TrackEvent e{.kind = EventKind::kPreset};
    e.preset = seqlib::midi::PresetIndex(presets_, key);
    RecordPreset(channel, tick, e);
}

void Player::Controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t tick)
{
    // A reset of all controllers is the warm reset of MusyX. The other MusyX commands among the controllers leave the
    // controllers of the channel as they are.
    if (controller == kResetControllers)
    {
        WarmReset(controllers_[channel]);
    }
    else if (controller < 128 && controller != kCrossfade && controller != kPriority && controller != kPriorityHigh &&
             controller != kAllNotesOff)
    {
        controllers_[channel][controller] = value;
    }

    TrackEvent e;
    e.value = value;
    switch (controller)
    {
    case kModWheel:
        e.kind = EventKind::kModulation;
        break;

    // The channel's volume as the song sets it. AddEvents maps it to the voices' curve (MapLevel).
    case kVolumeController:
        e.kind = EventKind::kVolume;
        break;

    case kPanController:
        e.kind = EventKind::kPan;
        break;

    case kExpressionController:
        e.kind = EventKind::kExpression;
        break;

    case kReverbController:
        e.kind = EventKind::kReverb;
        break;

    case kChorusController:
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
        if (controller != kResetControllers && !ReadByVoices(channel, controller))
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

    std::vector<KeyVoice> voices = VoicesOf(group_.pool, *channel_object_[channel], key);
    TakeControls(channel, voices, velocity, tick);
    NoteOfVoices(channel, key, velocity, end, section, tick, std::move(voices));
}

bool Player::ReadByVoices(uint8_t channel, uint8_t controller)
{
    if (!channel_object_[channel])
    {
        return false;
    }

    const std::vector<uint8_t>& read = instruments_.ObjectControllers(*channel_object_[channel]);
    return std::ranges::find(read, KeptController(controller)) != read.end();
}

uint8_t Player::HeldController(uint8_t channel, uint8_t controller) const
{
    if (controller < controllers_[channel].size())
    {
        return controllers_[channel][controller];
    }

    if (controller == kPitchWheelControl)
    {
        return static_cast<uint8_t>(wheel_[channel] >> 7);
    }

    return controller == kController132 ? 64 : 0;
}

void Player::TakeControls(uint8_t channel, std::vector<KeyVoice>& voices, uint8_t velocity, int64_t tick)
{
    const uint16_t object = *channel_object_[channel];
    const std::vector<uint8_t>& read = instruments_.ObjectControllers(object);
    if (read.empty() || !page_key_[channel])
    {
        return;
    }

    // MusyX reads the controllers when the voice's macro runs its step. The files take them at the note's start. Each
    // voice takes its start volume in place of the velocity.
    ChannelControls controls;
    for (const uint8_t c : read)
    {
        controls.emplace_back(
            c, instruments_.ObjectValue(object, c, c == kVelocityControl ? velocity : HeldController(channel, c)));
    }
    for (KeyVoice& v : voices)
    {
        v.controls = VoiceControls(controls, v.volume);
    }

    // A note that still sounds keeps the preset that it started with.
    if (variant_controls_[channel] != controls)
    {
        const PresetKey& page = *page_key_[channel];
        const PresetKey key{.bank = instruments_.VariantBank(page.bank, page.program, controls),
                            .program = page.program};
        TrackEvent e{.kind = EventKind::kPreset};
        e.preset = seqlib::midi::PresetIndex(presets_, key);
        RecordPreset(channel, tick, e);
        variant_controls_[channel] = std::move(controls);
    }
}

void Player::NoteOfVoices(uint8_t channel, uint8_t key, uint8_t velocity, uint32_t end, std::size_t section,
                          int64_t tick, std::vector<KeyVoice> voices)
{
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
    n.voices = std::move(voices);

    // The channel bends over the range of the note's first voice.
    if (!n.voices.empty())
    {
        const MacroVoice& voice = instruments_.Voice(n.voices.front());
        n.bend_range = {voice.bend_up, voice.bend_down};
        SetBendRange(channel, n.bend_range, tick);
    }
    n.wheel = Semitones(channel, wheel_[channel]);

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

void Player::Record(uint8_t channel, int64_t tick, TrackEvent e, std::optional<Rank> rank)
{
    events_[channel].push_back({tick, rank.value_or(RankOf(e.kind)), order_++, e});
}

void Player::RecordPreset(uint8_t channel, int64_t tick, TrackEvent e)
{
    const auto starts_note = [tick](const ChannelEvent& ce)
    {
        return ce.tick == tick && ce.event.kind == EventKind::kNoteOn;
    };
    const bool after_notes = std::ranges::any_of(events_[channel], starts_note);
    Record(channel, tick, e, after_notes ? std::optional(Rank::kNoteOn) : std::nullopt);
}

void Player::SetCurve(NoteRecord& n)
{
    // The note follows the curve of its first voice that plays a sample.
    const MacroVoice* first = nullptr;
    for (const KeyVoice& kv : n.voices)
    {
        const MacroVoice& voice = instruments_.Voice(kv);
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
        const MacroVoice& voice = instruments_.Voice(kv);
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

double Player::FadeAt(int64_t tick) const
{
    if (fade_in_ms_ <= 0.0)
    {
        return 1.0;
    }

    // The fader starts at 0 and takes a step at each frame (SetupFader and synthHandle). The margin keeps a step's
    // tick, the first tick at or after the step, from falling a rounding error short of it.
    const double steps = std::floor(SecondsAt(tick) * 1000.0 / kFaderStepMs + 1e-6);
    return std::min(steps * kFaderStepMs / fade_in_ms_, 1.0);
}

void Player::AddFadeSteps(std::vector<ChannelEvent>& events) const
{
    if (fade_in_ms_ <= 0.0)
    {
        return;
    }

    std::sort(events.begin(), events.end(), EarlierEvent);

    // Each step comes after the settings of its tick and before its notes.
    std::vector<ChannelEvent> steps;
    std::size_t next = 0;
    int set_volume = -1;
    for (double ms = kFaderStepMs; ms < fade_in_ms_ + kFaderStepMs; ms += kFaderStepMs)
    {
        const int64_t tick = TickAt(ms / 1000.0);
        for (; next < events.size() && events[next].tick <= tick; next++)
        {
            const TrackEvent& e = events[next].event;
            if (e.kind == EventKind::kVolume && e.part == 0)
            {
                set_volume = e.value;
            }
        }
        if (set_volume < 0)
        {
            continue;
        }

        TrackEvent step;
        step.kind = EventKind::kVolume;
        step.value = static_cast<uint8_t>(set_volume);
        steps.push_back({tick, Rank::kSetting, std::numeric_limits<uint64_t>::max(), step});
    }
    events.insert(events.end(), steps.begin(), steps.end());
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
        // A note's velocity takes the volume without the fade-in. The fade then scales the note on the players' curve.
        levels.full_volume = WrittenValue(channel, ChannelVolume(e.value));
        const double fade = FadeAt(e.tick);
        levels.volume = fade < 1.0 ? WrittenValue(channel, ChannelVolume(e.value) * fade) : levels.full_volume;
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
            e.value / 127.0 * WrittenVolume(channel, levels.full_volume) * WrittenVolume(channel, levels.expression);
        const double written = levels.full_volume / 127.0 * levels.expression / 127.0;
        const int velocity = written > 0.0 ? SquaredValue(TableLevel(table, volume) / (written * written))
                                           : SquaredValue(TableLevel(table, e.value / 127.0));
        e.value = static_cast<uint8_t>(std::max(velocity, 1));
    }
}

bool Player::Step()
{
    // The game's code sets the song's controllers once the song plays (GameSong::controls). They take the tick of the
    // first run at or after their time.
    if (!controls_.empty() && static_cast<double>(runs_) * kRunSeconds * 1000.0 >= controls_ms_)
    {
        const int64_t tick = TickAt(static_cast<double>(runs_) * kRunSeconds);
        for (const SongControl& c : controls_)
        {
            Controller(c.channel, c.controller, c.value, tick);
        }
        controls_.clear();
    }

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

void Player::Run(std::size_t passes, std::size_t section_loops)
{
    constexpr auto kMostRuns = static_cast<uint64_t>(kLimitSeconds / kRunSeconds);
    while (!ended_ && (jumps_.size() < passes || LeastLoops() < section_loops))
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

} // namespace dolphinsf2::musyx::player

namespace dolphinsf2::musyx
{

using player::GivenOrRow;
using player::kEffectChannels;
using player::kLongestCurveMs;
using player::kMorePasses;
using player::Page;
using player::Player;

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

    // The passes that mark the loop: the loops asked for, and more to find one that repeats as the next. Each section
    // that loops loops as many times as asked for.
    const auto loops = static_cast<std::size_t>(std::max(options.loops, 1));
    player.Run(loops + kMorePasses + 1, loops);

    return player.Finish(options);
}

int EffectPan(const GameEffect& effect)
{
    return std::clamp(GivenOrRow(effect.pan, effect.row.pan) + effect.pan_controller - 64, 0, 127);
}

std::vector<EffectNote> EffectNotes(const GameEffect& effect, Instruments& instruments,
                                    std::set<std::string>& approximations)
{
    std::vector<EffectNote> notes;
    // Each note's curves of its pitch, without the wheel, and of its pan.
    std::vector<std::pair<std::vector<PitchPoint>, std::vector<PanPoint>>> curves;
    for (const EffectVoice& ev : EffectVoicesOf(instruments, effect.row.object, effect.row.key & 0x7f, approximations))
    {
        KeyVoice voice = ev.voice;
        voice.from_ms = instruments.Macro(voice.macro, voice.part, 0.0).sample_ms;
        const MacroVoice& v = instruments.Voice(voice);
        if (!v.sample)
        {
            continue;
        }
        const Sample* sample = instruments.SampleOf(v);

        // Voices that start their samples at the same time and whose pitches and pans follow the same curves share a
        // note. A channel bends and pans its notes together.
        const double start = ev.start_ms + voice.from_ms;
        const auto curve = std::pair(
            sample != nullptr ? PitchCurve(v, *sample, voice.key, 0.0, kLongestCurveMs) : std::vector<PitchPoint>{{}},
            PanCurve(v, kLongestCurveMs));
        std::size_t note = 0;
        for (; note < notes.size(); note++)
        {
            if (notes[note].start_ms == start && curves[note] == curve)
            {
                break;
            }
        }
        if (note == notes.size() && notes.size() == kEffectChannels.size())
        {
            // A voice whose pitch moves apart can share a note that starts with it.
            const auto at_start = [start](const EffectNote& n)
            {
                return n.start_ms == start;
            };
            note = static_cast<std::size_t>(std::find_if(notes.begin(), notes.end(), at_start) - notes.begin());
            if (note == notes.size())
            {
                approximations.insert("voices of effects that start at more than 15 times left out");
                continue;
            }
        }
        if (note == notes.size())
        {
            notes.push_back({.start_ms = start, .key = static_cast<uint8_t>(voice.key), .voices = {}});
            curves.push_back(curve);
        }
        notes[note].voices.push_back(voice);
    }

    // The zones of a note start at the pan of the note's start where they can. The shift holds CC10 and the zones' pans
    // within 0 to 127 along the curve. Such a shift always exists: the voice's pan stays within 0 to 127.
    const int pan = EffectPan(effect);
    for (std::size_t i = 0; i < notes.size(); i++)
    {
        const std::vector<PanPoint>& curve = curves[i].second;
        if (curve.empty())
        {
            continue;
        }
        const auto [low, high] = std::minmax_element(
            curve.begin(), curve.end(), [](const PanPoint& a, const PanPoint& b) { return a.offset < b.offset; });
        const int zone = *instruments.Voice(notes[i].voices.front()).pan;
        const double least = std::max(pan + high->offset - 127.0, -static_cast<double>(zone));
        const double most = std::min(pan + low->offset, 127.0 - zone);
        notes[i].pan_shift = std::min(std::max(curve.front().offset, least), most);
    }
    std::stable_sort(notes.begin(), notes.end(),
                     [](const EffectNote& a, const EffectNote& b) { return a.start_ms < b.start_ms; });

    return notes;
}

EffectPerformance PlayEffect(const GameEffect& effect, Instruments& instruments)
{
    // An effect has no loop. The performance ends when the effect's last note falls silent.
    Player player(effect, instruments);
    std::vector<EffectNote> notes = player.StartEffect(effect);
    player.Run(1);

    return {player.Finish({}), std::move(notes)};
}

} // namespace dolphinsf2::musyx