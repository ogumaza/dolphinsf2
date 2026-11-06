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
#include <iterator>
#include <limits>
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

// The milliseconds over which EffectNotes compares the curves of its voices' pitches and pans.
constexpr double kLongestCurveMs = 60000.0;

// A run of the sequencer: a millisecond of MusyX's clock. The output's rate makes it a little shorter.
constexpr double kRunSeconds = 0.001 * kNominalRate / kOutputRate;

// The channel that plays the drum pages, and the channels in all.
constexpr uint8_t kDrumChannel = 9;
constexpr std::size_t kChannels = 16;
constexpr std::size_t kSections = 16;

// The channels of an effect's notes, in order: every channel but the drums'.
constexpr std::array<uint8_t, 15> kEffectChannels{0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};

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

// A channel's controllers below 128 after MusyX's cold reset, which starts each song (seqStartPlay, inpResetMidiCtrl):
// CC7 and CC11 at 127, CC10 at 64, and controllers 98 to 101 at 127. The rest start at 0.
std::array<uint8_t, 128> ColdControllers()
{
    std::array<uint8_t, 128> c{};
    c[7] = 127;
    c[10] = 64;
    c[11] = 127;
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
    for (const int i : {1, 33, 64, 65, 66, 67, 69})
    {
        c[i] = 0;
    }
    c[11] = 127;
    c[43] = 127;
}

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
    std::vector<int64_t> loops; // the performance's tick at the start of each of the section's passes after the first
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
    float wheel = 0.0f;                                     // the pitch wheel's bend at the note's start, in semitones
    bool other_range = false;                               // the wheel has bent the note over another note's range
    bool sounds = false;                                    // a voice plays a sample
    bool on_table = false;                                  // the first such voice plays on MusyX's volume table
};

// A note of a song whose voice sweeps its pitch. It plays on part `part` of its channel (TrackEvent::part) and bends
// along `curve` (PitchCurve) from tick `on` to tick `end`, where its sound ends.
struct BentNote
{
    uint8_t channel = 0;
    uint8_t part = 0;
    int64_t on = 0;
    int64_t end = 0;
    float wheel = 0.0f; // the pitch wheel's bend in semitones that the curve takes
    std::vector<PitchPoint> curve;
};

// A note of a song, as CutTails sees it, in seconds: its start, the file's note-off and where the game's voices of the
// note have all fallen silent. `linear_ms` holds the longest linear release that a voice plays from the note-off, in
// MusyX's milliseconds, or 0. `fades_to_release` is true where a voice fades after the note's release and then plays
// a linear release at a KeyOff (KeyOff::kFade with ReleaseFade::releases).
struct NoteTail
{
    uint8_t channel = 0;
    uint8_t part = 0;
    double on = 0.0;
    double off = 0.0;
    double silent = 0.0;
    double linear_ms = 0.0;
    bool fades_to_release = false;
};

// A cut of the tails of a channel's notes once the game's voices of the channel have all fallen silent. CC11 follows
// `steps`, each a time in seconds and a share of the channel's amplitude, down to 0 at `at`. CC120 then stops the
// sounds of each part in `parts`, and CC11 goes back to its value.
struct TailCut
{
    uint8_t channel = 0;
    std::set<uint8_t> parts;
    std::vector<std::pair<double, double>> steps;
    double at = 0.0;
};

// The seconds of the fade before a cut that follows no linear release. The fade falls linearly in amplitude, a step
// every kFaderStepMs.
constexpr double kShortCutSeconds = 0.02;

// A channel's CC7 and CC11 in the MIDI file.
struct ChannelLevels
{
    int volume = 127;      // the written CC7
    int full_volume = 127; // the written CC7 without the song's fade-in
    int expression = 127;
};

// The milliseconds of each step of MusyX's faders: the length of the DSP's frame (synthHandle).
constexpr double kFaderStepMs = 5.0;

// The MIDI value from 0 to 127 whose square is `amplitude`. SoundFont's and SFZ's default curves square the
// velocity, CC7 and CC11.
int SquaredValue(double amplitude)
{
    return static_cast<int>(std::lround(127.0 * std::sqrt(std::clamp(amplitude, 0.0, 1.0))));
}

// Where an event goes among a channel's events at one tick: note-offs come first, then settings, then note-ons. The
// note-off of a note that ends on the tick it starts comes last.
enum class Rank
{
    kNoteOff,
    kSetting,
    kNoteOn,
    kEmptyNoteOff,
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

// The clock of an effect's performance: a song without tracks, at 156.25 beats a minute in 1024ths. A tick of it lasts
// a millisecond of MusyX's clock.
const ntdlib::musyx::Song& EffectClock()
{
    static const ntdlib::musyx::Song kClock = []
    {
        ntdlib::musyx::Song clock;
        clock.fine_tempo = true;
        clock.tempo = 60000 * 1024 / ntdlib::musyx::kTicksPerBeat;
        return clock;
    }();

    return kClock;
}

class Player
{
public:
    Player(const ntdlib::musyx::Song& song, const GameSong& listed, Instruments& instruments);

    // A player of `effect` on the effect's clock (EffectClock). StartEffect starts the effect.
    Player(const GameEffect& effect, Instruments& instruments);

    // Start the player's effect. Note i of EffectNotes plays on channel kEffectChannels[i] from the note's start.
    void StartEffect(const GameEffect& effect);

    // Run until the song ends, or the time limit. The run stops earlier once the main loop has played `passes` passes
    // and each section that loops has looped `section_loops` times.
    void Run(std::size_t passes, std::size_t section_loops = 0);

    // The performance: the recorded events, the presets and the tempo, with the main loop marked.
    Performance Finish(const PerformOptions& options);

private:
    // The section that plays track `track`. A song without sections plays every track in section 0.
    std::size_t SectionOf(std::size_t track) const;

    // The section whose clock gives the tempo and the passes: the section with the longest loop. A song whose tracks
    // don't loop takes the first track's section. MusyX loops each section on its own and treats no section as the
    // master. Where the sections' loops differ in length, a pass of the master can play differently from the one
    // before it.
    std::size_t MasterSection() const;

    // Which sections loop: those with a track whose last entry goes back to the loop start.
    std::array<bool, kSections> LoopingSections() const;

    // The fewest times that a section that loops has looped, or the most a size_t holds if no section loops.
    std::size_t LeastLoops() const;

    // The tick of the clock of section `section` at the performance's tick `tick`.
    int64_t PlaceAt(std::size_t section, int64_t tick) const;

