// SPDX-License-Identifier: MIT

// The parts of MusyX's sequencer that musyx_player.cpp and musyx_player_output.cpp share: the constants, the
// sequencer's states and the class Player (see musyx_player.cpp).

#pragma once

#include <array>
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
#include "musyx_player.h"
#include "musyx_voices.h"
#include "output_units.h"

namespace dolphinsf2::musyx::player
{

using ntdlib::musyx::ChannelSetup;
using ntdlib::musyx::MidiSetup;
using ntdlib::musyx::Page;
using ntdlib::musyx::Pattern;
using ntdlib::musyx::PatternEvent;
using ntdlib::musyx::PatternEventKind;
using ntdlib::musyx::TrackEntry;
using xsflib::FormatError;

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

// MusyX's default range of the pitch wheel in semitones each way. A voice's macro can set another range.
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
    std::vector<int64_t> loops; // the performance's tick at the start of each of the section's passes after the first

    // Whether a track has looped since the clock last went back. The track may have looped in this run or an earlier
    // one.
    bool loop_due = false;
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

// Where the voices of a note end, in seconds, and what sounds after their ends (Player::EndsOf). `latest` and
// `earliest` are the latest and the earliest end of a voice that plays a sample. `tail` is the longest tail after a
// voice's end. `linear_ms` and `fades_to_release` are as in NoteTail.
struct VoiceEnds
{
    std::optional<double> latest;
    std::optional<double> earliest;
    double tail = 0.0;
    double faded = 0.0; // where the voices that go on with their held fades through the release fall silent
    double linear_ms = 0.0;
    bool fades_to_release = false;
    std::set<std::string> approximated; // the approximations of the voices
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

// The milliseconds of each step of MusyX's faders: the length of the DSP's frame (synthHandle), and the same in
// seconds.
constexpr double kFaderStepMs = 5.0;
constexpr double kFaderStepSeconds = kFaderStepMs / 1000.0;

// Where an event goes among a channel's events at one tick: note-offs come first, then settings, then note-ons. The
// note-off of a note that ends on the tick it starts comes last.
enum class Rank
{
    kNoteOff,
    kSetting,
    kNoteOn,
    kEmptyNoteOff,
};

// An event of a channel, before the events are put in order.
struct ChannelEvent
{
    int64_t tick = 0;
    Rank rank = Rank::kNoteOff;
    uint64_t order = 0;
    TrackEvent event;
};

// Whether event `a` of a channel comes before `b`: by tick, then by rank, then in the order in which they came.
inline bool EarlierEvent(const ChannelEvent& a, const ChannelEvent& b)
{
    return std::tie(a.tick, a.rank, a.order) < std::tie(b.tick, b.rank, b.order);
}

// The clock of an effect's performance: a song without tracks, at 156.25 beats a minute in 1024ths. A tick of it lasts
// a millisecond of MusyX's clock.
inline const ntdlib::musyx::Song& EffectClock()
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

    // Start the player's effect, and return its notes (EffectNotes). Note i plays on channel kEffectChannels[i] from
    // the note's start.
    std::vector<EffectNote> StartEffect(const GameEffect& effect);

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

    // The value from 0 to 127 that `channel` holds for `controller`, a controller as a macro reads it
    // (KeptController). The pitch wheel (0x80) gives its high 7 bits. The other controllers from 0x80 up keep their
    // values after a cold reset: 64 for controller 0x84, and 0 for the rest.
    uint8_t HeldController(uint8_t channel, uint8_t controller) const;

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

    // Add to `events`, the events of a channel, a setting of CC7 at each step of the song's fade-in after the
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

    // Where the voices of note `n` end, for a note that starts at `on_seconds` and whose note-off in the file comes at
    // `off_seconds`, and what sounds after their ends.
    VoiceEnds EndsOf(const NoteRecord& n, double on_seconds, double off_seconds);

    // The part of its channel on which `n`, a note of a song from `on_seconds` to `end_seconds`, bends along the curve
    // of its first voice whose pitch sweeps, or nothing. A MIDI channel bends all of its notes. The note therefore goes
    // on a part of the channel where no other bent note sounds, while MIDI's 16 channels last. The note's note-on goes
    // on that part.
    std::optional<uint8_t> BendPart(const NoteRecord& n, double on_seconds, double end_seconds);

    // Add to `events`, the events of channel `channel`, the events of the parts of its bent notes. Each such part takes
    // every setting of the channel and the channel's bends outside its notes. During a note, the part bends along the
    // note's curve, moved by the pitch wheel's moves since the note's start.
    void AddBentParts(uint8_t channel, std::vector<ChannelEvent>& events);

    // Add to `added` the bends of the part of bent note `b`, from `events`, the events of its channel in order.
    void AddBends(const BentNote& b, const std::vector<ChannelEvent>& events, std::vector<ChannelEvent>& added);

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

    // The milliseconds from `start` to `end` seconds of `note`, a note of an effect, with the longest tail of its
    // voices (TailOf).
    double NoteMs(const EffectNote& note, double start, double end);

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

    // The cuts of the tails of each channel in turn, in order of their times (TailCutAt).
    std::vector<TailCut> TailCuts() const;

    // Whether `perf` takes `cut`, one of the cuts `cuts` (see the comment of CutTails). No cut ends after the end of
    // `perf` or spans a loop marker, and CC11 doesn't move during a cut.
    bool KeepsCut(const Performance& perf, const std::vector<TailCut>& cuts, const TailCut& cut) const;

    // Add to `events`, the events of the channel of `cut`, the fade and the stop of part `part`.
    void CutPart(std::vector<TrackEvent>& events, const TailCut& cut, uint8_t part) const;

    const ntdlib::musyx::Song& song_;
    const SongGroup& group_;
    Instruments& instruments_;
    uint8_t song_volume_ = 127;   // the volume of the song in the game's song table, or the group volume of an effect
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

} // namespace dolphinsf2::musyx::player