    // True if each section that loops stands at the same place in its loop at the performance's ticks `start` and
    // `end`. MusyX then plays from `end` as it did from `start`.
    bool StateRepeats(int64_t start, int64_t end) const;

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

    // Send the clock of section `section` back to its loop start, at the tick that the clock has reached. The tracks
    // that loop start again.
    void StartLoop(std::size_t section);

    // Handle event `e` of section `section`, and put its track's next event in the section's list. For a track's loop,
    // put nothing in the list and return true. The section loops once its list is empty. StartLoop then puts the
    // tracks' next events in it.
    bool HandleEvent(const Event& e, std::size_t section);

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

    // Give `voices`, a note of `velocity` on `channel` at `tick`, the channel's controllers that their macros read, and
    // the velocity if they read it. The channel then plays the preset of its page at those values.
    void TakeControls(uint8_t channel, std::vector<KeyVoice>& voices, uint8_t velocity, int64_t tick);

    // Whether the voices of the page of `channel` read `controller` (Instruments::ObjectControllers).
    bool ReadByVoices(uint8_t channel, uint8_t controller);

    // Start a note of `voices`. NoteOn takes the voices that the channel's object starts for the key.
    void NoteOfVoices(uint8_t channel, uint8_t key, uint8_t velocity, uint32_t end, std::size_t section, int64_t tick,
                      std::vector<KeyVoice> voices);

    // Add `e` to the events of `channel` at `tick`. The event's kind gives its rank among the tick's events, unless
    // `rank` gives it.
    void Record(uint8_t channel, int64_t tick, TrackEvent e, std::optional<Rank> rank = std::nullopt);

    // Add preset event `e` to the events of `channel` at `tick`. MusyX plays a pattern's events in their order. A note
    // that comes before the change on the same tick plays the channel's earlier preset (Resident Evil Zero's bgm_75,
    // Rogue Leader's chords across a split). The change then keeps its place among the tick's notes.
    void RecordPreset(uint8_t channel, int64_t tick, TrackEvent e);

    // Find the volume curve that note `n` follows, from its voices. Count what that approximates.
    void SetCurve(NoteRecord& n);

    // Pick the curve of each channel's CC7 and CC11: MusyX's volume table for a channel whose sounding notes all play
    // on it, else the DLS curve.
    void ChooseCurves();

    // The product of the channel's volume `value`, the song's volume and the master volume.
    double ChannelVolume(int value) const;

    // The share of the song's volume that its fade-in has reached at tick `tick`, from 0 to 1.
    double FadeAt(int64_t tick) const;

    // Add to `events`, the events of channel `channel`, a setting of CC7 at each step of the song's fade-in after the
    // channel's first CC7. MapLevel scales each CC7 by the fade.
    void AddFadeSteps(std::vector<ChannelEvent>& events) const;

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

    // The part of its channel on which `n`, a note of a song from `on_seconds` to `end_seconds`, bends along the curve
    // of its first voice whose pitch sweeps, or nothing. A MIDI channel bends all of its notes. The note therefore goes
    // on a part of the channel where no other bent note sounds, while MIDI's 16 channels last. The note's note-on goes
    // on that part.
    std::optional<uint8_t> BendPart(const NoteRecord& n, double on_seconds, double end_seconds);

    // Add to `events`, the events of channel `channel`, the events of the parts of its bent notes. Each such part takes
    // every setting of the channel and the channel's bends outside its notes. During a note, the part bends along the
    // note's curve, moved by the pitch wheel's moves since the note's start.
    void AddBentParts(uint8_t channel, std::vector<ChannelEvent>& events);

    // The milliseconds after its note's start up to which voice `kv`, the voice `v`, has to last for a note released
    // `release_ms` after its start. A voice that plays on after the release goes on with its attack. A file's note that
    // ends during its attack releases from the level that it has reached. A voice that follows its fade therefore lasts
    // until the file's attack reaches the game's highest level. That level is the highest point of the attack's line
    // times the fade. A voice that goes on with its held fade (FadesThroughRelease) lasts for its whole attack. Any
    // other voice ends at the release: MusyX's release starts from the level reached too. Return 0 once the attack is
    // over.
    double RiseMs(const MacroVoice& v, const KeyVoice& kv, double release_ms) const;

    // The seconds where voice `kv`, the voice `v`, ends, for a note from `on_seconds` to `off_seconds`. A voice that
    // ignores the release plays its sample to the end. A voice that stops or releases by itself, whatever the release
    // does, ends then. So does a voice that decays to silence. A voice that plays on through its attack lasts as RiseMs
    // says. Add to `approximated` what the end approximates.
    double VoiceEnd(const MacroVoice& v, const KeyVoice& kv, double on_seconds, double off_seconds,
                    std::set<std::string>& approximated);

    // The seconds that the sample of voice `v` plays, or nothing if it loops. `key` is the key that the voice starts
    // with.
    std::optional<double> SampleSecondsOf(const MacroVoice& v, int key);

    // The seconds after its start at which a held note of `voices` falls silent by itself: where the macro of each
    // voice that plays a sample stops it or fades it to silence, or where the sample ends. Return nothing if one of
    // them sounds on.
    std::optional<double> HeldSilence(const std::vector<KeyVoice>& voices);

    // The send that the voices of `note`, a note of an effect, set through `send` (MacroVoice::reverb_controller or
    // chorus_controller), from 0 to 127. A channel has one send: the voices' largest. Nothing if no voice sets it.
    std::optional<uint8_t> NoteSend(const EffectNote& note, std::optional<double> MacroVoice::* send);

    // The send to aux A of `note`, a note of `effect`, from 0 to 127: as NoteSend gives it, with the sources that the
    // voices' ReverbSelect steps name in place of the reverb controller (MacroVoice::reverb_source). MusyX starts an
    // effect's controllers at their defaults. The game sets the volume, the pan and the sends. The mod wheel stays at
    // 0.
    uint8_t NoteReverb(const EffectNote& note, const GameEffect& effect);

    // Bend `channel`, the channel of `note`, a note of an effect, from `start` to `end` seconds and through its
    // voices' tails: by the pitch wheel, or along the curve of a voice's pitch that moves (PitchCurve). A channel has
    // one curve: the first such voice's.
    void BendNote(const EffectNote& note, uint8_t channel, double start, double end);

    // Pan `channel`, the channel of `note`, a note of an effect at `pan`, from `start` to `end` seconds and through its
    // voices' tails, along the curve of a voice's pan that moves (PanCurve). A channel has one curve: the curve of the
    // first such voice. CC10 takes away the note's pan_shift.
    void PanNote(const EffectNote& note, uint8_t channel, int pan, double start, double end);

    // The seconds where voice `v` falls silent, for a voice that goes on with its held fade through the release
    // (FadesThroughRelease) and a note from `on_seconds`: where the fade does, or where a sample ends before that.
    // `key` is the key that the voice starts with.
    double FadeEnd(const MacroVoice& v, int key, double on_seconds);

    // Put each channel's events in `perf`, in order, and the tempo changes.
    void AddEvents(Performance& perf);

    // Mark the main loop of `perf`: the first pass that plays as the next does, played `options.loops` times. Cut the
    // performance after the last pass that it plays.
    void MarkLoop(Performance& perf, const PerformOptions& options) const;

    // End `perf`, the performance of a song that doesn't loop, after its last event and after its last sound, which
    // ends at `sound_end` seconds. A run cut at the time limit ends at `last_tick`.
    void EndAfterLastSound(Performance& perf, int64_t last_tick, double sound_end) const;

    // The cut of the tails of channel `channel` at `silent` seconds, where the game's voices of `notes`, the notes of
    // the channel that start before then, have all fallen silent. `last` is the note whose voices fall silent there.
    // Nothing if no linear release of the files sounds on there, or if a note starts during the fade.
    std::optional<TailCut> TailCutAt(uint8_t channel, const std::vector<const NoteTail*>& notes, double silent,
                                     const NoteTail& last) const;

    // Cut the tails of the songs' linear releases (see the comment of the definition).
    void CutTails(Performance& perf) const;

    const ntdlib::musyx::Song& song_;
    const SongGroup& group_;
    Instruments& instruments_;
    uint8_t song_volume_ = 127;   // the volume that the game's song table gives the song
    uint8_t master_volume_ = 127; // the game's master volume for music
    double fade_in_ms_ = 0.0;     // GameSong::fade_in_ms
    std::vector<SongControl> controls_; // GameSong::controls, until the song reaches their time
    double controls_ms_ = 0.0;          // GameSong::controls_ms
    TickStep tick_step_ = TickStep::kFloat;
    std::array<uint8_t, 128> page_of_program_{};
    std::array<uint8_t, 128> drum_of_program_{};

    std::vector<TrackState> tracks_;
    std::array<SectionState, kSections> sections_{};
    std::size_t master_ = 0;                // the section whose clock gives the tempo and the passes
    std::array<bool, kSections> looping_{}; // the sections that loop
    std::array<std::optional<uint16_t>, kChannels> channel_object_{}; // the object that the channel's page plays
    std::array<int, kChannels> wheel_{};                              // the channel's latest pitch-wheel value
    std::array<std::pair<int, int>, kChannels> bend_range_{};         // the channel's range up and down
    std::array<bool, kChannels> on_table_{}; // the channel's CC7 and CC11 follow MusyX's volume table (ChooseCurves)
    // The channel's controllers below 128 as the song has set them, from MusyX's cold reset.
    std::array<std::array<uint8_t, 128>, kChannels> controllers_{};
    std::array<std::optional<PresetKey>, kChannels> page_key_{}; // the preset of the channel's page
    // The controllers' values of the channel's variant preset, if it plays one (Instruments::VariantBank).
    std::array<std::optional<ChannelControls>, kChannels> variant_controls_{};
    std::vector<ActiveNote> active_;
    std::vector<NoteRecord> notes_;
    std::vector<BentNote> bent_;
    std::vector<NoteTail> tails_; // each note of a song, for CutTails
    std::array<std::vector<ChannelEvent>, kChannels> events_;
    uint64_t order_ = 0;
    uint32_t next_note_ = 1;
    std::vector<PresetKey> presets_;
    std::vector<std::pair<int64_t, double>> tempo_; // the performance's tick and the beats a minute
    std::vector<int64_t> jumps_;                    // the tick of each pass after the first
    uint64_t runs_ = 0;
    bool ended_ = false;
    bool truncated_ = false;
    bool effect_ = false; // the player plays an effect, whose notes play the voices that macros start (EffectNotes)
    std::map<std::string, uint32_t> approximations_;
};

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

void Player::StartEffect(const GameEffect& effect)
{
    // MusyX starts an effect's controllers at the defaults of a channel. The game sets some of them. They apply to
    // every voice of the effect.
    const ntdlib::musyx::Effect& row = effect.row;
    const int pan = EffectPan(effect);
    const uint8_t velocity = effect.volume.value_or(row.volume) & 0x7f;
    std::set<std::string> left_out;
    const std::vector<EffectNote> notes = EffectNotes(effect, instruments_, left_out);
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
        Controller(channel, 7, effect.volume_controller, 0);
        Controller(channel, 10, static_cast<uint8_t>(pan), 0);
        Controller(channel, 11, 127, 0);
        Controller(channel, 91, NoteReverb(note, effect), 0);
        Controller(channel, 93, NoteSend(note, &MacroVoice::chorus_controller).value_or(effect.chorus), 0);
        wheel_[channel] = std::clamp<int>(effect.pitch_wheel, 0, 0x3fff);

        const double start = note.start_ms / 1000.0;
        const double end = silences[i] ? start + *silences[i] : last_change + kHeldEffectSeconds;
        NoteOfVoices(channel, note.key, velocity, static_cast<uint32_t>(TickAt(end)), 0, TickAt(start), note.voices);
        BendNote(note, channel, start, end);
        PanNote(note, channel, pan, start, end);
    }
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
    // MusyX sends the clock back once the section's list is empty, at the tick that the clock has reached. A loop
    // therefore comes after the last of its tracks' loop events.
    bool loop_due = false; // a track has looped since the section's clock last went back
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
            if (!loop_due)
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
            loop_due = false;
            continue;
        }

        const Event e = s.pending.front();
        s.pending.erase(s.pending.begin());
        if (HandleEvent(e, section))
        {
            loop_due = true;
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
    // MusyX's commands among the controllers leave the channel's controllers as they are.
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
    return std::ranges::find(read, controller < 0x40 ? controller & 0x1f : controller) != read.end();
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
            c, instruments_.ObjectValue(object, c, c == kVelocityControl ? velocity : controllers_[channel][c]));
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

    const auto earlier = [](const ChannelEvent& a, const ChannelEvent& b)
    {
        return std::tie(a.tick, a.rank, a.order) < std::tie(b.tick, b.rank, b.order);
    };
    std::sort(events.begin(), events.end(), earlier);

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
    constexpr auto kMostRuns = static_cast<uint64_t>(kMaxSeconds / kRunSeconds);
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
    CutTails(perf);

    return perf;
}

// The seconds that voice `v` sounds after its end: its release, its macro's fade up to its stop and the stop's fade,
// or the fade of a voice that stops.
double TailOf(const MacroVoice& v)
{
    if (v.key_off == KeyOff::kRelease && v.envelope)
    {
        return v.envelope->release_ms / 1000.0;
    }
    if (v.key_off == KeyOff::kFade)
    {
        const bool releases = v.release_fade.releases && v.envelope;
        return (v.release_fade.stop_ms + (releases ? v.envelope->release_ms : kCutMs)) / 1000.0;
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
        double faded = 0.0; // where the voices that go on with their held fades through the release fall silent
        double linear_ms = 0.0;
        bool fades_to_release = false;
        std::set<std::string> approximated;
        for (const KeyVoice& kv : n.voices)
        {
            const MacroVoice& v = instruments_.Voice(kv);
            approximated.insert(v.approximations.begin(), v.approximations.end());
            if (!effect_ && !v.started.empty())
            {
                approximated.insert(kCommandsLeftOut);
            }
            if (!effect_ && (v.volume_controller || v.reverb_controller || v.chorus_controller))
            {
                approximated.insert("volumes and sends that a macro sets left out");
            }
            if (!effect_ && v.reverb_source)
            {
                approximated.insert(kReverbSourcesLeftOut);
            }
            if (!effect_ && v.pan_last && v.pan_moves)
            {
                approximated.insert("pans that move keep their first pan");
            }
            if (!v.sample)
            {
                continue;
            }

            const double end = VoiceEnd(v, kv, on_seconds, off_seconds, approximated);
            latest = std::max(latest.value_or(end), end);
            earliest = std::min(earliest.value_or(end), end);

            if (FadesThroughRelease(v))
            {
                faded = std::max(faded, FadeEnd(v, kv.key, on_seconds));
            }
            else if (v.key_off == KeyOff::kFade && end > off_seconds)
            {
                // The voice's fade runs from the note's release, not from the file's note-off.
                faded = std::max(faded, off_seconds + TailOf(v));
            }
            else
            {
                tail = std::max(tail, TailOf(v));
                // A voice that ends before the note-off has decayed to silence or stopped. Its release doesn't sound.
                if (v.key_off == KeyOff::kRelease && v.envelope && !v.envelope->dls && end >= off_seconds)
                {
                    linear_ms = std::max(linear_ms, v.envelope->release_ms);
                }
            }
            if (v.key_off == KeyOff::kFade && v.release_fade.releases && v.envelope && !v.envelope->dls &&
                v.envelope->release_ms > 0.0 && end >= off_seconds)
            {
                fades_to_release = true;
            }
        }

        const double end_seconds = latest.value_or(off_seconds);
        std::optional<uint8_t> part;
        const bool sweeps = std::any_of(n.voices.begin(), n.voices.end(),
                                        [this](const KeyVoice& kv) { return !instruments_.Voice(kv).sweeps.empty(); });
        if (!effect_ && sweeps)
        {
            part = BendPart(n, on_seconds, std::max(end_seconds + tail, faded));
            if (!part)
            {
                approximated.insert(kCommandsLeftOut);
            }
        }

        for (const std::string& a : approximated)
        {
            perf.approximations[a]++;
        }
        if (latest && earliest && *latest - *earliest > 0.001)
        {
            perf.approximations["layered voices that end at different times end together"]++;
        }

        const int64_t end = std::max(n.on, end_seconds == off_seconds ? off : TickAt(end_seconds));
        TrackEvent e{.kind = EventKind::kNoteOff, .key = n.key};
        e.note = n.id;
        e.part = part.value_or(0);
        Record(n.channel, end, e, end == n.on ? std::optional(Rank::kEmptyNoteOff) : std::nullopt);
        sound_end = std::max({sound_end, end_seconds + tail, faded});
        if (!effect_)
        {
            tails_.push_back({.channel = n.channel,
                              .part = e.part,
                              .on = on_seconds,
                              .off = end_seconds,
                              .silent = std::max(end_seconds + tail, faded),
                              .linear_ms = linear_ms,
                              .fades_to_release = fades_to_release});
        }
    }

    return sound_end;
}

std::optional<uint8_t> Player::BendPart(const NoteRecord& n, double on_seconds, double end_seconds)
{
    // A voice at the DSP's highest pitch plays there in its zone (Zone::highest_pitch), whatever its sweep.
    std::vector<PitchPoint> curve;
    for (const KeyVoice& kv : n.voices)
    {
        const MacroVoice& v = instruments_.Voice(kv);
        const Sample* sample = instruments_.SampleOf(v);
        if (!v.sweeps.empty() && sample != nullptr && !AtHighestPitch(v, *sample, kv.key))
        {
            curve = PitchCurve(v, *sample, kv.key, n.wheel, (end_seconds - on_seconds) * 1000.0);
            break;
        }
    }
    if (curve.empty() || !PitchMoves(curve, n.wheel))
    {
        return std::nullopt;
    }

    // The writer gives each part with notes a MIDI channel of its own before it gives any channel a layer.
    const int64_t on = n.on;
    const int64_t end = std::max(n.on, TickAt(end_seconds));
    std::set<std::pair<uint8_t, uint8_t>> parts;
    for (const BentNote& b : bent_)
    {
        parts.insert({b.channel, b.part});
    }
    std::size_t channels = parts.size();
    for (const std::vector<ChannelEvent>& events : events_)
    {
        channels += std::any_of(events.begin(), events.end(),
                                [](const ChannelEvent& ce) { return ce.event.kind == EventKind::kNoteOn; });
    }
    for (uint8_t part = 1; part < kChannels; part++)
    {
        const bool used = parts.contains({n.channel, part});
        if (!used && channels >= kChannels)
        {
            return std::nullopt;
        }
        const auto overlaps = [&](const BentNote& b)
        {
            return b.channel == n.channel && b.part == part && b.on < end && on < b.end;
        };
        const bool free = std::none_of(bent_.begin(), bent_.end(), overlaps);
        if (!free)
        {
            continue;
        }

        bent_.push_back(
            {.channel = n.channel, .part = part, .on = on, .end = end, .wheel = n.wheel, .curve = std::move(curve)});
        for (ChannelEvent& ce : events_[n.channel])
        {
            if (ce.event.kind == EventKind::kNoteOn && ce.event.note == n.id)
            {
                ce.event.part = part;
            }
        }
        return part;
    }

    return std::nullopt;
}

void Player::AddBentParts(uint8_t channel, std::vector<ChannelEvent>& events)
{
    std::set<uint8_t> parts;
    for (const BentNote& b : bent_)
    {
        if (b.channel == channel)
        {
            parts.insert(b.part);
        }
    }
    if (parts.empty())
    {
        return;
    }

    const auto earlier = [](const ChannelEvent& a, const ChannelEvent& b)
    {
        return std::tie(a.tick, a.rank, a.order) < std::tie(b.tick, b.rank, b.order);
    };
    std::sort(events.begin(), events.end(), earlier);
    std::vector<ChannelEvent> added;
    for (const uint8_t part : parts)
    {
        const auto inside = [&](int64_t tick)
        {
            const auto bends_at = [&](const BentNote& b)
            {
                return b.channel == channel && b.part == part && b.on <= tick && tick < b.end;
            };

            return std::any_of(bent_.begin(), bent_.end(), bends_at);
        };
        for (const ChannelEvent& ce : events)
        {
            const EventKind kind = ce.event.kind;
            if (ce.event.part != 0 || kind == EventKind::kNoteOn || kind == EventKind::kNoteOff ||
                (kind == EventKind::kPitch && inside(ce.tick)))
            {
                continue;
            }

            ChannelEvent copy = ce;
            copy.event.part = part;
            added.push_back(copy);
        }

        for (const BentNote& b : bent_)
        {
            if (b.channel != channel || b.part != part)
            {
                continue;
            }

            // The curve's points and the wheel's moves during the note, in order of their ticks. A point holds the
            // curve's bend, and a move the wheel's.
            struct Change
            {
                int64_t tick = 0;
                bool wheel = false;
                double semitones = 0.0;
            };
            std::vector<Change> changes;
            const double start = SecondsAt(b.on);
            for (const PitchPoint& p : b.curve)
            {
                const int64_t tick = p.ms == 0.0 ? b.on : std::min(TickAt(start + p.ms / 1000.0), b.end);
                changes.push_back({tick, false, p.semitones});
            }
            float wheel = 0.0f;
            for (const ChannelEvent& ce : events)
            {
                if (ce.event.part == 0 && ce.event.kind == EventKind::kPitch && ce.tick <= b.end)
                {
                    wheel = ce.event.semitones;
                    if (ce.tick > b.on && ce.tick < b.end)
                    {
                        changes.push_back({ce.tick, true, ce.event.semitones});
                    }
                }
            }
            std::stable_sort(changes.begin(), changes.end(),
                             [](const Change& x, const Change& y) { return x.tick < y.tick; });

            // MusyX adds the sweep to the DSP's pitch after the wheel. The wheel's move therefore scales the DSP's
            // pitch without the sweep.
            double curve = b.curve.front().semitones;
            double moved = b.wheel;
            for (const Change& change : changes)
            {
                (change.wheel ? moved : curve) = change.semitones;
                const double ratio =
                    std::pow(2.0, moved / 12.0) + std::pow(2.0, curve / 12.0) - std::pow(2.0, b.wheel / 12.0);
                TrackEvent bend{.kind = EventKind::kPitch};
                bend.semitones = static_cast<float>(12.0 * std::log2(std::max(ratio, 1.0 / 4096.0)));
                bend.part = part;
                added.push_back({change.tick, Rank::kSetting, order_++, bend});
            }

            // At the end, the part's bend comes back to the channel's.
            TrackEvent back{.kind = EventKind::kPitch};
            back.semitones = wheel;
            back.part = part;
            added.push_back({b.end, Rank::kSetting, order_++, back});
        }
    }
    events.insert(events.end(), added.begin(), added.end());
}

double Player::RiseMs(const MacroVoice& v, const KeyVoice& kv, double release_ms) const
{
    if (!v.envelope || v.envelope->attack_ms <= release_ms)
    {
        return 0.0;
    }

    const double attack = v.envelope->attack_ms;
    if (v.key_off != KeyOff::kFade)
    {
        return FadesThroughRelease(v) ? attack : 0.0;
    }

    const double volume = VoiceVolume(v, kv.volume, 1.0);
    const double full = instruments_.Level(v, volume);
    if (full <= 0.0)
    {
        return 0.0;
    }

    constexpr int kSteps = 64;
    const double last = std::min(attack, release_ms + v.release_fade.stop_ms);
    double peak = release_ms / attack;
    for (int i = 1; i < kSteps; i++)
    {
        const double ms = release_ms + (last - release_ms) * i / kSteps;
        const double level = instruments_.Level(v, FadeVolume(v.release_fade, volume, ms - release_ms).value());
        peak = std::max(peak, ms / attack * level / full);
    }
    if (const std::optional<double> faded = FadeVolume(v.release_fade, volume, last - release_ms))
    {
        peak = std::max(peak, last / attack * instruments_.Level(v, *faded) / full);
    }

    return peak * attack;
}

double Player::VoiceEnd(const MacroVoice& v, const KeyVoice& kv, double on_seconds, double off_seconds,
                        std::set<std::string>& approximated)
{
    // A voice whose held fade goes on through the release doesn't stop at its note's end. A voice whose envelope decays
    // to silence ends where DecayedMs says, after its note's end too if it ignores the release.
    const std::optional<double> decayed = DecayedMs(v);
    double end = off_seconds;
    if (v.key_off == KeyOff::kIgnored)
    {
        if (const std::optional<double> length = SampleSecondsOf(v, kv.key))
        {
            end = on_seconds + *length;
        }
        else if (decayed)
        {
            end = on_seconds + *decayed / 1000.0;
        }
        else if (!FadesThroughRelease(v))
        {
            approximated.insert("voices that loop and ignore the release stop at their note's end");
        }
    }
    end = std::max(end, on_seconds + RiseMs(v, kv, (off_seconds - on_seconds) * 1000.0) / 1000.0);
    if (v.stop_ms && (v.releases_itself || (v.key_off == KeyOff::kAsHeld && !FadesThroughRelease(v))))
    {
        end = on_seconds + *v.stop_ms / 1000.0;
    }
    if (v.stop_ms)
    {
        end = std::min(end, on_seconds + *v.stop_ms / 1000.0);
    }
    if (decayed)
    {
        end = std::min(end, on_seconds + *decayed / 1000.0);
    }

    return end;
}

std::optional<double> Player::SampleSecondsOf(const MacroVoice& v, int key)
{
    // The voice's key, moved by the macro, against the sample's. A rate that the macro sets takes the place of both.
    // An effect's voice follows the curve of its pitch.
    const Sample* sample = instruments_.GetSample(*v.sample);
    if (effect_ && sample != nullptr && sample->pcm.sample_rate > 0)
    {
        if (const std::optional<double> ms = CurveSampleMs(v, *sample, key))
        {
            return *ms / 1000.0;
        }
    }
    const int voice_key = v.fixed_key.value_or(key + v.key_shift);
    double ratio = sample != nullptr ? std::pow(2.0, (voice_key - sample->key) / 12.0 + v.detune_cents / 1200.0) : 1.0;
    if (sample != nullptr && v.rate_hz && *v.rate_hz > 0.0 && sample->pcm.sample_rate > 0)
    {
        ratio = *v.rate_hz / sample->pcm.sample_rate;
    }

    return instruments_.SampleSeconds(*v.sample, ratio);
}

std::optional<uint8_t> Player::NoteSend(const EffectNote& note, std::optional<double> MacroVoice::* send)
{
    std::optional<double> largest;
    for (const KeyVoice& kv : note.voices)
    {
        if (const std::optional<double>& value = instruments_.Voice(kv).*send)
        {
            largest = std::max(largest.value_or(0.0), *value);
        }
    }
    if (!largest)
    {
        return std::nullopt;
    }

    return static_cast<uint8_t>(std::lround(*largest * 127.0));
}

void Player::BendNote(const EffectNote& note, uint8_t channel, double start, double end)
{
    // The pitch wheel bends over the range of the note's first voice.
    const double wheel = Semitones(channel, wheel_[channel]);
    double tail = 0.0;
    for (const KeyVoice& kv : note.voices)
    {
        tail = std::max(tail, TailOf(instruments_.Voice(kv)));
    }
    const double ms = (end - start + tail) * 1000.0;

    std::vector<std::vector<PitchPoint>> curves;
    const std::vector<PitchPoint>* moving = nullptr;
    for (const KeyVoice& kv : note.voices)
    {
        const MacroVoice& v = instruments_.Voice(kv);
        if (const Sample* sample = instruments_.SampleOf(v))
        {
            curves.push_back(PitchCurve(v, *sample, kv.key, wheel, ms));
        }
    }
    for (const std::vector<PitchPoint>& curve : curves)
    {
        if (PitchMoves(curve, wheel))
        {
            moving = &curve;
            break;
        }
    }

    if (moving == nullptr)
    {
        if (wheel_[channel] != kWheelCentre)
        {
            TrackEvent bend{.kind = EventKind::kPitch};
            bend.semitones = static_cast<float>(wheel);
            Record(channel, 0, bend);
        }
        return;
    }
    if (std::any_of(curves.begin(), curves.end(), [moving](const auto& curve) { return curve != *moving; }))
    {
        approximations_["layered voices whose pitches move apart bend together"]++;
    }
    for (const PitchPoint& p : *moving)
    {
        TrackEvent bend{.kind = EventKind::kPitch};
        bend.semitones = static_cast<float>(p.semitones);
        Record(channel, p.ms == 0.0 ? 0 : TickAt(start + p.ms / 1000.0), bend);
    }
}

void Player::PanNote(const EffectNote& note, uint8_t channel, int pan, double start, double end)
{
    double tail = 0.0;
    for (const KeyVoice& kv : note.voices)
    {
        tail = std::max(tail, TailOf(instruments_.Voice(kv)));
    }
    const double ms = (end - start + tail) * 1000.0;

    std::vector<std::vector<PanPoint>> curves;
    for (const KeyVoice& kv : note.voices)
    {
        const MacroVoice& v = instruments_.Voice(kv);
        if (v.sample)
        {
            curves.push_back(PanCurve(v, ms));
        }
    }
    const auto moving = std::find_if(curves.begin(), curves.end(), [](const auto& curve) { return !curve.empty(); });
    if (moving == curves.end())
    {
        return;
    }
    if (std::any_of(curves.begin(), curves.end(), [&](const auto& curve) { return curve != *moving; }))
    {
        approximations_["layered voices whose pans move apart pan together"]++;
    }

    // MusyX adds the channel's distance from the centre to the voice's pan. It holds the sum within 0 to 127
    // (0x8014b788 in Pokémon Colosseum).
    for (const PanPoint& p : *moving)
    {
        const auto value = static_cast<uint8_t>(std::clamp<long>(std::lround(pan + p.offset - note.pan_shift), 0, 127));
        Controller(channel, 10, value, p.ms == 0.0 ? 0 : TickAt(start + p.ms / 1000.0));
    }
}

uint8_t Player::NoteReverb(const EffectNote& note, const GameEffect& effect)
{
    const uint8_t send = NoteSend(note, &MacroVoice::reverb_controller).value_or(effect.reverb);
    std::optional<double> largest;
    for (const KeyVoice& kv : note.voices)
    {
        const MacroVoice& v = instruments_.Voice(kv);
        if (!v.reverb_source)
        {
            if (v.reverb_controller)
            {
                largest = std::max(largest.value_or(0.0), *v.reverb_controller * kFullInput);
            }
            continue;
        }

        // A controller's 7 bits are the top 7 of its 14.
        const auto value14 = [](uint8_t value)
        {
            return value * 128.0;
        };
        double controller = 0.0;
        switch (v.reverb_source->controller)
        {
        case 7:
            controller = value14(effect.volume_controller);
            break;

        case 10:
            controller = value14(effect.pan_controller);
            break;

        case 91:
            controller = v.reverb_controller ? *v.reverb_controller * kFullInput : value14(effect.reverb);
            break;

        case 93:
            controller = v.chorus_controller ? *v.chorus_controller * kFullInput : value14(effect.chorus);
            break;

        default:
            if (!v.reverb_source->variable && v.reverb_source->controller != 1)
            {
                approximations_[kReverbSourcesLeftOut]++;
                continue;
            }
            break;
        }
        largest = std::max(largest.value_or(0.0), SourceValue(*v.reverb_source, controller));
    }
    if (!largest)
    {
        return send;
    }

    return static_cast<uint8_t>(std::lround(*largest / kFullInput * 127.0));
}

std::optional<double> Player::HeldSilence(const std::vector<KeyVoice>& voices)
{
    double silence = 0.0;
    for (const KeyVoice& kv : voices)
    {
        const MacroVoice& v = instruments_.Voice(kv);
        if (!v.sample)
        {
            continue;
        }

        std::optional<double> end;
        if (const std::optional<double> ms = HeldSilenceMs(v))
        {
            end = *ms / 1000.0;
        }
        if (const std::optional<double> length = SampleSecondsOf(v, kv.key))
        {
            end = std::min(end.value_or(*length), *length);
        }
        if (!end)
        {
            return std::nullopt;
        }

        silence = std::max(silence, *end);
    }

    return silence;
}

double Player::FadeEnd(const MacroVoice& v, int key, double on_seconds)
{
    double end = on_seconds + HeldSilenceMs(v).value() / 1000.0;
    if (const std::optional<double> length = SampleSecondsOf(v, key))
    {
        end = std::min(end, on_seconds + *length);
    }

    return end;
}

void Player::AddEvents(Performance& perf)
{
    // The events of each channel, in order, mapped to the voices' curves, without settings that repeat the last value.
    for (std::size_t c = 0; c < kChannels; c++)
    {
        std::vector<ChannelEvent>& events = events_[c];
        AddFadeSteps(events);
        AddBentParts(static_cast<uint8_t>(c), events);
        const auto earlier = [](const ChannelEvent& a, const ChannelEvent& b)
        {
            return std::tie(a.tick, a.rank, a.order) < std::tie(b.tick, b.rank, b.order);
        };
        std::sort(events.begin(), events.end(), earlier);

        // Each part of the channel keeps its last values. The parts take the same CC7 and CC11.
        std::map<std::pair<uint8_t, EventKind>, int> last;
        std::map<uint8_t, float> pitch;
        ChannelLevels levels;
        for (ChannelEvent& ce : events)
        {
            TrackEvent e = ce.event;
            e.tick = static_cast<uint32_t>(std::max<int64_t>(ce.tick, 0));
            MapLevel(c, e, levels);

            if (e.kind == EventKind::kPitch)
            {
                if (pitch[e.part] == e.semitones)
                {
                    continue;
                }
                pitch[e.part] = e.semitones;
            }
            else if (seqlib::midi::IsSetting(e.kind) || e.kind == EventKind::kPreset)
            {
                const int value = e.kind == EventKind::kPreset ? static_cast<int>(e.preset) : e.value;
                const auto it = last.find({e.part, e.kind});
                if (it != last.end() && it->second == value)
                {
                    continue;
                }
                last[{e.part, e.kind}] = value;
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
    // The main loop's passes start at the loop start and at each jump back.
    std::vector<int64_t> starts{LoopStart(master_)};
    starts.insert(starts.end(), jumps_.begin(), jumps_.end());
    const std::size_t passes = jumps_.size();

    // The fewest passes, from the first pass that can start them, that play as the passes after them do. Each section
    // that loops must stand at the same place in its loop at their end as at their start. A player that loops on them
    // otherwise jumps back to a state that the game doesn't play again.
    const ntdlib::sequence_export::LoopRules rules;
    const auto loops = static_cast<std::size_t>(std::max(options.loops, 1));
    std::optional<std::pair<std::size_t, std::size_t>> marked; // the first pass and the count
    for (std::size_t n = 1; !marked && 2 * n <= passes; n++)
    {
        for (std::size_t k = 0; k + 2 * n <= passes && k + n * loops <= passes; k++)
        {
            const int64_t s = starts[k];
            const int64_t e = starts[k + n];
            if (StateRepeats(s, e) && ntdlib::sequence_export::RepeatsAsTheGame(perf, {}, static_cast<uint32_t>(s),
                                                                                static_cast<uint32_t>(e), rules))
            {
                marked = {k, n};
                break;
            }
        }
    }

    // Those passes are the loop, played `options.loops` times. Without them, the performance plays until each section
    // that loops has looped as many times. Ending after as many passes of the main loop would end before a section
    // that starts its loop later has looped.
    int64_t last = marked ? starts[marked->first + marked->second * loops] : starts[std::min(loops, passes)];
    if (!marked)
    {
        for (std::size_t i = 0; i < kSections; i++)
        {
            const std::vector<int64_t>& section_loops = sections_[i].loops;
            if (looping_[i] && !section_loops.empty())
            {
                last = std::max(last, section_loops[std::min(loops, section_loops.size()) - 1]);
            }
        }
    }

    const auto end = static_cast<uint32_t>(last);
    if (marked)
    {
        const int64_t s = starts[marked->first];
        const int64_t e = starts[marked->first + marked->second];
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

std::optional<TailCut> Player::TailCutAt(uint8_t channel, const std::vector<const NoteTail*>& notes, double silent,
                                         const NoteTail& last) const
{
    // The parts on which a linear release of the files still sounds. The files fit it by its energy: it falls
    // kEnergyFitDb in the game's time, and kSoundFontFallDb in that time's kSoundFontFallDb / kEnergyFitDb. The release
    // of a fade that ends in a linear release reaches only 20 dB down when the game's voice does (FadeRate).
    TailCut cut{.channel = channel, .at = silent};
    for (const NoteTail* n : notes)
    {
        if ((n->linear_ms > 0.0 && silent - n->off < n->linear_ms / 1000.0 * kSoundFontFallDb / kEnergyFitDb) ||
            n->fades_to_release)
        {
            cut.parts.insert(n->part);
        }
    }
    if (cut.parts.empty())
    {
        return std::nullopt;
    }

    // A linear release that falls silent at `silent` gives the steps. The game's level falls from its start in a line,
    // 1 - u at the share u of the release. The files' level falls as 10^(-kEnergyFitDb u / 20). Past the point where
    // the files' level rises above the game's, at about u = 0.58, CC11 takes the share of the two. Any other note ends
    // with a short fade.
    const double step = kFaderStepMs / 1000.0;
    const double release = last.linear_ms / 1000.0;
    if (release > 0.0 && std::abs(last.off + release - silent) < 1e-9)
    {
        for (double t = last.off + step; t < silent - step / 2.0; t += step)
        {
            const double u = (t - last.off) / release;
            const double share = (1.0 - u) * std::pow(10.0, kEnergyFitDb * u / 20.0);
            if (share < 1.0)
            {
                cut.steps.emplace_back(t, share);
            }
        }
    }
    else
    {
        const auto count = static_cast<int>(std::lround(kShortCutSeconds / step));
        for (int i = 1; i < count; i++)
        {
            cut.steps.emplace_back(silent - kShortCutSeconds + i * step, 1.0 - static_cast<double>(i) / count);
        }
    }
    cut.steps.emplace_back(silent, 0.0);

    // A note that starts during the fade would fade too.
    const double from = cut.steps.front().first;
    if (std::any_of(notes.begin(), notes.end(), [from](const NoteTail* n) { return n->on >= from; }))
    {
        return std::nullopt;
    }

    return cut;
}

// A SoundFont plays a note's release to its end. The files fit a linear release by its energy (kEnergyFitDb). Their
// release therefore sounds on after the game's has ended, about 13 dB down. Once the game's voices of a channel have
// all fallen silent, and until the channel's next note, CC11 takes the files' tails down along the game's line and
// CC120 stops them. CC11 then goes back to its value. A tail under a later note of its channel sounds on.
//
// A cut inside the loop has to fit each pass: the one after the jump back starts from the end of the loop. Such a cut
// stays only where the run made the same cut one loop later. No cut spans a loop marker, and none comes after the end.
void Player::CutTails(Performance& perf) const
{
    std::vector<TailCut> cuts;
    for (uint8_t c = 0; c < kChannels; c++)
    {
        std::vector<const NoteTail*> notes;
        for (const NoteTail& n : tails_)
        {
            if (n.channel == c)
            {
                notes.push_back(&n);
            }
        }
        std::stable_sort(notes.begin(), notes.end(),
                         [](const NoteTail* a, const NoteTail* b) { return a->on < b->on; });

        // Where the voices of the notes so far have all fallen silent, and the note whose voices fall silent last. A
        // note with a linear release goes first among those that fall silent together. The channel's next note starts
        // a step or more after the cut. FluidSynth 2.6.1 undoes CC120 for a note-on in the same block of 64 samples.
        std::vector<const NoteTail*> before;
        const NoteTail* last = nullptr;
        const double step = kFaderStepMs / 1000.0;
        for (std::size_t j = 0; j <= notes.size(); j++)
        {
            if (last != nullptr && (j == notes.size() || (notes[j]->on - last->silent >= step &&
                                                          TickAt(last->silent) < TickAt(notes[j]->on))))
            {
                if (std::optional<TailCut> cut = TailCutAt(c, before, last->silent, *last))
                {
                    cuts.push_back(std::move(*cut));
                }
            }
            if (j < notes.size())
            {
                const NoteTail* n = notes[j];
                if (last == nullptr || n->silent > last->silent ||
                    (n->silent == last->silent && n->linear_ms > 0.0 && last->linear_ms <= 0.0))
                {
                    last = n;
                }
                before.push_back(n);
            }
        }
    }

    const auto spans = [](int64_t from, int64_t to, uint32_t marker)
    {
        return from <= marker && marker <= to;
    };
    // The steps of two such cuts can differ by one where they fall on the release.
    const auto same_cut = [this](const TailCut& a, const TailCut& b, int64_t ticks)
    {
        const int64_t moved = TickAt(a.at) + ticks - TickAt(b.at);
        const auto steps = static_cast<int64_t>(a.steps.size()) - static_cast<int64_t>(b.steps.size());
        return a.channel == b.channel && a.parts == b.parts && steps >= -1 && steps <= 1 && moved >= -1 && moved <= 1;
    };
    for (const TailCut& cut : cuts)
    {
        const int64_t from = TickAt(cut.steps.front().first);
        const int64_t at = TickAt(cut.at);
        if (at > perf.end_tick)
        {
            continue;
        }
        if (perf.loop)
        {
            const Performance::Loop loop = *perf.loop;
            if (spans(from, at, loop.start) || spans(from, at, loop.end))
            {
                continue;
            }
            const int64_t length = static_cast<int64_t>(loop.end) - loop.start;
            if (from > loop.start && at < loop.end &&
                std::none_of(
                    cuts.begin(), cuts.end(), [&](const TailCut& later) { return same_cut(cut, later, length); }))
            {
                continue;
            }
        }

        // CC11 mustn't move during the fade.
        std::vector<TrackEvent>& events = perf.tracks[cut.channel];
        const auto moves_cc11 = [&](const TrackEvent& e)
        {
            return (e.kind == EventKind::kExpression || e.kind == seqlib::midi::ControlKind(kResetControllers)) &&
                   e.tick >= from && e.tick <= at;
        };
        if (std::any_of(events.begin(), events.end(), moves_cc11))
        {
            continue;
        }

        for (const uint8_t part : cut.parts)
        {
            // The part's CC11 before the fade. A synth starts it at 127, and a reset of all controllers sets 127.
            int expression = 127;
            for (const TrackEvent& e : events)
            {
                if (e.part == part && e.tick < from)
                {
                    if (e.kind == EventKind::kExpression)
                    {
                        expression = e.value;
                    }
                    else if (e.kind == seqlib::midi::ControlKind(kResetControllers))
                    {
                        expression = 127;
                    }
                }
            }
            if (expression == 0)
            {
                continue;
            }

            std::vector<TrackEvent> added;
            int written = expression;
            const double amplitude = expression / 127.0 * (expression / 127.0);
            for (const auto& [seconds, share] : cut.steps)
            {
                const int value = SquaredValue(amplitude * share);
                if (value != written)
                {
                    added.push_back({.tick = static_cast<uint32_t>(TickAt(seconds)),
                                     .kind = EventKind::kExpression,
                                     .value = static_cast<uint8_t>(value),
                                     .part = part});
                    written = value;
                }
            }
            const auto tick = static_cast<uint32_t>(at);
            added.push_back({.tick = tick, .kind = EventKind::kSoundOff, .part = part});
            added.push_back({.tick = tick,
                             .kind = EventKind::kExpression,
                             .value = static_cast<uint8_t>(expression),
                             .part = part});

            // Each event goes after the events of its tick, before those between it and the next.
            for (const TrackEvent& a : added)
            {
                const auto after_a = [&a](const TrackEvent& e)
                {
                    return e.tick > a.tick || (e.tick == a.tick && e.after > 0.0);
                };
                events.insert(std::find_if(events.begin(), events.end(), after_a), a);
            }
        }
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

    // The passes that mark the loop: the loops asked for, and more to find one that repeats as the next. Each section
    // that loops loops as many times as asked for.
    const auto loops = static_cast<std::size_t>(std::max(options.loops, 1));
    player.Run(loops + kMorePasses + 1, loops);

    return player.Finish(options);
}

int EffectPan(const GameEffect& effect)
{
    return std::clamp(effect.pan.value_or(effect.row.pan) + effect.pan_controller - 64, 0, 127);
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

Performance PlayEffect(const GameEffect& effect, Instruments& instruments)
{
    // An effect has no loop. The performance ends when the effect's last note falls silent.
    Player player(effect, instruments);
    player.StartEffect(effect);
    player.Run(1);

    return player.Finish({});
}

} // namespace dolphinsf2::musyx
