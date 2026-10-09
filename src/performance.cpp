// SPDX-License-Identifier: MIT

// Sequence execution (see performance.h). The performer follows JAudio2's sequence player as Four Swords Adventures'
// main.dol links it. Its parser's command table is Twilight Princess's without 0xe7 (built at 0x80491e54), and the
// player's classes are Twilight Princess's: JASSeqParser, JASSeqCtrl, JASSeqReader, JASTrack, JASRegisterParam,
// JASTrackPort, JASChannel, JASOscillator, JASLfo and JASDSPChannel. Each sub-frame of 80 samples, the channels update
// first (JASDSPChannel::updateAll), then the sequence runs its ticks (JASTrack::seqMain).

#include "performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "binary.h"
#include "envelope.h"
#include "instruments.h"
#include "wave.h"

namespace dolphinsf2
{

namespace
{

// Commands a track may run in one tick. JAudio2 parses until a command waits. A track that loops without waiting hangs
// the game. The performer stops such a track instead.
constexpr int kParseLimit = 100000;

// Repeat short loops, such as sustained sound effects, until the performance reaches this duration. Keep loop markers
// around the first pass.
constexpr double kMinLoopSeconds = 10.0;

// Longest performance, in seconds of music.
constexpr double kMaxSeconds = 900.0;

// Maximum duration after a held sound stops changing. This covers tracks waiting on endless looping notes.
constexpr double kHoldSeconds = 10.0;

// Shortest wait that counts as waiting for ever: 200 beats at the default timebase, minutes at a typical tempo. Games
// wait that long for sounds that they stop themselves (revosf2 and WSF use the same limit).
constexpr int32_t kHeldTicks = 9600;

// Longest release tail kept after the end.
constexpr double kMaxTailSeconds = 10.0;

// The DSP program's channels, each a voice (JASDSPChannel).
constexpr int kDspChannels = 64;

// A track's channel slots (TChannelMgr::CHANNEL_MAX), its children, and the depth of its call stack.
constexpr int kNoteIds = 8;
constexpr int kChildren = 16;
constexpr int kStackSize = 8;

// A sequence that opens more tracks than this is damaged.
constexpr std::size_t kMaxTracks = 512;

// A vibrato's counter moves on by this much each sub-frame (JASChannel::updateDSPChannel, with 32028.5 at 0x8053b090).
constexpr float kLfoStep = 32028.5f / kDacRate;

// The vibrato depth that CC1's full value gives, in cents.
constexpr double kVibratoRangeCents = 100.0;

// A note quieter than this, in dB, can't be heard. A released note that quiet no longer counts as sounding when a track
// looks for a free MIDI channel. A tie on a note whose envelope has fallen that far raises nothing in MIDI.
constexpr double kInaudibleDb = -40.0;

// The command table at 0x80491e54: each command's argument count and their encodings, two bits each, the first argument
// lowest: 0 a byte, 1 16 bits, 2 24 bits, 3 a register's value (its number as a byte).
struct CommandInfo
{
    uint8_t count = 0;
    uint16_t types = 0;
};

constexpr std::array<CommandInfo, 96> MakeCommands()
{
    std::array<CommandInfo, 96> c{};
    auto set = [&](int cmd, uint8_t count, uint16_t types)
    {
        c[static_cast<std::size_t>(cmd - 0xa0)] = {count, types};
    };
    set(0xb1, 3, 0x0000); // noteon: key, note id, velocity
    set(0xb2, 1, 0x0000); // noteoff
    set(0xb3, 4, 0x0040); // note: flags, key, velocity, length
    set(0xb4, 1, 0x0000); // set the latest key
    set(0xb8, 2, 0x0000); // parameter, a byte value
    set(0xb9, 2, 0x0004); // parameter, a 16-bit value
    set(0xba, 3, 0x0010); // parameter, a byte value, a time
    set(0xbb, 3, 0x0014); // parameter, a 16-bit value, a time
    set(0xc1, 2, 0x0008); // open a track
    set(0xc2, 1, 0x0000); // close a track
    set(0xc3, 1, 0x0002); // call
    set(0xc4, 2, 0x0008); // call if
    set(0xc5, 0, 0x0000); // return
    set(0xc6, 1, 0x0000); // return if
    set(0xc7, 1, 0x0002); // jump
    set(0xc8, 2, 0x0008); // jump if
    set(0xc9, 2, 0x000b); // jump through a table
    set(0xca, 2, 0x000b); // call through a table
    set(0xcb, 1, 0x0001); // loop start
    set(0xcc, 0, 0x0000); // loop end
    set(0xd0, 2, 0x0000); // read a port
    set(0xd1, 2, 0x000c); // write a port
    set(0xd2, 1, 0x0000); // check a port's import
    set(0xd3, 1, 0x0000); // check a port's export
    set(0xd4, 2, 0x000c); // write the parent's port
    set(0xd5, 2, 0x000c); // write a child's port
    set(0xd6, 2, 0x0000); // read the parent's port
    set(0xd7, 2, 0x0000); // read a child's port
    set(0xd8, 2, 0x0004); // load a register
    set(0xd9, 3, 0x0030); // register arithmetic with a register
    set(0xda, 3, 0x0010); // register arithmetic with a value
    set(0xdb, 2, 0x0000); // negate a register
    set(0xdc, 4, 0x00e0); // load a register from a table
    set(0xe0, 1, 0x0001); // tempo
    set(0xe1, 1, 0x0001); // bank and program
    set(0xe2, 1, 0x0000); // bank
    set(0xe3, 1, 0x0000); // program
    set(0xe8, 2, 0x0008); // an oscillator's table
    set(0xe9, 5, 0x0155); // a simple ADSR
    set(0xea, 2, 0x0004); // bus connection
    set(0xeb, 1, 0x0000); // IIR cutoff
    set(0xec, 4, 0x0055); // IIR coefficients
    set(0xed, 1, 0x0002); // FIR coefficients
    set(0xf0, 0, 0x0000); // wait, a variable-length time
    set(0xf1, 1, 0x0000); // wait, a byte
    set(0xf3, 1, 0x0002); // interrupt table
    set(0xf4, 1, 0x0001); // enable interrupts
    set(0xf5, 1, 0x0001); // disable interrupts
    set(0xf6, 0, 0x0000); // return from an interrupt
    set(0xf7, 0, 0x0000); // clear the interrupt
    set(0xf8, 2, 0x0004); // interrupt timer
    set(0xf9, 1, 0x0001); // synchronise with the game
    set(0xfd, 0, 0x0000); // print
    set(0xfe, 0, 0x0000); // nothing
    set(0xff, 0, 0x0000); // finish

    return c;
}

constexpr std::array<CommandInfo, 96> kCommands = MakeCommands();

// The interrupts a track can take (JASSeqCtrl::IntrType): pause, unpause, a write to port 0 or 1, its timer, a tick.
constexpr int kIntrPort0 = 2;
constexpr int kIntrPort1 = 3;
constexpr int kIntrTimer = 5;
constexpr int kIntrTick = 6;

// The game's fast random generator (JMath::TRandom_fast_).
class Random
{
public:
    explicit Random(uint32_t seed) : state_(seed)
    {
    }

    uint32_t Next()
    {
        state_ = state_ * 0x19660du + 0x3c6ef35fu;

        return state_;
    }

private:
    uint32_t state_;
};

// A track parameter that moves linearly to a target over a number of ticks (JASTrack::MoveParam_).
struct MoveParam
{
    float value = 0.0f;
    float target = 0.0f;
    uint32_t count = 0;
};

// The settings a track gives its channels (JASChannelParams): volume and pitch multiply down the tracks, and pan,
// effect send and the bend, in octaves, add.
struct ChannelParams
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fx_mix = 0.0f;
    float bend = 0.0f;
};

// A channel's vibrato or tremolo (JASLfo): a sine of `depth`, `pitch` 72nds of a cycle per sub-frame, after a delay
// in sub-frames.
class Lfo
{
public:
    void Set(float depth, float pitch)
    {
        depth_ = depth;
        pitch_ = pitch;
    }

    void SetDelay(uint16_t delay)
    {
        delay_ = delay;
    }

    // Start again from the start of the cycle, after the delay (resetCounter).
    void Reset()
    {
        wait_ = delay_;
        phase_ = 0.0f;
    }

    void Advance(float step)
    {
        if (wait_ > 0)
        {
            wait_--;
            return;
        }

        phase_ += step * ((1.0f / 72.0f) * pitch_);
        if (phase_ >= 1.0f)
        {
            phase_ -= 1.0f;
        }
    }

    float Value() const
    {
        if (wait_ > 0 || depth_ == 0.0f)
        {
            return 0.0f;
        }

        return depth_ * std::sin(2.0f * 3.14159265f * phase_);
    }

private:
    float depth_ = 0.0f;
    float pitch_ = 1.0f;
    uint16_t delay_ = 0;
    uint16_t wait_ = 0;
    float phase_ = 0.0f;
};

// A JASChannel: a voice playing one note, and the MIDI note that stands for it.
struct Channel
{
    // The game's state.
    int track = -1;   // the track that gives it settings, or -1 once the track has detached it
    int note_id = -1; // its slot in the track
    NoteSound sound;
    uint8_t key = 0; // the key the track played, transposed
    uint8_t velocity = 0;
    int base_key = 60;       // the key that plays the wave at its rate: the wave's base key, or a drum's key
    float key_offset = 0.0f; // semitones from the base key (mKey)
    float key_sweep = 0.0f;  // how far a sweep has moved, in semitones
    float sweep_target = 0.0f;
    uint32_t sweep_count = 0; // sub-frames left in the sweep
    float init_pitch = 1.0f;  // the instrument's pitch times the wave's rate over the output rate
    std::array<const Oscillator*, 2> osc_data{};
    std::array<OscillatorState, 2> osc{};
    Lfo vibrato, tremolo;
    uint32_t update_timer = 0; // sub-frames until a gate note's release
    uint16_t direct_release = 0;
    bool started = false; // has had its first sub-frame
    bool released = false;
    bool ended = false;
    bool pitch_moved = false; // an oscillator has moved its pitch
    ChannelParams params;
    uint8_t priority = 0x40;
    uint8_t release_priority = 0;
    uint32_t age = 0; // sub-frames since it started (JASDSPChannel's update counter)
    double position = 0.0;
    uint32_t wave_length = 0; // for a wave that doesn't loop: its samples
    bool wave_loops = true;
    float level = 0.0f;          // its volume in the last sub-frame
    float envelope = 0.0f;       // its oscillators' volume in the last sub-frame
    bool envelope_heard = false; // that volume has been above kInaudibleDb

    // The MIDI note.
    int slot = -1; // its MIDI track
    bool midi_on = false;
    uint32_t midi_id = 0;
    uint8_t part = 0;
    uint32_t preset = 0;
    uint32_t on_tick = 0;
    float midi_key_offset = 0.0f; // key_offset when the MIDI note started
    uint8_t midi_velocity = 0;    // the MIDI note's velocity
    uint8_t cc11_velocity = 0;    // the velocity that CC11 follows (GateOn)
    bool bends = false;           // pitch bends and CC11 follow the note's sweeps and tied notes
};

// The ticks in a sub-frame at a tempo, timebase and tempo rate (JASTrack::updateTempo, 0x80037804). 80 samples a
// sub-frame make 4/3 of a 60th of a second's ticks per thousand samples (4/3 at 0x8053b274).
float TickRate(uint16_t tempo, uint16_t timebase, float tempo_rate)
{
    return tempo_rate * (static_cast<float>(timebase) * static_cast<float>(tempo) * (4.0f / 3.0f) / kDacRate);
}

// True if `volume`, an amplitude, is below kInaudibleDb.
bool Inaudible(float volume)
{
    return 20.0 * std::log10(std::max(static_cast<double>(volume), 1e-10)) <= kInaudibleDb;
}

// True if `ch`'s envelope has fallen below hearing. An attack that starts there hasn't. A tie changes the note's
// velocity but not its envelope.
bool BelowHearing(const Channel& ch)
{
    return ch.envelope_heard && Inaudible(ch.envelope);
}

// Start `semitones` of sweep on `ch` over `subframes` (JASChannel::setKeySweepTarget, 0x8002ae78). A sweep of no
// sub-frames moves at once.
void SetSweep(Channel& ch, float semitones, uint32_t subframes)
{
    ch.sweep_count = subframes;
    if (subframes == 0)
    {
        ch.key_sweep = semitones;
    }
    else
    {
        ch.key_sweep = 0.0f;
        ch.sweep_target = semitones;
    }
}

// JASSeqParser::execNoteOnGate (0x80035050) turns a gate note's flags into JASTrack::gateOn's. Bit 7 sweeps from the
// latest key (1), bit 6 ties the next note to this one (2), and a length of 0 waits for the note to end (4).
uint32_t GateFlags(uint8_t flags, uint32_t length)
{
    return ((flags & 0x80) ? 1u : 0u) | ((flags & 0x40) ? 2u : 0u) | (length == 0 ? 4u : 0u);
}

// A JASTrack with its sequence control (JASSeqCtrl) and reader (JASSeqReader).
struct Track
{
    // Allocation.
    bool open = false;
    int parent = -1;
    std::array<int, kChildren> children{};
    int slot = -1; // the MIDI track it records into
    std::string name;

    // The reader.
    uint32_t pc = 0;
    std::array<uint32_t, kStackSize> return_to{};
    std::array<uint16_t, kStackSize> loop_count{};
    int depth = 0;
    int32_t wait = 0;
    bool wait_note_finish = false;

    // Interrupts.
    uint32_t intr_table = 0;
    uint16_t intr_mask = 0;
    uint16_t intr_pending = 0;
    std::optional<uint32_t> intr_return;
    uint8_t timer_repeats = 0;
    uint32_t timer = 0;
    uint32_t timer_reload = 0;

    // Registers and ports.
    std::array<uint16_t, 14> regs{};
    std::array<uint16_t, 16> ports{};
    uint16_t imported = 0, exported = 0;

    // Settings.
    std::array<MoveParam, 6> move{};
    std::array<std::optional<Oscillator>, 2> osc{};
    uint16_t bank = 0;
    uint16_t program = 0xf0;
    int8_t transpose = 0;
    uint8_t bend_sense = 12;
    uint8_t gate_rate = 100;
    uint8_t note_on_prio = 0x40;
    uint8_t release_prio = 0;
    uint32_t skip_samples = 0;
    uint16_t direct_release = 0;
    float pan_power = 1.0f;
    float vib_depth = 0.0f, vib_pitch = 1.0f, trem_depth = 0.0f, trem_pitch = 1.0f;
    uint16_t vib_delay = 0, trem_delay = 0;
    uint8_t latest_key = 60;
    float tick_rate = TickRate(120, 48, 1.0f); // the tick rate that gate notes use (JASTrack +0x1dc)
    bool tied = false;                         // the next gate note continues the last (flag4)
    bool dirty = true;                         // settings to recompute (flag5)
    bool iir = false;                          // an IIR filter other than the open one
    std::array<int, kNoteIds> slots{};
    ChannelParams params;

    // MIDI state.
    uint8_t part = 0;
    std::map<std::tuple<uint8_t, EventKind>, int> sent; // each part's last controller values
    std::map<uint8_t, float> sent_pitch;
    std::optional<uint32_t> last_change;
    std::optional<uint64_t> last_signal; // when it last wrote another track's port, in commands run
    bool idle = false;                   // its last pass of a loop changed nothing
    uint32_t opened = 0;                 // the tick it last opened in
    std::optional<uint32_t> last_wait;
    std::map<uint32_t, std::pair<uint32_t, uint64_t>> visited; // each command's last tick, and the commands run then
};

// A command run's outcome.
enum class Outcome
{
    kContinue,
    kWait,
    kFinish,
};

// Execute a sequence sub-frame by sub-frame, following JAudio2, and record its output.
class Performer
{
public:
    // What MarkSteadyPass chose: whether the performance stops, the pass of the main loop it marked, if any, and
    // whether the performance plays one more pass to mark it.
    struct LoopChoice
    {
        bool stop = false;
        std::optional<std::size_t> marked;
        bool extend = false;
    };

    // Multiply CC11 by `level_scale` (LevelPeak). Give each MIDI track at most `part_limits` parts. Make the main
    // loop's choices in `loop_choices`, from an earlier run (LoopChoices), and then decide.
    Performer(const SequenceSound& sound, BankSet& banks, const PerformOptions& options, double level_scale,
              const std::array<uint8_t, 16>& part_limits, const std::vector<LoopChoice>& loop_choices,
              const std::map<uint32_t, uint8_t>& tie_velocities)
        : sound_(sound),
          banks_(banks),
          options_(options),
          rng_(options.seed),
          replay_(loop_choices),
          level_scale_(level_scale),
          part_limits_(part_limits),
          tie_velocities_in_(tie_velocities)
    {
    }

    Performance Run();

    // Highest CC11 value before `level_scale` and MIDI clamping.
    double LevelPeak() const
    {
        return level_peak_;
    }

    // The parts each MIDI track used: one more than the highest.
    const std::array<uint8_t, 16>& PartsUsed() const
    {
        return parts_used_;
    }

    // The choices MarkSteadyPass made for the main loop, for a later run to make again.
    const std::vector<LoopChoice>& LoopChoices() const
    {
        return choices_;
    }

    // The highest velocity that ties gave each MIDI note above its velocity, by MIDI note ID, for a later run to start
    // the note at.
    const std::map<uint32_t, uint8_t>& TieVelocities() const
    {
        return tie_velocities_;
    }

private:
    // A setting of a MIDI track's part: the MIDI track, the part and the kind of event.
    using SettingKey = std::tuple<int, uint8_t, EventKind>;

    // How a pass of the main loop starts: each part's settings and the tempo after the commands of its first tick, and
    // the part each MIDI track's new notes go to.
    struct PassStart
    {
        std::map<SettingKey, TrackEvent> settings;
        double bpm = 120.0;
        std::array<uint8_t, 16> parts{};
    };

    // A pass of a loop that the sub-frame's end decides about: the track, and the ticks it started and ended in.
    struct PendingLoop
    {
        int track = 0;
        uint32_t start = 0;
        uint32_t end = 0;
    };

    // --- Tracks ---

    int NewTrack(int parent, int number);
    void CloseTrack(int t);
    int OpenChild(int t, uint32_t number);
    void CloseChild(int t, uint32_t number);
    int TickProc(int t);
    int SeqTick(int t);
    Outcome Parse(int t);
    Outcome Command(int t, uint8_t cmd, uint16_t types);
    uint8_t ReadByte(int t);
    uint16_t Read16(int t);
    uint32_t Read24(int t);
    uint32_t ReadMidiValue(int t);
    uint32_t Data8(uint32_t offset) const;
    uint32_t Data16(uint32_t offset) const;
    uint32_t Data24(uint32_t offset) const;
    uint32_t Data32(uint32_t offset) const;
    void Jump(int t, uint32_t target);
    bool Call(int t, uint32_t target);
    bool Return(int t);
    bool Condition(int t, uint32_t condition);
    uint32_t ReadReg(int t, uint32_t reg);
    void WriteReg(int t, uint32_t reg, uint32_t value);
    void WriteRegister(int t, uint32_t reg, uint32_t value);
    uint32_t ReadRegister(int t, uint32_t reg);
    void Interrupt(int t, int type);
    void CheckInterrupt(int t);
    void TimerProcess(int t);
    void SetParam(int t, uint32_t target, float value, uint32_t count);
    void UpdateTimedParams(int t);
    void UpdateSeq(int t, bool force, float pan_scale);
    void UpdateTrack(int t, float pan_scale);
    float TicksPerSubFrame() const;
    int TransposeTotal(int t) const;

    // Run one sub-frame: the channels, then the ticks that are due. Return false when the first track finishes.
    bool Advance();

    // --- Channels ---

    std::optional<std::size_t> StartChannel(int t, int key, int velocity, uint32_t timer, float sweep = 0.0f,
                                            uint32_t sweep_subframes = 0, bool bends = false);
    void NoteOn(int t, uint32_t note_id, int key, int velocity);
    void GateOn(int t, int key, int velocity, float time, uint32_t flags);
    void NoteOff(int t, uint32_t note_id, uint16_t release);
    void ReleaseChannel(std::size_t c, uint16_t direct_release);
    void Detach(std::size_t c);
    void EndChannel(std::size_t c);
    void UpdateChannels();
    void UpdateChannel(Channel& ch);
    std::optional<std::size_t> AllocateVoice(uint8_t priority);
    float ChannelVolume(const Channel& ch, float osc_volume) const;
    float ChannelRatio(const Channel& ch, float osc_pitch) const;

    // --- Loops and ends ---

    // Handle track `t` jumping back to `target`: a pass of a loop when it's a jump back to commands that ran before
    // a wait. Detect the main loop, and ask the sub-frame's end to decide whether to stop.
    void OnJumpBack(int t, uint32_t target);

    // Decide about the pending loop at the end of a sub-frame: mark its steady pass and stop, or play on.
    void DecideLoop();

    // True if open track `t` won't read on before the game stops the sound: it waits for a note that never ends, or
    // waits kHeldTicks or more.
    bool WaitsForever(int t) const;

    // True if track `t` sounds nothing, and kHoldSeconds have passed since its last change or since it opened.
    bool Quiet(int t) const;

    // True if no open track will read on before the game stops the sound and nothing sounds: the sequence has ended.
    bool Stuck() const;

    // True if every open track waits for ever and none of their settings moves.
    bool Steady() const;

    // The time to end at if the main loop, which started at tick `start`, holds a sound unchanged: its pass is longer
    // than kHoldSeconds, nothing has changed for longer than that, and every sounding note is an endless one that its
    // track hasn't released. The performance ends kHoldSeconds after the last change.
    std::optional<double> HeldSoundEnd(uint32_t start) const;

    // The settings of each part after the commands of `tick`, from `perf`'s events.
    static std::map<SettingKey, TrackEvent> SettingsAt(const Performance& perf, uint32_t tick);

    // How a pass that starts at `tick` starts.
    PassStart PassStartAt(uint32_t tick) const;

    // A copy of this performer that plays on up to tick `until`, with no limit on the loops.
    Performer PlayOn(uint32_t until) const;

    // True if events `a` and `b` give a setting the same value.
    static bool SameSetting(const TrackEvent& a, const TrackEvent& b);

    // True if a player that loops on the pass from `start` to `end` plays each repeat's notes as the game plays the
    // notes of the next pass: the same notes at the same ticks, with the same settings and tempo (revosf2's rule).
    bool RepeatsAsTheGame(uint32_t start, uint32_t end) const;

    // Select the first of the loop owner's passes that plays as the pass after it does.
    LoopChoice ChooseSteadyPass() const;

    // Mark the pass that ChooseSteadyPass selects. Return true if the performance can stop.
    bool MarkSteadyPass();

    // Send at the main loop's start each setting, and the tempo, that the game gives back its value at the loop's start
    // in `next_pass`. A player that loops on the markers otherwise keeps the loop end's value.
    void RestoreAtLoopStart(const PassStart& next_pass);

    // --- Recording ---

    int SlotOf(int t);
    void Emit(int t, TrackEvent e, int part = -1);
    void EmitTempo();
    void RecordOutputs(int t);
    void SendSetting(int t, EventKind kind, int value);
    void SendPitch(int t, float semitones);
    float GatePitch(int t) const;
    bool GateBends(int t) const;
    void SweepBetweenTicks();
    void SwitchPart(int t);
    void MidiNoteOn(std::size_t c);
    void MidiNoteOff(std::size_t c);
    bool Audible(const Channel& ch) const;
    bool TailsOnPart(int slot, uint8_t part) const;
    int PresetIndex(const PresetKey& key);
    void Approximate(const std::string& what);
    void RecordTickTime();
    double TrackLevel(int t) const;
    bool AnySounding() const;
    bool TrackSounds(int t) const;
    void Changed(int t);
    double TickSeconds(uint32_t tick) const;
    void EndPerformance(bool finished);
    uint32_t SecondsToTick(double seconds) const;

    const SequenceSound& sound_;
    BankSet& banks_;
    PerformOptions options_;
    Random rng_;
    Performance out_;

    std::vector<Track> tracks_;
    std::vector<Channel> channels_;
    std::array<int, kDspChannels> voices_{}; // channel index per voice, -1 free

    float accumulator_ = 1.0f; // ticks due (JASTrack's field_0x1d8)
    uint16_t tempo_ = 120;
    uint16_t timebase_ = 48;
    float tempo_rate_ = 1.0f;
    uint64_t subframe_ = 0;
    uint32_t tick_ = 0;
    bool ticked_ = false; // the first tick has run
    std::vector<double> tick_seconds_;
    std::vector<uint64_t> tick_subframes_;
    double sound_end_ = 0.0;     // when the last channel stopped
    uint64_t last_sounding_ = 0; // the last sub-frame in which a channel sounded
    uint64_t commands_run_ = 0;  // by all tracks
    uint32_t next_midi_id_ = 1;
    bool stop_ = false;
    bool silent_end_ = false; // the main loop's pass was idle and silent: the sequence has ended
    std::optional<PendingLoop> pending_loop_;
    std::optional<uint32_t> stop_tick_;                       // the end of the pass that stopped the performance
    std::optional<std::pair<int, uint32_t>> loop_owner_;      // the track and loop start of the marked loop
    std::vector<std::pair<uint32_t, uint32_t>> owner_passes_; // the owner's counted passes, start and end ticks
    std::size_t marked_ = 0;                                  // the marked one of them
    bool extended_ = false;                                   // the performance plays one more pass to mark it
    std::optional<double> held_sound_end_;                    // set when the main loop holds a sound (HeldSoundEnd)
    std::optional<uint32_t> last_draw_;                       // the last tick a track drew a random number in
    std::vector<std::tuple<uint32_t, int, uint8_t>> part_switches_; // tick, MIDI track and new part of each switch
    std::vector<LoopChoice> replay_;                                // the choices of an earlier run to make again
    std::size_t replayed_ = 0;                                      // how many of them have been made
    std::vector<LoopChoice> choices_;                               // the choices made
    double level_scale_ = 1.0;
    double level_peak_ = 0.0;
    std::array<uint8_t, 16> part_limits_{};
    std::array<uint8_t, 16> parts_used_{};
    std::array<int, 16> slot_tracks_{};             // the track recorded in each MIDI track, or -1
    std::map<uint32_t, uint8_t> tie_velocities_in_; // an earlier run's TieVelocities
    std::map<uint32_t, uint8_t> tie_velocities_;
};

// --- Tracks ---

int Performer::NewTrack(int parent, int number)
{
    if (tracks_.size() >= kMaxTracks)
    {
        throw FormatError("sequence opens too many tracks");
    }

    Track tr;
    tr.parent = parent;
    tr.children.fill(-1);
    tr.slots.fill(-1);
    tr.move[0].value = 1.0f; // volume
    tr.move[3].value = 0.5f; // pan
    for (MoveParam& m : tr.move)
    {
        m.target = m.value;
    }
    tr.name = parent < 0 ? "Root"
                         : (tracks_[static_cast<std::size_t>(parent)].parent < 0
                                ? "Track "
                                : tracks_[static_cast<std::size_t>(parent)].name + ".") +
                               std::to_string(number);
    tracks_.push_back(std::move(tr));

    return static_cast<int>(tracks_.size() - 1);
}

int Performer::OpenChild(int t, uint32_t number)
{
    // JASTrack::openChild: a running child is closed and starts again; the new child inherits its parent's bank,
    // program, priorities, skip, filters and bus connections.
    Track& parent = tracks_[static_cast<std::size_t>(t)];
    int child = parent.children[number];
    if (child >= 0 && tracks_[static_cast<std::size_t>(child)].open)
    {
        UpdateSeq(0, false, 1.0f);
        CloseTrack(child);
    }
    if (child < 0)
    {
        child = NewTrack(t, static_cast<int>(number));
        tracks_[static_cast<std::size_t>(t)].children[number] = child;
    }
    else
    {
        // Start afresh. Keep the MIDI track it records into.
        Track& c = tracks_[static_cast<std::size_t>(child)];
        Track fresh;
        fresh.parent = t;
        fresh.children = c.children;
        fresh.slots.fill(-1);
        fresh.move[0].value = 1.0f;
        fresh.move[3].value = 0.5f;
        for (MoveParam& m : fresh.move)
        {
            m.target = m.value;
        }
        fresh.slot = c.slot;
        fresh.name = c.name;
        fresh.part = c.part;
        fresh.sent = c.sent;
        fresh.sent_pitch = c.sent_pitch;
        fresh.visited = c.visited;
        c = std::move(fresh);
    }

    Track& c = tracks_[static_cast<std::size_t>(child)];
    const Track& p = tracks_[static_cast<std::size_t>(t)];
    c.bank = p.bank;
    c.program = p.program;
    c.note_on_prio = p.note_on_prio;
    c.release_prio = p.release_prio;
    c.skip_samples = p.skip_samples;
    c.iir = p.iir;
    c.opened = tick_;

    return child;
}

void Performer::CloseTrack(int t)
{
    // JASTrack::close: close the children, and release the track's channels.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    for (int child : tr.children)
    {
        if (child >= 0 && tracks_[static_cast<std::size_t>(child)].open)
        {
            CloseTrack(child);
        }
    }
    for (int& slot : tracks_[static_cast<std::size_t>(t)].slots)
    {
        if (slot >= 0)
        {
            ReleaseChannel(static_cast<std::size_t>(slot), 0);
            Detach(static_cast<std::size_t>(slot));
            slot = -1;
        }
    }
    tracks_[static_cast<std::size_t>(t)].open = false;
}

void Performer::CloseChild(int t, uint32_t number)
{
    const int child = tracks_[static_cast<std::size_t>(t)].children[number];
    if (child >= 0 && tracks_[static_cast<std::size_t>(child)].open)
    {
        UpdateSeq(0, false, 1.0f);
        CloseTrack(child);
    }
}

int Performer::TickProc(int t)
{
    // JASTrack::tickProc: run the track's tick, move its timed parameters, then its children's ticks.
    const int ret = SeqTick(t);
    UpdateTimedParams(t);
    tracks_[static_cast<std::size_t>(t)].dirty = true;
    if (ret < 0)
    {
        return -1;
    }

    for (int i = 0; i < kChildren && !stop_; i++)
    {
        const int child = tracks_[static_cast<std::size_t>(t)].children[static_cast<std::size_t>(i)];
        if (child >= 0 && tracks_[static_cast<std::size_t>(child)].open && TickProc(child) < 0)
        {
            UpdateSeq(0, false, 1.0f);
            CloseTrack(child);
        }
    }

    return 0;
}

int Performer::SeqTick(int t)
{
    // JASSeqCtrl::tickProc.
    Interrupt(t, kIntrTick);
    TimerProcess(t);
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.wait_note_finish)
    {
        if (tr.slots[0] >= 0)
        {
            return 0;
        }
        tr.wait_note_finish = false;
    }
    if (tr.wait > 0)
    {
        tr.wait--;
    }
    CheckInterrupt(t);

    int commands = 0;
    while ((tracks_[static_cast<std::size_t>(t)].wait == 0 || tracks_[static_cast<std::size_t>(t)].intr_return) &&
           !tracks_[static_cast<std::size_t>(t)].wait_note_finish && !stop_)
    {
        if (++commands > kParseLimit)
        {
            out_.warnings.push_back(tracks_[static_cast<std::size_t>(t)].name +
                                    " runs without waiting. It would hang the game. dolphinsf2 stopped it");
            return -1;
        }

        const Outcome outcome = Parse(t);
        if (outcome == Outcome::kFinish)
        {
            return -1;
        }
    }

    // A wait for a note to end is a wait too, as far as loops go.
    const Track& after = tracks_[static_cast<std::size_t>(t)];
    if (after.wait > 0 || after.wait_note_finish)
    {
        tracks_[static_cast<std::size_t>(t)].last_wait = tick_;
    }

    return 0;
}

uint32_t Performer::Data8(uint32_t offset) const
{
    return offset < sound_.data.size() ? sound_.data[offset] : 0xff;
}

uint32_t Performer::Data16(uint32_t offset) const
{
    return Data8(offset) << 8 | Data8(offset + 1);
}

uint32_t Performer::Data24(uint32_t offset) const
{
    return Data8(offset) << 16 | Data8(offset + 1) << 8 | Data8(offset + 2);
}

uint32_t Performer::Data32(uint32_t offset) const
{
    return Data16(offset) << 16 | Data16(offset + 2);
}

uint8_t Performer::ReadByte(int t)
{
    // Past the end of the data, read 0xff: finish.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    const auto b = static_cast<uint8_t>(Data8(tr.pc));
    tr.pc++;

    return b;
}

uint16_t Performer::Read16(int t)
{
    const uint32_t hi = ReadByte(t);
    return static_cast<uint16_t>(hi << 8 | ReadByte(t));
}

uint32_t Performer::Read24(int t)
{
    const uint32_t hi = Read16(t);
    return hi << 8 | ReadByte(t);
}

uint32_t Performer::ReadMidiValue(int t)
{
    // JASSeqReader::readMidiValue: up to four bytes of seven bits; a longer value reads as 0.
    uint32_t value = ReadByte(t);
    if (!(value & 0x80))
    {
        return value;
    }

    value &= 0x7f;
    for (int i = 0;; i++)
    {
        if (i > 2)
        {
            return 0;
        }

        const uint8_t b = ReadByte(t);
        value = value << 7 | (b & 0x7f);
        if (!(b & 0x80))
        {
            return value;
        }
    }
}

void Performer::Jump(int t, uint32_t target)
{
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (target <= tr.pc)
    {
        OnJumpBack(t, target);
    }
    tracks_[static_cast<std::size_t>(t)].pc = target;
}

bool Performer::Call(int t, uint32_t target)
{
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.depth >= kStackSize)
    {
        return false;
    }

    tr.return_to[static_cast<std::size_t>(tr.depth++)] = tr.pc;
    tr.pc = target;

    return true;
}

bool Performer::Return(int t)
{
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.depth == 0)
    {
        return false;
    }

    tr.pc = tr.return_to[static_cast<std::size_t>(--tr.depth)];

    return true;
}

bool Performer::Condition(int t, uint32_t condition)
{
    // JASSeqParser::conditionCheck, on register 3.
    const uint32_t reg = ReadReg(t, 3);
    switch (condition)
    {
    case 0:
        return true;
    case 1:
        return reg == 0;
    case 2:
        return reg != 0;
    case 3:
        return reg == 1;
    case 4:
        return reg >= 0x8000;
    case 5:
        return reg < 0x8000;
    default:
        return false;
    }
}

uint32_t Performer::ReadRegister(int t, uint32_t reg)
{
    // JASRegisterParam::read: 14 registers, and pairs of them read together.
    const std::array<uint16_t, 14>& r = tracks_[static_cast<std::size_t>(t)].regs;
    if (reg < 14)
    {
        return r[reg];
    }

    switch (reg)
    {
    case 32:
        return (r[0] & 0xff) << 8 | (r[1] & 0xff);
    case 33:
        return static_cast<uint32_t>(r[4]) << 16 | r[5];
    case 34:
        return r[4] >> 8;
    case 35:
        return r[4] & 0xff;
    case 36:
        return r[5] >> 8;
    case 37:
        return r[5] & 0xff;
    case 38:
        return static_cast<uint32_t>(r[6]) << 16 | r[7];
    case 39:
        return static_cast<uint32_t>(r[8]) << 16 | r[9];
    case 40:
    case 41: // the game reads 40's registers for 41 too
        return static_cast<uint32_t>(r[10]) << 16 | r[11];
    default:
        return 0;
    }
}

void Performer::WriteRegister(int t, uint32_t reg, uint32_t value)
{
    std::array<uint16_t, 14>& r = tracks_[static_cast<std::size_t>(t)].regs;
    if (reg < 14)
    {
        r[reg] = static_cast<uint16_t>(reg <= 2 ? (value & 0xff) : (value & 0xffff));
        return;
    }

    switch (reg)
    {
    case 32:
        r[0] = (value >> 8) & 0xff;
        r[1] = value & 0xff;
        break;
    case 33:
        r[4] = static_cast<uint16_t>(value >> 16);
        r[5] = static_cast<uint16_t>(value);
        break;
    case 34:
        r[4] = static_cast<uint16_t>((r[4] & 0xff) | (value & 0xff) << 8);
        break;
    case 35:
        r[4] = static_cast<uint16_t>((r[4] & 0xff00) | (value & 0xff));
        break;
    case 36:
        r[5] = static_cast<uint16_t>((r[5] & 0xff) | (value & 0xff) << 8);
        break;
    case 37:
        r[5] = static_cast<uint16_t>((r[5] & 0xff00) | (value & 0xff));
        break;
    case 38:
        r[6] = static_cast<uint16_t>(value >> 16);
        r[7] = static_cast<uint16_t>(value);
        break;
    case 39:
        r[8] = static_cast<uint16_t>(value >> 16);
        r[9] = static_cast<uint16_t>(value);
        break;
    case 40:
        r[10] = static_cast<uint16_t>(value >> 16);
        r[11] = static_cast<uint16_t>(value);
        break;
    case 41:
        r[12] = static_cast<uint16_t>(value >> 16);
        r[13] = static_cast<uint16_t>(value);
        break;
    default:
        break;
    }
}

uint32_t Performer::ReadReg(int t, uint32_t reg)
{
    // JASSeqParser::readReg: the registers, the ports from 0x40, and the track's settings from 0x60.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (reg < 0x40)
    {
        return ReadRegister(t, reg);
    }
    if (reg < 0x50)
    {
        return tr.ports[reg - 0x40];
    }

    switch (reg)
    {
    case 0x60:
        {
            uint32_t running = 0;
            for (int i = 0; i < kChildren; i++)
            {
                const int child = tr.children[static_cast<std::size_t>(i)];
                if (child >= 0 && tracks_[static_cast<std::size_t>(child)].open)
                {
                    running |= 1u << i;
                }
            }
            return running;
        }
    case 0x61:
        return tr.depth > 0 ? tr.loop_count[static_cast<std::size_t>(tr.depth - 1)] : 0;
    case 0x62:
        return timebase_;
    case 0x63:
        return static_cast<uint32_t>(static_cast<int32_t>(tr.transpose));
    case 0x64:
        return tr.bend_sense;
    case 0x65:
        return tr.gate_rate;
    case 0x66:
        return tr.skip_samples;
    case 0x67:
        return tr.bank;
    case 0x68:
        return tr.program;
    case 0x69:
        return static_cast<uint32_t>(static_cast<int32_t>(tr.pan_power * 32767.0f));
    case 0x6a:
        return tr.release_prio | tr.note_on_prio; // the game forgets to shift the release priority
    case 0x6b:
        return tr.note_on_prio;
    case 0x6c:
        return tr.release_prio;
    case 0x6d:
        return tr.direct_release;
    case 0x6e:
        return static_cast<uint32_t>(tr.vib_depth * 1524.0f);
    case 0x6f:
        return static_cast<uint32_t>(tr.vib_depth * 12192.0f);
    case 0x70:
        return static_cast<uint32_t>(tr.trem_depth * 256.0f);
    case 0x71:
        return static_cast<uint32_t>(tr.vib_pitch * 64.0f);
    case 0x72:
        return static_cast<uint32_t>(tr.trem_pitch * 64.0f);
    case 0x73:
        return tr.vib_delay;
    case 0x74:
        return tr.trem_delay;
    default:
        return 0;
    }
}

void Performer::WriteReg(int t, uint32_t reg, uint32_t value)
{
    // JASSeqParser::writeReg: every write also sets register 3, the flag the conditions test.
    WriteRegister(t, 3, value);
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (reg < 0x40)
    {
        WriteRegister(t, reg, value);
        return;
    }
    if (reg < 0x50)
    {
        tr.ports[reg - 0x40] = static_cast<uint16_t>(value);
        return;
    }

    switch (reg)
    {
    case 0x62:
        // Only the first track's timebase counts (JASTrack::setTimebase asserts it).
        if (tr.parent < 0)
        {
            timebase_ = static_cast<uint16_t>(value);
            tr.dirty = true;
            EmitTempo();
        }
        break;
    case 0x63:
        tr.transpose = static_cast<int8_t>(value);
        break;
    case 0x64:
        tr.bend_sense = static_cast<uint8_t>(value);
        break;
    case 0x65:
        tr.gate_rate = static_cast<uint8_t>(value);
        break;
    case 0x66:
        tr.skip_samples = value;
        break;
    case 0x67:
        tr.bank = static_cast<uint8_t>(value);
        break;
    case 0x68:
        tr.program = static_cast<uint8_t>(value);
        break;
    case 0x69:
        tr.pan_power = static_cast<float>(value) / 32767.0f;
        break;
    case 0x6a:
        tr.release_prio = static_cast<uint8_t>(value >> 8);
        tr.note_on_prio = static_cast<uint8_t>(value);
        break;
    case 0x6b:
        tr.note_on_prio = static_cast<uint8_t>(value);
        break;
    case 0x6c:
        tr.release_prio = static_cast<uint8_t>(value);
        break;
    case 0x6d:
        tr.direct_release = static_cast<uint16_t>(value);
        break;
    case 0x6e:
        tr.vib_depth = static_cast<float>(value) / 1524.0f;
        break;
    case 0x6f:
        tr.vib_depth = static_cast<float>(value) / 12192.0f;
        break;
    case 0x70:
        tr.trem_depth = static_cast<float>(value) / 256.0f;
        break;
    case 0x71:
        tr.vib_pitch = static_cast<float>(value) / 64.0f;
        break;
    case 0x72:
        tr.trem_pitch = static_cast<float>(value) / 64.0f;
        break;
    case 0x73:
        tr.vib_delay = static_cast<uint16_t>(value);
        break;
    case 0x74:
        tr.trem_delay = static_cast<uint16_t>(value);
        break;
    default:
        break;
    }
}

void Performer::Interrupt(int t, int type)
{
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.intr_mask & (1u << type))
    {
        tr.intr_pending = static_cast<uint16_t>(tr.intr_pending | (1u << type));
    }
}

void Performer::CheckInterrupt(int t)
{
    // JASSeqCtrl::checkIntr: take the lowest pending interrupt that the mask enables. Do nothing while one is running.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.intr_return)
    {
        return;
    }

    const uint16_t ready = tr.intr_mask & tr.intr_pending;
    for (int i = 0; i < 16; i++)
    {
        if (ready & (1u << i))
        {
            tr.intr_pending = static_cast<uint16_t>(tr.intr_pending & ~(1u << i));
            tr.intr_return = tr.pc;
            tr.pc = Data24(tr.intr_table + 3 * static_cast<uint32_t>(i));
            return;
        }
    }
}

void Performer::TimerProcess(int t)
{
    // JASSeqCtrl::timerProcess: the timer raises its interrupt when it runs out, and starts again for as many repeats
    // as it has left, or for ever with none.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.timer == 0 || --tr.timer != 0)
    {
        return;
    }

    Interrupt(t, kIntrTimer);
    if (tr.timer_repeats != 0)
    {
        tr.timer_repeats--;
        if (tr.timer_repeats != 0)
        {
            tr.timer = tr.timer_reload;
        }
    }
    else
    {
        tr.timer = tr.timer_reload;
    }
}

void Performer::SetParam(int t, uint32_t target, float value, uint32_t count)
{
    // JASTrack::setParam: move to the value over `count` ticks, or at once.
    if (target >= 6)
    {
        return;
    }

    MoveParam& m = tracks_[static_cast<std::size_t>(t)].move[target];
    if (m.target != value || m.value != value)
    {
        Changed(t);
    }
    m.target = value;
    if (count == 0)
    {
        m.value = value;
    }
    m.count = count;
}

void Performer::UpdateTimedParams(int t)
{
    for (MoveParam& m : tracks_[static_cast<std::size_t>(t)].move)
    {
        if (m.count != 0)
        {
            m.value += (m.target - m.value) / static_cast<float>(m.count);
            m.count--;
        }
    }
}

void Performer::UpdateSeq(int t, bool force, float pan_scale)
{
    // JASTrack::updateSeq: recompute the settings of the tracks that changed, and of all their descendants.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (!force)
    {
        force = tr.dirty;
    }
    tr.dirty = false;
    if (force)
    {
        UpdateTrack(t, pan_scale);
    }

    const float child_scale = pan_scale * tracks_[static_cast<std::size_t>(t)].pan_power;
    for (int i = 0; i < kChildren; i++)
    {
        const int child = tracks_[static_cast<std::size_t>(t)].children[static_cast<std::size_t>(i)];
        if (child >= 0 && tracks_[static_cast<std::size_t>(child)].open)
        {
            UpdateSeq(child, force, child_scale);
        }
    }
}

void Performer::UpdateTrack(int t, float pan_scale)
{
    // JASTrack::updateTrack: the track's volume squared and its bend (bend sense / 3 octaves at full range, with 1/3 at
    // 0x8053b270) combine with its parent's settings. The sound table's volume comes in at the first track.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    const float volume = tr.move[0].value * tr.move[0].value;
    const float bend = tr.move[1].value * static_cast<float>(tr.bend_sense) * (1.0f / 3.0f);
    const float pan = (tr.move[3].value - 0.5f) * tr.pan_power * pan_scale;
    ChannelParams p;
    if (tr.parent < 0)
    {
        p.volume = volume * sound_.volume;
        p.pitch = 1.0f;
        p.pan = pan;
        p.fx_mix = tr.move[2].value;
        p.bend = bend;
    }
    else
    {
        const ChannelParams& up = tracks_[static_cast<std::size_t>(tr.parent)].params;
        p.volume = up.volume * volume;
        p.pitch = up.pitch;
        p.pan = (up.pan - 0.5f) + pan;
        p.fx_mix = up.fx_mix + tr.move[2].value;
        p.bend = up.bend + bend;
    }
    p.pan += 0.5f;
    tr.params = p;

    // updateTempo (0x80037804): the first track works out its tick rate. A child copies its parent's. setTempo and
    // setTimebase only store their values.
    tr.tick_rate = tr.parent < 0 ? TicksPerSubFrame() : tracks_[static_cast<std::size_t>(tr.parent)].tick_rate;
}

float Performer::TicksPerSubFrame() const
{
    return TickRate(tempo_, timebase_, tempo_rate_);
}

int Performer::TransposeTotal(int t) const
{
    int total = 0;
    for (; t >= 0; t = tracks_[static_cast<std::size_t>(t)].parent)
    {
        total += tracks_[static_cast<std::size_t>(t)].transpose;
    }

    return total;
}

Outcome Performer::Parse(int t)
{
    // JASSeqParser::parse.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    tr.visited[tr.pc] = {tick_, commands_run_++};
    const uint8_t b = ReadByte(t);
    if (!(b & 0x80))
    {
        // A note: flags (the note id in the low three bits), velocity, and a gate note's length.
        const uint8_t flags = ReadByte(t);
        const uint8_t velocity = ReadByte(t);
        if ((flags & 7) == 0)
        {
            const uint32_t length = ReadMidiValue(t);
            GateOn(t, b, velocity, static_cast<float>(length), GateFlags(flags, length));
            Track& again = tracks_[static_cast<std::size_t>(t)];
            if (length != 0)
            {
                again.wait = static_cast<int32_t>(length);
                return Outcome::kWait;
            }
            again.wait_note_finish = true;
            return Outcome::kWait;
        }

        NoteOn(t, flags & 7, b, velocity);
        return Outcome::kContinue;
    }

    switch (b & 0xf0)
    {
    case 0x80:
        if (b & 7)
        {
            NoteOff(t, b & 7, 0);
        }
        return Outcome::kContinue;

    case 0x90:
        {
            // A register prefix: the next command reads the flagged arguments from registers.
            const int count = (b & 7) + 1;
            uint8_t mask = ReadByte(t);
            uint16_t types = 0;
            uint16_t bits = 3;
            for (int i = 0; i < count; i++)
            {
                if (mask & 0x80)
                {
                    types = static_cast<uint16_t>(types | bits);
                }
                mask = static_cast<uint8_t>(mask << 1);
                bits = static_cast<uint16_t>(bits << 2);
            }
            const uint8_t cmd = ReadByte(t);
            return Command(t, cmd, types);
        }

    default:
        return Command(t, b, 0);
    }
}

Outcome Performer::Command(int t, uint8_t cmd, uint16_t override_types)
{
    // JASSeqParser::parseCommand: read the arguments the command table gives, then run the command.
    if (cmd == 0xb0)
    {
        ReadByte(t); // an extended command: only a debugging dump
        return Outcome::kContinue;
    }
    if (cmd < 0xa0)
    {
        return Outcome::kContinue;
    }

    const CommandInfo& info = kCommands[cmd - 0xa0u];
    uint16_t types = info.types | override_types;
    std::array<uint32_t, 8> a{};
    for (int i = 0; i < info.count; i++, types = static_cast<uint16_t>(types >> 2))
    {
        switch (types & 3)
        {
        case 0:
            a[static_cast<std::size_t>(i)] = ReadByte(t);
            break;
        case 1:
            a[static_cast<std::size_t>(i)] = Read16(t);
            break;
        case 2:
            a[static_cast<std::size_t>(i)] = Read24(t);
            break;
        default:
            a[static_cast<std::size_t>(i)] = ReadReg(t, ReadByte(t));
            break;
        }
    }

    Track& tr = tracks_[static_cast<std::size_t>(t)];
    switch (cmd)
    {
    case 0xb1:
        NoteOn(t, a[1], static_cast<int>(a[0]), static_cast<int>(a[2]));
        break;
    case 0xb2:
        NoteOff(t, a[0], 0);
        break;
    case 0xb3:
        GateOn(t, static_cast<int>(a[1]), static_cast<int>(a[2]), static_cast<float>(a[3]),
               GateFlags(static_cast<uint8_t>(a[0]), a[3]));
        if (a[3] != 0)
        {
            tracks_[static_cast<std::size_t>(t)].wait = static_cast<int32_t>(a[3]);
        }
        else
        {
            tracks_[static_cast<std::size_t>(t)].wait_note_finish = true;
        }
        return Outcome::kWait;
    case 0xb4:
        if (a[0] < 256)
        {
            tr.latest_key = static_cast<uint8_t>(a[0] + TransposeTotal(t));
        }
        break;
    case 0xb8:
    case 0xba:
        {
            // A byte value spread over 16 bits: below 0x80, its low bits repeat.
            const auto byte = static_cast<int32_t>(a[1]);
            auto value = static_cast<int16_t>(byte << 8);
            if (!(byte & 0x80))
            {
                value = static_cast<int16_t>(value | static_cast<int16_t>(byte << 1));
            }
            SetParam(t, a[0], static_cast<float>(value) / 32767.0f, cmd == 0xba ? a[2] : 0);
            break;
        }
    case 0xb9:
    case 0xbb:
        SetParam(t, a[0], static_cast<float>(static_cast<int16_t>(a[1])) / 32767.0f, cmd == 0xbb ? a[2] : 0);
        break;
    case 0xc1:
        {
            if (a[0] >= kChildren)
            {
                break;
            }
            const int child = OpenChild(t, a[0]);
            Track& c = tracks_[static_cast<std::size_t>(child)];
            c.pc = a[1];
            c.open = true;
            c.dirty = true;
            break;
        }
    case 0xc2:
        if (a[0] < kChildren)
        {
            CloseChild(t, a[0]);
        }
        break;
    case 0xc3:
        Call(t, a[0]);
        break;
    case 0xc4:
        if (Condition(t, a[0]))
        {
            Call(t, a[1]);
        }
        break;
    case 0xc5:
        Return(t);
        break;
    case 0xc6:
        if (Condition(t, a[0]))
        {
            Return(t);
        }
        break;
    case 0xc7:
        Jump(t, a[0]);
        break;
    case 0xc8:
        if (Condition(t, a[0]))
        {
            Jump(t, a[1]);
        }
        break;
    case 0xc9:
        Jump(t, Data24(a[1] + 3 * a[0]));
        break;
    case 0xca:
        Call(t, Data24(a[1] + 3 * a[0]));
        break;
    case 0xcb:
        // JASSeqReader::loopStart: the count goes on the call stack with the return address.
        if (tr.depth < kStackSize)
        {
            tr.return_to[static_cast<std::size_t>(tr.depth)] = tr.pc;
            tr.loop_count[static_cast<std::size_t>(tr.depth)] = static_cast<uint16_t>(a[0]);
            tr.depth++;
        }
        break;
    case 0xcc:
        {
            // JASSeqReader::loopEnd: a count of 0 or 1 plays the loop once.
            if (tr.depth == 0)
            {
                break;
            }
            uint16_t& count = tr.loop_count[static_cast<std::size_t>(tr.depth - 1)];
            const uint16_t left = count != 0 ? static_cast<uint16_t>(count - 1) : 0;
            if (left == 0)
            {
                tr.depth--;
                break;
            }
            count = left;
            tr.pc = tr.return_to[static_cast<std::size_t>(tr.depth - 1)];
            break;
        }
    case 0xd0:
        tr.imported = static_cast<uint16_t>(tr.imported & ~(1u << (a[0] & 15)));
        WriteReg(t, a[1], tr.ports[a[0] & 15]);
        break;
    case 0xd1:
        tr.exported = static_cast<uint16_t>(tr.exported | (1u << (a[0] & 15)));
        tr.ports[a[0] & 15] = static_cast<uint16_t>(a[1]);
        break;
    case 0xd2:
        WriteRegister(t, 3, (tr.imported >> (a[0] & 15)) & 1);
        break;
    case 0xd3:
        WriteRegister(t, 3, (tr.exported >> (a[0] & 15)) & 1);
        break;
    case 0xd4:
    case 0xd5:
        {
            // A write to another track's port. For ports 0 and 1, it raises that track's port interrupt.
            int other = -1;
            uint32_t port = a[0] & 15;
            if (cmd == 0xd4)
            {
                other = tr.parent;
            }
            else
            {
                other = tr.children[(a[0] >> 4) & 15];
            }
            if (other < 0)
            {
                break;
            }
            tracks_[static_cast<std::size_t>(t)].last_signal = commands_run_;
            Track& o = tracks_[static_cast<std::size_t>(other)];
            o.imported = static_cast<uint16_t>(o.imported | (1u << port));
            o.ports[port] = static_cast<uint16_t>(a[1]);
            if (port <= 1)
            {
                Interrupt(other, port == 0 ? kIntrPort0 : kIntrPort1);
            }
            break;
        }
    case 0xd6:
    case 0xd7:
        {
            int other = -1;
            uint32_t port = a[0] & 15;
            if (cmd == 0xd6)
            {
                other = tr.parent;
            }
            else
            {
                other = tr.children[(a[0] >> 4) & 15];
            }
            if (other < 0)
            {
                break;
            }
            Track& o = tracks_[static_cast<std::size_t>(other)];
            o.exported = static_cast<uint16_t>(o.exported & ~(1u << port));
            WriteReg(t, a[1], o.ports[port]);
            break;
        }
    case 0xd8:
        WriteReg(t, a[0], a[1]);
        break;
    case 0xd9:
    case 0xda:
        {
            // JASSeqParser::cmdReg: arithmetic on a register, with a value or another register's value.
            uint32_t reg = a[1];
            uint32_t v = a[2];
            switch (a[0])
            {
            case 0:
                break;
            case 1:
                v += ReadReg(t, reg);
                break;
            case 2:
                v = ReadReg(t, reg) - v;
                break;
            case 3:
                v = ReadReg(t, reg) - v;
                reg = 3;
                break;
            case 4:
                v *= ReadReg(t, reg);
                reg = 0x21;
                break;
            case 5:
                v &= ReadReg(t, reg);
                break;
            case 6:
                v |= ReadReg(t, reg);
                break;
            case 7:
                v ^= ReadReg(t, reg);
                break;
            case 8:
                last_draw_ = tick_;
                v = v != 0 ? (rng_.Next() >> 9) % v : 0;
                break;
            case 9:
                v = ReadReg(t, reg) << (v & 31);
                break;
            case 10:
                v = ReadReg(t, reg) >> (v & 31);
                break;
            default:
                return Outcome::kContinue;
            }
            WriteReg(t, reg, v);
            break;
        }
    case 0xdb:
        if (a[0] == 11)
        {
            WriteReg(t, a[1], static_cast<uint32_t>(-static_cast<int32_t>(ReadReg(t, a[1]))));
        }
        break;
    case 0xdc:
        {
            uint32_t v = 0;
            switch (a[0])
            {
            case 12:
                v = Data8(a[2] + a[3]);
                break;
            case 13:
                v = Data16(a[2] + 2 * a[3]);
                break;
            case 14:
                v = Data24(a[2] + 3 * a[3]);
                break;
            case 15:
                v = Data32(a[2] + 4 * a[3]);
                break;
            case 16:
                v = Data32(a[2] + a[3]);
                break;
            default:
                return Outcome::kContinue;
            }
            WriteReg(t, a[1], v);
            break;
        }
    case 0xe0:
        // Only the first track's tempo counts (JASTrack::setTempo asserts it).
        if (tr.parent < 0)
        {
            tempo_ = static_cast<uint16_t>(a[0]);
            tr.dirty = true;
            EmitTempo();
        }
        break;
    case 0xe1:
        tr.bank = static_cast<uint16_t>((a[0] >> 8) & 0xff);
        tr.program = static_cast<uint16_t>(a[0] & 0xff);
        break;
    case 0xe2:
        tr.bank = static_cast<uint16_t>(a[0] & 0xff);
        break;
    case 0xe3:
        tr.program = static_cast<uint16_t>(a[0] & 0xff);
        break;
    case 0xe8:
        {
            // JASTrack::setOscTable: the track's table for oscillator 0 (volume) or 1 (pitch), from the sequence.
            if (a[0] >= 2)
            {
                break;
            }
            Oscillator osc;
            osc.target = static_cast<uint8_t>(a[0]);
            for (uint32_t at = a[1], n = 0; n < 0x10000; at += 6, n++)
            {
                const OscPoint p{static_cast<int16_t>(Data16(at)), static_cast<int16_t>(Data16(at + 2)),
                                 static_cast<int16_t>(Data16(at + 4))};
                osc.attack.push_back(p);
                if (p.mode > 10)
                {
                    break;
                }
            }
            if (a[0] == 0 && tr.osc[0])
            {
                osc.release = tr.osc[0]->release; // osc 0 keeps its release table
            }
            tr.osc[a[0]] = std::move(osc);
            break;
        }
    case 0xe9:
        {
            // JASTrack::setOscAdsr: oscillator 0 becomes the track's four-point table (sAdsTable) with these times
            // and sustain, and the direct release.
            Oscillator osc;
            osc.target = 0;
            osc.attack = {{0, static_cast<int16_t>(a[0]), 0x7fff},
                          {0, static_cast<int16_t>(a[1]), 0x7fff},
                          {0, static_cast<int16_t>(a[2]), static_cast<int16_t>(a[3])},
                          {0xe, 0, 0}};
            tr.osc[0] = std::move(osc);
            tr.direct_release = static_cast<uint16_t>(a[4]);
            break;
        }
    case 0xea:
    case 0xed:
        break; // bus connections and FIR filters: the effects that the game sets up
    case 0xeb:
        tr.iir = a[0] < 127;
        break;
    case 0xec:
        tr.iir = !(static_cast<int16_t>(a[0]) == 0x7fff && a[1] == 0 && a[2] == 0 && a[3] == 0);
        break;
    case 0xf0:
        tr.wait = static_cast<int32_t>(ReadMidiValue(t));
        return tr.wait != 0 ? Outcome::kWait : Outcome::kContinue;
    case 0xf1:
        tr.wait = static_cast<int32_t>(a[0]);
        return tr.wait != 0 ? Outcome::kWait : Outcome::kContinue;
    case 0xf3:
        tr.intr_table = a[0];
        break;
    case 0xf4:
        tr.intr_mask = static_cast<uint16_t>(tr.intr_mask | a[0]);
        break;
    case 0xf5:
        tr.intr_mask = static_cast<uint16_t>(tr.intr_mask & ~a[0]);
        break;
    case 0xf6:
        if (tr.intr_return)
        {
            tr.pc = *tr.intr_return;
            tr.intr_return.reset();
        }
        CheckInterrupt(t);
        break;
    case 0xf7:
        tr.intr_return.reset();
        CheckInterrupt(t);
        break;
    case 0xf8:
        tr.timer_repeats = static_cast<uint8_t>(a[0]);
        tr.timer = a[1];
        tr.timer_reload = a[1];
        break;
    case 0xf9:
        // A game can answer through a callback. Four Swords Adventures registers none. The command's handler
        // (0x80034654) then writes 0xffff to the flag register.
        WriteRegister(t, 3, 0xffff);
        break;
    case 0xfd:
        {
            // A debugging print: its text, then a byte for each % in it.
            int args = 0;
            for (int i = 0; i < 128; i++)
            {
                const uint8_t c = ReadByte(t);
                if (c == 0)
                {
                    break;
                }
                if (c == '\\')
                {
                    if (ReadByte(t) == 0)
                    {
                        break;
                    }
                }
                else if (c == '%')
                {
                    if (ReadByte(t) == 0)
                    {
                        break;
                    }
                    args++;
                }
            }
            for (int i = 0; i < args; i++)
            {
                ReadByte(t);
            }
            break;
        }
    case 0xff:
        return Outcome::kFinish;
    default:
        break;
    }

    return Outcome::kContinue;
}

void Performer::OnJumpBack(int t, uint32_t target)
{
    // A backward jump to commands the track hasn't run is not a loop; shared code may precede its callers. Also ignore
    // passes without waits, such as polling.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    const auto run = tr.visited.find(target);
    if (run == tr.visited.end() || !tr.last_wait || *tr.last_wait < run->second.first)
    {
        return;
    }

    // A pass that changed nothing is idle: the track only waited. Writing another track's port counts as a change.
    const uint32_t start = run->second.first;
    const uint64_t first_command = run->second.second;
    auto changed = [&](std::size_t i)
    {
        const Track& other = tracks_[i];
        return (other.last_change && *other.last_change >= start) ||
               (other.last_signal && *other.last_signal > first_command);
    };
    tr.idle = !changed(static_cast<std::size_t>(t));

    // Use the first track that can still advance, in the order the tracks opened. A pass counts only if no earlier
    // track contributed during it. A track whose last pass was idle doesn't count as advancing. An idle pass counts
    // only if no later track advances or contributed either. A later track that has been quiet for kHoldSeconds doesn't
    // count as advancing (revosf2's OnLoop).
    auto plays_on = [&](std::size_t i)
    {
        return tracks_[i].open && !WaitsForever(static_cast<int>(i)) && !tracks_[i].idle &&
               (static_cast<int>(i) < t || !Quiet(static_cast<int>(i)));
    };
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        if (static_cast<int>(i) != t && (static_cast<int>(i) < t || tr.idle) && (plays_on(i) || changed(i)))
        {
            return;
        }
    }

    // An idle pass in which nothing sounded means the sequence has ended: its tracks only wait now.
    if (tr.idle && !AnySounding() && start < tick_subframes_.size() && last_sounding_ < tick_subframes_[start])
    {
        silent_end_ = true;
        return;
    }

    // A pass that changed something, of another loop than the marked one, moves the markers to it.
    const std::pair<int, uint32_t> owner{t, target};
    if (!loop_owner_ || (!tr.idle && *loop_owner_ != owner))
    {
        out_.loop.reset();
        out_.loops = 0;
        loop_owner_ = owner;
        owner_passes_.clear();
        marked_ = 0;
        extended_ = false;
    }
    if (owner == *loop_owner_)
    {
        owner_passes_.emplace_back(start, tick_);
    }

    out_.loops++;
    if (!out_.loop)
    {
        out_.loop = Performance::Loop{start, tick_};
    }
    if (out_.loops >= options_.loops && TickSeconds(tick_) >= kMinLoopSeconds)
    {
        pending_loop_ = PendingLoop{t, start, tick_};
    }
}

// --- Channels ---

std::optional<std::size_t> Performer::AllocateVoice(uint8_t priority)
{
    // JASDSPChannel::getLowestChannel: a free voice, or the oldest voice of the lowest priority at or below this one.
    int best = -1;
    int best_priority = 0xff;
    uint32_t best_age = 0;
    for (int v = 0; v < kDspChannels; v++)
    {
        const int c = voices_[static_cast<std::size_t>(v)];
        if (c < 0)
        {
            return static_cast<std::size_t>(v);
        }

        const Channel& ch = channels_[static_cast<std::size_t>(c)];
        const int p = ch.released ? ch.release_priority : ch.priority;
        if (p <= priority && p <= best_priority && (p != best_priority || ch.age > best_age))
        {
            best = v;
            best_priority = p;
            best_age = ch.age;
        }
    }
    if (best < 0)
    {
        return std::nullopt;
    }

    // The voice's channel stops at once.
    const auto stolen = static_cast<std::size_t>(voices_[static_cast<std::size_t>(best)]);
    Approximate("notes cut when the game's 64 voices ran out");
    EndChannel(stolen);

    return static_cast<std::size_t>(best);
}

std::optional<std::size_t> Performer::StartChannel(int t, int key, int velocity, uint32_t timer, float sweep,
                                                   uint32_t sweep_subframes, bool bends)
{
    // JASTrack::channelStart and JASBank::noteOn: find what the program plays, and start a channel on a voice.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.program >= 0xf0)
    {
        Approximate("notes on the DSP's oscillators omitted");
        return std::nullopt;
    }

    const Bank* bank = banks_.Archive().FindBank(tr.bank);
    if (bank == nullptr)
    {
        Approximate("notes on a missing bank omitted");
        return std::nullopt;
    }

    const std::optional<NoteSound> found = FindSound(*bank, tr.program, key, velocity);
    if (!found)
    {
        return std::nullopt;
    }

    const Pcm* pcm = banks_.GetWave(found->wave_system, found->wave_id);
    const WaveSystem* ws = banks_.Archive().FindWaveSystem(found->wave_system);
    if (pcm == nullptr || ws == nullptr)
    {
        return std::nullopt;
    }
    if (found->random)
    {
        Approximate("random variation of instruments omitted");
    }
    if (found->velocity_sense)
    {
        Approximate("instrument effects that follow the velocity kept at each velocity region's top");
    }

    const std::optional<std::size_t> voice = AllocateVoice(tr.note_on_prio);
    if (!voice)
    {
        Approximate("notes lost when the game's 64 voices ran out");
        return std::nullopt;
    }

    Channel ch;
    ch.track = t;
    ch.sound = *found;
    ch.sound.wave = &ws->waves.at(found->wave_id);
    ch.key = static_cast<uint8_t>(std::clamp(key, 0, 127));
    ch.velocity = static_cast<uint8_t>(std::clamp(velocity, 0, 127));
    ch.init_pitch = found->pitch * (ch.sound.wave->sample_rate / kDacRate);
    ch.base_key = found->drum ? key : ch.sound.wave->base_key;
    ch.key_offset = static_cast<float>(key - ch.base_key);
    ch.update_timer = timer;
    ch.priority = tr.note_on_prio;
    ch.release_priority = tr.release_prio;
    ch.direct_release = found->direct_release;
    if (tr.direct_release != 0)
    {
        ch.direct_release = tr.direct_release;
    }
    ch.wave_loops = pcm->loop;
    ch.wave_length = pcm->Length();
    ch.params = tr.params;
    ch.vibrato.SetDelay(tr.vib_delay);
    ch.tremolo.SetDelay(tr.trem_delay);
    ch.vibrato.Set(tr.vib_depth, tr.vib_pitch);
    ch.tremolo.Set(tr.trem_depth, tr.trem_pitch);
    if (tr.skip_samples != 0)
    {
        Approximate("sample skips omitted");
    }

    // The instrument's oscillators, then the track's tables in their place (JASTrack::overwriteOsc).
    for (std::size_t i = 0; i < 2 && i < found->oscillators.size(); i++)
    {
        ch.osc_data[i] = found->oscillators[i];
    }
    if (found->drum)
    {
        static const Oscillator kDrumOscillator;
        ch.osc_data[0] = &kDrumOscillator;
        ch.osc_data[1] = nullptr;
    }
    for (std::size_t i = 0; i < 2; i++)
    {
        if (tr.osc[i])
        {
            ch.osc_data[i] = &*tr.osc[i];
        }
    }
    for (std::size_t i = 0; i < 2; i++)
    {
        if (ch.osc_data[i] != nullptr)
        {
            ch.osc[i].Start(*ch.osc_data[i]);
        }
    }

    if (sweep != 0.0f || sweep_subframes != 0)
    {
        SetSweep(ch, sweep, sweep_subframes);
    }
    ch.bends = bends;
    ch.on_tick = tick_;
    channels_.push_back(std::move(ch));
    const std::size_t c = channels_.size() - 1;
    voices_[*voice] = static_cast<int>(c);
    MidiNoteOn(c);

    return c;
}

void Performer::NoteOn(int t, uint32_t note_id, int key, int velocity)
{
    // JASTrack::noteOn: the note id's channel is released for the new one.
    if (note_id == 0 || note_id >= kNoteIds)
    {
        return;
    }

    key += TransposeTotal(t);
    Changed(t);
    NoteOff(t, note_id, 0);
    const std::optional<std::size_t> c = StartChannel(t, key, velocity, 0);
    tracks_[static_cast<std::size_t>(t)].slots[note_id] = c ? static_cast<int>(*c) : -1;
    if (c)
    {
        channels_[*c].note_id = static_cast<int>(note_id);
    }
}

void Performer::GateOn(int t, int key, int velocity, float time, uint32_t flags)
{
    // JASTrack::gateOn (0x80037214): a note on slot 0 with a length. Flag 1 sweeps from the latest key to the key over
    // the length, flag 2 ties the next note to this one, and flag 4 waits for the note to end.
    Changed(t);
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    key += TransposeTotal(t);
    if (tr.gate_rate != 100)
    {
        time *= static_cast<float>(tr.gate_rate) / 100.0f;
    }

    // JASTrack::seqTimeToDspTime (0x80037bc8) divides the length by the track's tick rate. A track opens with the
    // default rate (tempo 120, timebase 48). updateTrack sets the rate at the end of each sub-frame. So a note in the
    // tick that opens its track, or in a tick that changes the tempo, keeps the earlier rate.
    const auto subframes = static_cast<uint32_t>(time / tr.tick_rate);
    const uint32_t timer = (flags & 6) ? 0 : subframes;
    const bool sweeps = (flags & 1) != 0;
    const int from = sweeps ? tr.latest_key : key;
    const auto sweep = static_cast<float>(key - from);
    if (!tr.tied)
    {
        NoteOff(t, 0, 0);
        const std::optional<std::size_t> c = StartChannel(t, from, velocity, timer, sweeps ? sweep : 0.0f,
                                                          sweeps ? subframes : 0, sweeps || (flags & 2) != 0);
        tracks_[static_cast<std::size_t>(t)].slots[0] = c ? static_cast<int>(*c) : -1;
        if (c)
        {
            channels_[*c].note_id = 0;
        }
    }
    else if (tr.slots[0] >= 0)
    {
        // The sounding note goes on at the new key and velocity. Its MIDI note bends to the key, and CC11 follows the
        // velocity. A velocity above the MIDI note's goes in TieVelocities for the next run. The game's tie doesn't
        // start the envelope again. A note below hearing stays there. Its tie keeps CC11 at or below the note's start
        // and adds nothing to TieVelocities. A rise past CC11's limit would lower the whole sequence for a note nobody
        // hears.
        Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
        const bool heard = !BelowHearing(ch);
        ch.key_offset = static_cast<float>(from - ch.base_key);
        ch.velocity = static_cast<uint8_t>(std::clamp(velocity, 0, 127));
        ch.cc11_velocity = heard ? ch.velocity : std::min(ch.velocity, ch.midi_velocity);
        if (ch.midi_on && heard && ch.velocity > ch.midi_velocity)
        {
            uint8_t& peak = tie_velocities_[ch.midi_id];
            peak = std::max(peak, ch.velocity);
        }
        ch.update_timer = timer;
        if (sweeps)
        {
            SetSweep(ch, sweep, subframes);
        }
    }

    Track& again = tracks_[static_cast<std::size_t>(t)];
    again.tied = (flags & 2) != 0;
    again.latest_key = static_cast<uint8_t>(std::clamp(key, 0, 127));
}

void Performer::NoteOff(int t, uint32_t note_id, uint16_t release)
{
    // TChannelMgr::noteOff: release the note id's channel and detach it.
    if (note_id >= kNoteIds)
    {
        return;
    }

    int& slot = tracks_[static_cast<std::size_t>(t)].slots[note_id];
    if (slot < 0)
    {
        return;
    }

    const auto c = static_cast<std::size_t>(slot);
    slot = -1;
    Changed(t);
    ReleaseChannel(c, release);
    Detach(c);
}

void Performer::ReleaseChannel(std::size_t c, uint16_t direct_release)
{
    // JASChannel::release: the oscillators release, and the voice drops to the release priority.
    Channel& ch = channels_[c];
    if (ch.ended || ch.released)
    {
        return;
    }

    if (direct_release != 0)
    {
        ch.direct_release = direct_release;
    }
    for (std::size_t i = 0; i < 2; i++)
    {
        if (ch.osc[i].Valid())
        {
            ch.osc[i].Release(i == 0 ? ch.direct_release : 0);
        }
    }
    ch.released = true;
    MidiNoteOff(c);
}

void Performer::Detach(std::size_t c)
{
    // JASChannel::free: the channel no longer takes its track's settings.
    channels_[c].track = -1;
}

void Performer::EndChannel(std::size_t c)
{
    Channel& ch = channels_[c];
    if (ch.ended)
    {
        return;
    }

    ch.ended = true;
    MidiNoteOff(c);
    for (int& v : voices_)
    {
        if (v == static_cast<int>(c))
        {
            v = -1;
        }
    }
    if (ch.track >= 0)
    {
        for (int& slot : tracks_[static_cast<std::size_t>(ch.track)].slots)
        {
            if (slot == static_cast<int>(c))
            {
                slot = -1;
            }
        }
    }
    sound_end_ = std::max(sound_end_, static_cast<double>(subframe_) * kSubFrameSeconds);
}

float Performer::ChannelVolume(const Channel& ch, float osc_volume) const
{
    // JASChannel::updateEffectorParam: velocity squared, the instrument's volume, the oscillators, the track's volume
    // and the tremolo.
    const float v = static_cast<float>(ch.velocity) / 127.0f;
    const float volume = ch.sound.volume * osc_volume * ch.params.volume * (ch.tremolo.Value() + 1.0f) * (v * v);
    return std::max(volume, 0.0f);
}

float Performer::ChannelRatio(const Channel& ch, float osc_pitch) const
{
    // The pitch the channel gives the DSP, as a ratio of samples per output sample. The DSP takes a whole number of
    // 4096ths. The game truncates the pitch (0x8002b688). JASDsp::TChannel::setPitch (0x8002d3a0) keeps it below
    // 0x7fff.
    const float octaves = ch.params.bend + (ch.key_offset + ch.key_sweep) / 12.0f + ch.vibrato.Value();
    const float pitch = ch.init_pitch * osc_pitch * std::exp2(octaves) * ch.params.pitch * 4096.0f;
    return std::min(std::trunc(std::max(pitch, 0.0f)), 32767.0f) / 4096.0f;
}

void Performer::UpdateChannel(Channel& ch)
{
    // JASChannel::updateDSPChannel, or initialUpdateDSPChannel in its first sub-frame.
    if (ch.track >= 0)
    {
        const Track& tr = tracks_[static_cast<std::size_t>(ch.track)];
        ch.params = tr.params;
        ch.vibrato.Set(tr.vib_depth, tr.vib_pitch);
        ch.tremolo.Set(tr.trem_depth, tr.trem_pitch);
    }

    if (!ch.started)
    {
        ch.started = true;
        ch.vibrato.Reset();
        ch.tremolo.Reset();
    }
    else
    {
        ch.vibrato.Advance(kLfoStep);
        ch.tremolo.Advance(kLfoStep);
        if (ch.update_timer != 0 && --ch.update_timer == 0 && ch.track >= 0)
        {
            // A gate note's time is up: its track releases and detaches it (channelUpdateCallback's timer case).
            Track& tr = tracks_[static_cast<std::size_t>(ch.track)];
            const auto c = static_cast<std::size_t>(&ch - channels_.data());
            if (tr.slots[0] == static_cast<int>(c))
            {
                tr.slots[0] = -1;
            }
            ReleaseChannel(c, 0);
            Detach(c);
        }
        for (std::size_t i = 0; i < 2; i++)
        {
            if (ch.osc[i].Valid())
            {
                ch.osc[i].Advance(kOscStep);
                if (i == 0 && ch.osc[0].Stopped())
                {
                    EndChannel(static_cast<std::size_t>(&ch - channels_.data()));
                    return;
                }
            }
        }
    }

    float osc_volume = 1.0f, osc_pitch = 1.0f;
    for (std::size_t i = 0; i < 2; i++)
    {
        if (!ch.osc[i].Valid())
        {
            continue;
        }

        const float value = ch.osc[i].Value();
        if (ch.osc_data[i]->target == 0)
        {
            osc_volume *= value;
        }
        else if (ch.osc_data[i]->target == 1)
        {
            osc_pitch *= value;
        }
    }
    ch.envelope = osc_volume;
    ch.envelope_heard = ch.envelope_heard || !Inaudible(osc_volume);
    ch.level = ChannelVolume(ch, osc_volume);
    if (osc_pitch != 1.0f && !ch.pitch_moved)
    {
        ch.pitch_moved = true;
        Approximate("pitch envelopes omitted");
    }

    // The DSP plays the sub-frame at this pitch. A wave that doesn't loop ends when it runs out.
    ch.position += kSubFrameSamples * static_cast<double>(ChannelRatio(ch, osc_pitch));
    if (ch.sweep_count != 0)
    {
        ch.key_sweep += (ch.sweep_target - ch.key_sweep) / static_cast<float>(ch.sweep_count);
        ch.sweep_count--;
    }
    if (!ch.wave_loops && ch.position >= static_cast<double>(ch.wave_length))
    {
        EndChannel(static_cast<std::size_t>(&ch - channels_.data()));
    }
}

void Performer::UpdateChannels()
{
    for (int v = 0; v < kDspChannels; v++)
    {
        const int c = voices_[static_cast<std::size_t>(v)];
        if (c >= 0)
        {
            Channel& ch = channels_[static_cast<std::size_t>(c)];
            ch.age++;
            UpdateChannel(ch);
            last_sounding_ = subframe_;
        }
    }
}

// --- Recording ---

int Performer::SlotOf(int t)
{
    // The first 16 tracks that record something get MIDI tracks: the first track's children by number, others in the
    // order they first record. Tracks beyond that share the last one.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.slot >= 0)
    {
        return tr.slot;
    }

    int wanted = -1;
    if (tr.parent == 0)
    {
        for (int i = 0; i < kChildren; i++)
        {
            if (tracks_[0].children[static_cast<std::size_t>(i)] == t)
            {
                wanted = i;
            }
        }
    }
    if (wanted < 0 || slot_tracks_[static_cast<std::size_t>(wanted)] >= 0)
    {
        wanted = -1;
        for (int s = 0; s < 16 && wanted < 0; s++)
        {
            // Leave the slots that the first track's children would take for them.
            const bool reserved = tracks_[0].children[static_cast<std::size_t>(s)] >= 0 &&
                                  tracks_[0].children[static_cast<std::size_t>(s)] != t;
            if (slot_tracks_[static_cast<std::size_t>(s)] < 0 && !reserved)
            {
                wanted = s;
            }
        }
        for (int s = 0; s < 16 && wanted < 0; s++)
        {
            if (slot_tracks_[static_cast<std::size_t>(s)] < 0)
            {
                wanted = s;
            }
        }
    }
    if (wanted < 0)
    {
        Approximate("tracks beyond 16 share the last MIDI track");
        wanted = 15;
    }
    else
    {
        slot_tracks_[static_cast<std::size_t>(wanted)] = t;
        out_.track_names[static_cast<std::size_t>(wanted)] = tr.name;
    }
    tracks_[static_cast<std::size_t>(t)].slot = wanted;

    return wanted;
}

void Performer::Emit(int t, TrackEvent e, int part)
{
    // Controller changes go before the tick's notes. A change replaces an earlier value of the same setting in the
    // tick.
    e.tick = tick_;
    e.part = static_cast<uint8_t>(part >= 0 ? part : tracks_[static_cast<std::size_t>(t)].part);
    tracks_[static_cast<std::size_t>(t)].last_change = tick_;
    std::vector<TrackEvent>& events = out_.tracks[static_cast<std::size_t>(SlotOf(t))];
    const bool setting = e.kind != EventKind::kNoteOn && e.kind != EventKind::kNoteOff && e.kind != EventKind::kPreset;
    if (!setting)
    {
        events.push_back(e);
        return;
    }

    auto at = events.end();
    while (at != events.begin() && std::prev(at)->tick == tick_ && std::prev(at)->after == 0 &&
           (std::prev(at)->kind == EventKind::kNoteOn || std::prev(at)->kind == EventKind::kPreset))
    {
        --at;
    }
    for (auto it = at; it != events.begin() && std::prev(it)->tick == tick_; --it)
    {
        if (std::prev(it)->kind == e.kind && std::prev(it)->part == e.part && std::prev(it)->after == 0)
        {
            *std::prev(it) = e;
            return;
        }
    }
    events.insert(at, e);
}

void Performer::EmitTempo()
{
    // The MIDI tempo is the tempo times the timebase over the file's timebase.
    if (!ticked_)
    {
        out_.timebase = std::max<uint16_t>(timebase_, 1);
    }
    const double bpm = static_cast<double>(tempo_) * timebase_ / out_.timebase * tempo_rate_;
    if (bpm <= 0.0)
    {
        return;
    }

    if (!out_.tempo.empty() && out_.tempo.back().tick == tick_)
    {
        out_.tempo.back().bpm = bpm;
    }
    else if (out_.tempo.empty() || out_.tempo.back().bpm != bpm)
    {
        out_.tempo.push_back({tick_, bpm});
    }
}

bool Performer::Audible(const Channel& ch) const
{
    if (ch.ended)
    {
        return false;
    }
    if (!ch.started || !ch.released)
    {
        return true;
    }

    return !Inaudible(ch.level);
}

bool Performer::TailsOnPart(int slot, uint8_t part) const
{
    // A note on the part that its track has detached, and that can still be heard.
    for (const int c : voices_)
    {
        if (c >= 0)
        {
            const Channel& ch = channels_[static_cast<std::size_t>(c)];
            if (ch.midi_on && ch.track < 0 && ch.slot == slot && ch.part == part && Audible(ch))
            {
                return true;
            }
        }
    }

    return false;
}

void Performer::SwitchPart(int t)
{
    // Tails keep the settings they had when their track detached them. A change the track makes while they still sound
    // on its part would reach them. The track therefore moves to a part where none of its tails sound, and sends that
    // part every setting first.
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    const int slot = SlotOf(t);
    if (!TailsOnPart(slot, tr.part))
    {
        return;
    }

    // A note that bends keeps the part. Its pitch and CC11 follow the track's settings. The tails there follow them
    // too.
    if (GateBends(t))
    {
        Approximate("released notes take the settings of a later tied or sweeping note of their track");
        return;
    }

    const uint8_t limit = part_limits_[static_cast<std::size_t>(slot)];
    for (uint8_t p = 0; p < limit; p++)
    {
        if (p == tr.part || TailsOnPart(slot, p))
        {
            continue;
        }

        const uint8_t old = tr.part;
        tr.part = p;
        part_switches_.emplace_back(tick_, slot, p);
        parts_used_[static_cast<std::size_t>(slot)] =
            std::max<uint8_t>(parts_used_[static_cast<std::size_t>(slot)], static_cast<uint8_t>(p + 1));
        std::vector<std::pair<EventKind, int>> copy;
        for (const auto& [key, value] : tr.sent)
        {
            if (std::get<0>(key) == old)
            {
                copy.emplace_back(std::get<1>(key), value);
            }
        }
        for (const auto& [kind, value] : copy)
        {
            tracks_[static_cast<std::size_t>(t)].sent[{p, kind}] = value;
            TrackEvent e;
            e.kind = kind;
            if (kind == EventKind::kPreset)
            {
                e.preset = static_cast<uint32_t>(value);
            }
            else
            {
                e.value = static_cast<uint8_t>(value);
            }
            Emit(t, e);
        }
        const auto pitch = tracks_[static_cast<std::size_t>(t)].sent_pitch.find(old);
        if (pitch != tracks_[static_cast<std::size_t>(t)].sent_pitch.end())
        {
            tracks_[static_cast<std::size_t>(t)].sent_pitch[p] = pitch->second;
            TrackEvent e;
            e.kind = EventKind::kPitch;
            e.semitones = pitch->second;
            Emit(t, e);
        }
        return;
    }
}

void Performer::SendSetting(int t, EventKind kind, int value)
{
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    const auto it = tr.sent.find({tr.part, kind});
    if (it != tr.sent.end() && it->second == value)
    {
        return;
    }

    if (it != tr.sent.end())
    {
        SwitchPart(t);
    }
    tracks_[static_cast<std::size_t>(t)].sent[{tracks_[static_cast<std::size_t>(t)].part, kind}] = value;
    TrackEvent e;
    e.kind = kind;
    e.value = static_cast<uint8_t>(value);
    Emit(t, e);
}

void Performer::SendPitch(int t, float semitones)
{
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    const auto it = tr.sent_pitch.find(tr.part);
    if ((it == tr.sent_pitch.end() && semitones == 0.0f) || (it != tr.sent_pitch.end() && it->second == semitones))
    {
        return;
    }

    SwitchPart(t);
    tracks_[static_cast<std::size_t>(t)].sent_pitch[tracks_[static_cast<std::size_t>(t)].part] = semitones;
    TrackEvent e;
    e.kind = EventKind::kPitch;
    e.semitones = semitones;
    Emit(t, e);
}

float Performer::GatePitch(int t) const
{
    // The bend of the track's gate note on top of the track's: its key since its MIDI note started, and its sweep.
    if (!GateBends(t))
    {
        return 0.0f;
    }

    const Channel& ch = channels_[static_cast<std::size_t>(tracks_[static_cast<std::size_t>(t)].slots[0])];
    return ch.key_offset - ch.midi_key_offset + ch.key_sweep;
}

bool Performer::GateBends(int t) const
{
    const Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (tr.slots[0] < 0)
    {
        return false;
    }

    const Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
    return ch.midi_on && ch.bends && ch.part == tr.part;
}

void Performer::SweepBetweenTicks()
{
    // A sweep moves on in every sub-frame, between ticks too. Send its pitch where the sub-frame's events go: at the
    // start of the sub-frame before (RecordTickTime), as a fraction of the way from the last tick to the next.
    if (!ticked_ || tick_ >= tick_seconds_.size() || TicksPerSubFrame() <= 0.0f)
    {
        return;
    }

    const double per_tick = kSubFrameSeconds / static_cast<double>(TicksPerSubFrame());
    const double at = static_cast<double>(subframe_ > 0 ? subframe_ - 1 : 0) * kSubFrameSeconds;
    const double after = std::clamp((at - tick_seconds_[tick_]) / per_tick, 1e-6, 1.0 - 1e-6);
    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        const int i = static_cast<int>(t);
        if (!tracks_[t].open || !GateBends(i))
        {
            continue;
        }

        const float semitones = 12.0f * tracks_[t].params.bend + GatePitch(i);
        const auto sent = tracks_[t].sent_pitch.find(tracks_[t].part);
        if (sent != tracks_[t].sent_pitch.end() && sent->second == semitones)
        {
            continue;
        }

        const int slot = SlotOf(i);
        tracks_[t].sent_pitch[tracks_[t].part] = semitones;
        TrackEvent e;
        e.tick = tick_;
        e.kind = EventKind::kPitch;
        e.semitones = semitones;
        e.part = tracks_[t].part;
        e.after = after;
        out_.tracks[static_cast<std::size_t>(slot)].push_back(e);
    }
}

double Performer::TrackLevel(int t) const
{
    // CC11 holds the square root of the parents' volumes and the sound's volume. CC11 acts squared.
    const Track& tr = tracks_[static_cast<std::size_t>(t)];
    const float above = tr.parent < 0 ? sound_.volume : tracks_[static_cast<std::size_t>(tr.parent)].params.volume;
    return 127.0 * std::sqrt(std::max(static_cast<double>(above), 0.0));
}

void Performer::RecordOutputs(int t)
{
    // The MIDI controllers for a track's settings: CC7 its volume, CC11 the rest of the volume, CC10 the pan, the
    // bend, CC1 the vibrato's depth and CC91 the effect send.
    const Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (!tr.open || tr.slot < 0)
    {
        return;
    }

    // A tied note's later velocities scale CC11. The game squares the velocity, and CC11 acts squared.
    double level = TrackLevel(t);
    if (GateBends(t))
    {
        const Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
        level *= static_cast<double>(ch.cc11_velocity) / static_cast<double>(ch.midi_velocity);
    }
    level_peak_ = std::max(level_peak_, level);
    const int volume = std::clamp(static_cast<int>(std::lround(127.0 * std::abs(tr.move[0].value))), 0, 127);
    const int expression = std::clamp(static_cast<int>(std::lround(level * level_scale_)), 0, 127);
    const int pan = std::clamp(static_cast<int>(std::lround(127.0 * std::clamp(tr.params.pan, 0.0f, 1.0f))), 0, 127);
    const double vibrato_cents = 1200.0 * std::abs(tr.vib_depth);
    if (vibrato_cents > kVibratoRangeCents)
    {
        Approximate("vibrato deeper than a semitone kept to a semitone");
    }
    // CC1 holds 128 times the depth over kVibratoRangeCents. The SoundFont's and SFZ's modulators read it that way.
    const int modulation =
        std::clamp(static_cast<int>(std::lround(128.0 * vibrato_cents / kVibratoRangeCents)), 0, 127);
    const int reverb =
        std::clamp(static_cast<int>(std::lround(127.0 * std::clamp(tr.params.fx_mix, 0.0f, 1.0f))), 0, 127);
    if (tr.trem_depth != 0.0f)
    {
        Approximate("tremolo omitted");
    }
    if (tr.iir)
    {
        Approximate("track filters omitted");
    }

    SendSetting(t, EventKind::kVolume, volume);
    SendSetting(t, EventKind::kExpression, expression);
    SendSetting(t, EventKind::kPan, pan);
    SendSetting(t, EventKind::kModulation, modulation);
    SendSetting(t, EventKind::kReverb, reverb);
    SendPitch(t, 12.0f * tr.params.bend + GatePitch(t));
}

int Performer::PresetIndex(const PresetKey& key)
{
    for (std::size_t i = 0; i < out_.presets.size(); i++)
    {
        if (out_.presets[i] == key)
        {
            return static_cast<int>(i);
        }
    }

    out_.presets.push_back(key);

    return static_cast<int>(out_.presets.size() - 1);
}

void Performer::MidiNoteOn(std::size_t c)
{
    Channel& ch = channels_[c];
    const int t = ch.track;
    Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (ch.velocity == 0)
    {
        return; // the game plays it silently
    }

    // The note's preset: the bank and program, with the track's oscillators and settings.
    PresetKey key;
    key.bank = ch.sound.bank;
    key.program = ch.sound.program;
    for (std::size_t i = 0; i < 2; i++)
    {
        if (tr.osc[i])
        {
            int index = -1;
            for (std::size_t k = 0; k < out_.oscillators.size(); k++)
            {
                const Oscillator& o = out_.oscillators[k];
                if (o.target == tr.osc[i]->target && o.attack.size() == tr.osc[i]->attack.size() &&
                    std::equal(o.attack.begin(), o.attack.end(), tr.osc[i]->attack.begin(),
                               [](const OscPoint& a, const OscPoint& b)
                               { return a.mode == b.mode && a.time == b.time && a.value == b.value; }))
                {
                    index = static_cast<int>(k);
                }
            }
            if (index < 0)
            {
                out_.oscillators.push_back(*tr.osc[i]);
                index = static_cast<int>(out_.oscillators.size() - 1);
            }
            (i == 0 ? key.volume_oscillator : key.pitch_oscillator) = index;
        }
    }
    key.direct_release = tr.direct_release;
    key.vibrato_rate = tr.vib_pitch;
    key.vibrato_delay = tr.vib_delay;
    const int preset = PresetIndex(key);

    // A note that bends goes on a part where no tail of its track sounds. The tails keep their pitch there.
    if (ch.bends)
    {
        SwitchPart(t);
    }

    // The settings go before the note. The note then starts with them.
    RecordOutputs(t);
    Track& again = tracks_[static_cast<std::size_t>(t)];
    const auto sent = again.sent.find({again.part, EventKind::kPreset});
    if (sent == again.sent.end() || sent->second != preset)
    {
        again.sent[{again.part, EventKind::kPreset}] = preset;
        TrackEvent p;
        p.kind = EventKind::kPreset;
        p.preset = static_cast<uint32_t>(preset);
        Emit(t, p);
    }

    ch.midi_on = true;
    ch.midi_key_offset = ch.key_offset;
    ch.midi_velocity = ch.velocity;
    ch.cc11_velocity = ch.velocity;
    ch.slot = SlotOf(t);
    ch.midi_id = next_midi_id_++;

    // A note that ties take above its velocity starts at the highest they reach. The instrument has to play the same
    // wave at that velocity. CC11 lowers the note until then.
    const auto planned = tie_velocities_in_.find(ch.midi_id);
    if (planned != tie_velocities_in_.end())
    {
        const Bank* bank = banks_.Archive().FindBank(ch.sound.bank);
        const std::optional<NoteSound> higher =
            bank != nullptr ? FindSound(*bank, ch.sound.program, ch.key, planned->second) : std::nullopt;
        if (higher && higher->wave_system == ch.sound.wave_system && higher->wave_id == ch.sound.wave_id)
        {
            ch.midi_velocity = planned->second;
        }
    }
    ch.part = again.part;
    ch.preset = static_cast<uint32_t>(preset);
    TrackEvent e;
    e.kind = EventKind::kNoteOn;
    e.key = ch.key;
    e.value = ch.midi_velocity;
    e.note = ch.midi_id;
    Emit(t, e);
    const int slot = SlotOf(t);
    parts_used_[static_cast<std::size_t>(slot)] =
        std::max<uint8_t>(parts_used_[static_cast<std::size_t>(slot)], static_cast<uint8_t>(again.part + 1));
}

void Performer::MidiNoteOff(std::size_t c)
{
    Channel& ch = channels_[c];
    if (!ch.midi_on)
    {
        return;
    }

    ch.midi_on = false;
    TrackEvent e;
    e.kind = EventKind::kNoteOff;
    e.key = ch.key;
    e.note = ch.midi_id;
    e.tick = tick_;
    e.part = ch.part;
    out_.tracks[static_cast<std::size_t>(ch.slot)].push_back(e);
}

void Performer::Approximate(const std::string& what)
{
    out_.approximations[what]++;
}

void Performer::RecordTickTime()
{
    // A tick's nominal time, and the start of the sub-frame before the one it runs in as a fractional tick: where the
    // MIDI writer puts its events (Performance::frame_ticks). The game runs a tick in the first sub-frame after its
    // time, and the DSP plays its changes from the next one.
    const double per_tick = kSubFrameSeconds / static_cast<double>(TicksPerSubFrame());
    tick_seconds_.push_back(tick_seconds_.empty() ? 0.0 : tick_seconds_.back() + per_tick);
    tick_subframes_.push_back(subframe_);
    const double start = static_cast<double>(subframe_ > 0 ? subframe_ - 1 : 0) * kSubFrameSeconds;
    std::size_t k = tick_seconds_.size() - 1;
    while (k > 0 && tick_seconds_[k] > start)
    {
        k--;
    }

    double tick = static_cast<double>(k);
    if (k + 1 < tick_seconds_.size() && tick_seconds_[k + 1] > tick_seconds_[k])
    {
        tick += (start - tick_seconds_[k]) / (tick_seconds_[k + 1] - tick_seconds_[k]);
    }
    out_.frame_ticks.push_back(std::min(std::max(tick, 0.0), static_cast<double>(tick_)));
}

bool Performer::TrackSounds(int t) const
{
    for (const int c : voices_)
    {
        if (c >= 0 && channels_[static_cast<std::size_t>(c)].track == t)
        {
            return true;
        }
    }

    return false;
}

bool Performer::AnySounding() const
{
    return std::ranges::any_of(voices_, [](int v) { return v >= 0; });
}

bool Performer::Steady() const
{
    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.open)
        {
            continue;
        }
        if (!WaitsForever(static_cast<int>(t)))
        {
            return false;
        }
        for (const MoveParam& m : tr.move)
        {
            if (m.count != 0)
            {
                return false;
            }
        }
    }

    return true;
}

bool Performer::WaitsForever(int t) const
{
    // A track waiting for a note that never ends, or waiting kHeldTicks or more: minutes at a typical tempo. Games wait
    // that long for sounds they stop themselves.
    const Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (!tr.open)
    {
        return false;
    }
    if (tr.wait_note_finish)
    {
        if (tr.slots[0] < 0)
        {
            return false;
        }

        const Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
        return ch.wave_loops && !ch.released && !ch.ended;
    }

    return tr.wait >= kHeldTicks;
}

bool Performer::Quiet(int t) const
{
    const Track& tr = tracks_[static_cast<std::size_t>(t)];
    if (TrackSounds(t))
    {
        return false;
    }

    const uint32_t since = std::max(tr.last_change.value_or(0), tr.opened);
    return TickSeconds(tick_) - TickSeconds(since) >= kHoldSeconds;
}

bool Performer::Stuck() const
{
    if (AnySounding())
    {
        return false;
    }

    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        if (tracks_[t].open && !WaitsForever(static_cast<int>(t)))
        {
            return false;
        }
    }

    return true;
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    const double now = TickSeconds(tick_);
    if (now - TickSeconds(start) <= kHoldSeconds)
    {
        return std::nullopt;
    }

    // The last change: the last MIDI event or tempo change.
    uint32_t last = 0;
    for (const std::vector<TrackEvent>& events : out_.tracks)
    {
        if (!events.empty())
        {
            last = std::max(last, events.back().tick);
        }
    }
    if (!out_.tempo.empty())
    {
        last = std::max(last, out_.tempo.back().tick);
    }
    if (now - TickSeconds(last) <= kHoldSeconds)
    {
        return std::nullopt;
    }

    // Every sounding note must be endless, and still held by its track.
    bool sounding = false;
    for (const int c : voices_)
    {
        if (c < 0)
        {
            continue;
        }

        const Channel& ch = channels_[static_cast<std::size_t>(c)];
        if (!ch.wave_loops || ch.released || ch.track < 0)
        {
            return std::nullopt;
        }
        sounding = true;
    }
    if (!sounding)
    {
        return std::nullopt;
    }

    return TickSeconds(last) + kHoldSeconds;
}

void Performer::Changed(int t)
{
    tracks_[static_cast<std::size_t>(t)].last_change = tick_;
}

double Performer::TickSeconds(uint32_t tick) const
{
    if (tick < tick_seconds_.size())
    {
        return tick_seconds_[tick];
    }

    const double per_tick = kSubFrameSeconds / static_cast<double>(std::max(TicksPerSubFrame(), 1e-6f));
    return (tick_seconds_.empty() ? 0.0 : tick_seconds_.back()) +
           (static_cast<double>(tick) - static_cast<double>(tick_seconds_.size()) + 1.0) * per_tick;
}

std::map<Performer::SettingKey, TrackEvent> Performer::SettingsAt(const Performance& perf, uint32_t tick)
{
    // A MIDI track's first part starts at its key's pitch without an event.
    std::map<SettingKey, TrackEvent> settings;
    for (int t = 0; t < 16; t++)
    {
        settings[{t, 0, EventKind::kPitch}] = TrackEvent{0, EventKind::kPitch};
        for (const TrackEvent& e : perf.tracks[static_cast<std::size_t>(t)])
        {
            if (e.tick > tick || (e.tick == tick && e.after > 0))
            {
                break;
            }
            if (e.kind != EventKind::kNoteOn && e.kind != EventKind::kNoteOff)
            {
                settings[{t, e.part, e.kind}] = e;
            }
        }
    }

    return settings;
}

Performer::PassStart Performer::PassStartAt(uint32_t tick) const
{
    PassStart start;
    start.settings = SettingsAt(out_, tick);
    for (const Performance::Tempo& tempo : out_.tempo)
    {
        if (tempo.tick > tick)
        {
            break;
        }
        start.bpm = tempo.bpm;
    }
    for (const auto& [switch_tick, slot, part] : part_switches_)
    {
        if (switch_tick > tick)
        {
            break;
        }
        start.parts[static_cast<std::size_t>(slot)] = part;
    }

    return start;
}

Performer Performer::PlayOn(uint32_t until) const
{
    Performer probe = *this;
    probe.stop_ = false;
    probe.pending_loop_.reset();
    probe.options_.loops = std::numeric_limits<int>::max();
    while (probe.tick_ < until && probe.Advance())
    {
        probe.pending_loop_.reset();
        if (TickSeconds(probe.tick_) >= kMaxSeconds)
        {
            break;
        }
    }

    return probe;
}

bool Performer::SameSetting(const TrackEvent& a, const TrackEvent& b)
{
    switch (a.kind)
    {
    case EventKind::kPitch:
        return a.semitones == b.semitones;
    case EventKind::kPreset:
        return a.preset == b.preset;
    default:
        return a.value == b.value;
    }
}

bool Performer::RepeatsAsTheGame(uint32_t start, uint32_t end) const
{
    const PassStart at_start = PassStartAt(start);
    const PassStart next = PassStartAt(end);
    const uint32_t length = end - start;

    // The tempo: a repeat starts with the tempo at the loop's end. A tempo event on the loop's first tick replaces that
    // with the start's tempo. When the game's next pass starts with the start's tempo, RestoreAtLoopStart sends that
    // tempo again, and the repeat starts with it too.
    double end_bpm = at_start.bpm;
    bool tempo_sent = false;
    for (const Performance::Tempo& tempo : out_.tempo)
    {
        tempo_sent = tempo_sent || tempo.tick == start;
        if (tempo.tick < end)
        {
            end_bpm = tempo.bpm;
        }
    }
    const double repeat_bpm = tempo_sent || next.bpm == at_start.bpm ? at_start.bpm : end_bpm;

    auto bpm_at = [&](uint32_t tick, uint32_t since, double otherwise)
    {
        double bpm = otherwise;
        for (const Performance::Tempo& tempo : out_.tempo)
        {
            if (tempo.tick > tick)
            {
                break;
            }
            if (tempo.tick >= since)
            {
                bpm = tempo.bpm;
            }
        }

        return bpm;
    };

    // A note of a pass: its tick in the pass, key, velocity and part, and the settings of its part that it plays with.
    struct Strike
    {
        uint32_t offset = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        uint8_t part = 0;
        std::map<EventKind, TrackEvent> settings;
    };

    for (int t = 0; t < 16; t++)
    {
        const auto slot = static_cast<std::size_t>(t);
        std::vector<Strike> loop;
        std::vector<Strike> game;
        std::array<std::map<EventKind, TrackEvent>, 16> settings{};
        std::array<std::map<EventKind, TrackEvent>, 16> sent{};
        std::array<std::map<EventKind, TrackEvent>, 16> at_end{};
        std::array<std::set<EventKind>, 16> in_first_tick{};
        settings[0][EventKind::kPitch] = TrackEvent{0, EventKind::kPitch};

        bool ended = false;
        for (const TrackEvent& e : out_.tracks[slot])
        {
            if (e.tick >= end + length)
            {
                break;
            }
            if (e.part >= 16)
            {
                continue;
            }

            if (!ended && e.tick >= end)
            {
                at_end = settings;
                ended = true;
            }
            if (e.kind != EventKind::kNoteOn && e.kind != EventKind::kNoteOff)
            {
                settings[e.part][e.kind] = e;
                if (e.tick >= start)
                {
                    sent[e.part][e.kind] = e;
                }
                if (e.tick == start && e.after == 0)
                {
                    in_first_tick[e.part].insert(e.kind);
                }
            }
            else if (e.kind == EventKind::kNoteOn && e.tick >= start)
            {
                const bool in_loop = e.tick < end;
                (in_loop ? loop : game)
                    .push_back({e.tick - (in_loop ? start : end), e.key, e.value, e.part,
                                in_loop ? sent[e.part] : settings[e.part]});
            }
        }
        if (!ended)
        {
            at_end = settings;
        }
        if (loop.size() != game.size())
        {
            return false;
        }

        auto repeat_value = [&](uint8_t part, EventKind kind) -> const TrackEvent*
        {
            const auto first = at_start.settings.find({t, part, kind});
            const auto again = next.settings.find({t, next.parts[slot], kind});
            if (part == at_start.parts[slot] && !in_first_tick[part].contains(kind) &&
                first != at_start.settings.end() && again != next.settings.end() &&
                SameSetting(first->second, again->second))
            {
                return &first->second;
            }

            const auto kept = at_end[part].find(kind);
            return kept != at_end[part].end() ? &kept->second : nullptr;
        };

        for (std::size_t k = 0; k < loop.size(); k++)
        {
            const Strike& a = loop[k];
            const Strike& b = game[k];
            if (a.offset != b.offset || a.key != b.key || a.velocity != b.velocity ||
                bpm_at(start + a.offset, start, repeat_bpm) != bpm_at(end + b.offset, 0, at_start.bpm))
            {
                return false;
            }

            std::set<EventKind> kinds;
            const std::array<const std::map<EventKind, TrackEvent>*, 3> lists = {&a.settings, &b.settings,
                                                                                 &at_end[a.part]};
            for (const auto* list : lists)
            {
                for (const auto& [kind, e] : *list)
                {
                    kinds.insert(kind);
                }
            }
            for (EventKind kind : kinds)
            {
                const auto sent_before = a.settings.find(kind);
                const TrackEvent* repeat =
                    sent_before != a.settings.end() ? &sent_before->second : repeat_value(a.part, kind);
                const auto played = b.settings.find(kind);
                if ((repeat == nullptr) != (played == b.settings.end()) ||
                    (repeat && !SameSetting(*repeat, played->second)))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

Performer::LoopChoice Performer::ChooseSteadyPass() const
{
    // A loop that draws random numbers plays each pass otherwise anyway, and keeps its first pass.
    if (owner_passes_.empty() || (last_draw_ && *last_draw_ >= owner_passes_[0].first))
    {
        return {true, std::nullopt, false};
    }

    // A copy plays the two passes after the owner's last pass. Each pass and the next then have the pass after them to
    // compare with.
    const auto [last_start, last_end] = owner_passes_.back();
    const uint32_t length = last_end - last_start;
    const Performer probe = PlayOn(last_end + 2 * length);
    for (std::size_t i = marked_; i < owner_passes_.size(); i++)
    {
        const auto [start, end] = owner_passes_[i];
        if (probe.RepeatsAsTheGame(start, end))
        {
            return {static_cast<int>(owner_passes_.size() - i) >= options_.loops, i, false};
        }
    }

    // No pass so far plays as the next one does. If the next pass plays as the one after it, play it too, and otherwise
    // keep the first.
    if (!extended_ && probe.RepeatsAsTheGame(last_end, last_end + length))
    {
        return {false, std::nullopt, true};
    }

    return {true, std::nullopt, false};
}

bool Performer::MarkSteadyPass()
{
    const LoopChoice choice = replayed_ < replay_.size() ? replay_[replayed_++] : ChooseSteadyPass();
    choices_.push_back(choice);
    if (choice.marked && *choice.marked < owner_passes_.size())
    {
        marked_ = *choice.marked;
        const auto [start, end] = owner_passes_[marked_];
        out_.loop = Performance::Loop{start, end};
        out_.loops = static_cast<int>(owner_passes_.size() - marked_);
    }
    extended_ = extended_ || choice.extend;

    return choice.stop;
}

void Performer::DecideLoop()
{
    const PendingLoop pending = *pending_loop_;
    pending_loop_.reset();
    if (MarkSteadyPass())
    {
        stop_ = true;
        stop_tick_ = pending.end;
        held_sound_end_ = HeldSoundEnd(pending.start);
    }
}

void Performer::RestoreAtLoopStart(const PassStart& next_pass)
{
    const uint32_t start = out_.loop->start;
    const uint32_t end = out_.loop->end;
    const PassStart at_loop_start = PassStartAt(start);

    auto tempo_at = [&](uint32_t tick, bool in_tick)
    {
        double bpm = at_loop_start.bpm;
        for (const Performance::Tempo& tempo : out_.tempo)
        {
            if (tempo.tick > start && (tempo.tick < tick || (in_tick && tempo.tick == tick)))
            {
                bpm = tempo.bpm;
            }
        }

        return bpm;
    };

    const bool tempo_sent = std::ranges::any_of(out_.tempo, [start](const auto& tempo) { return tempo.tick == start; });
    if (!tempo_sent && next_pass.bpm == at_loop_start.bpm &&
        (tempo_at(end, false) != at_loop_start.bpm || tempo_at(end, true) != at_loop_start.bpm))
    {
        auto later = [start](const Performance::Tempo& tempo)
        {
            return tempo.tick > start;
        };
        out_.tempo.insert(std::ranges::find_if(out_.tempo, later), {start, at_loop_start.bpm});
    }

    for (const auto& [key, at_start] : at_loop_start.settings)
    {
        // The game gives the setting back its value at the loop's start. Only the part that the MIDI track's new notes
        // go to counts.
        const auto [t, part, kind] = key;
        const auto slot = static_cast<std::size_t>(t);
        if (part != at_loop_start.parts[slot])
        {
            continue;
        }
        const auto next = next_pass.settings.find({t, next_pass.parts[slot], kind});
        if (next == next_pass.settings.end() || !SameSetting(next->second, at_start))
        {
            continue;
        }

        std::vector<TrackEvent>& events = out_.tracks[slot];
        bool sent = false;
        const TrackEvent* before_end = &at_start;
        const TrackEvent* at_end = &at_start;
        for (const TrackEvent& e : events)
        {
            if (e.part != part || e.kind != kind || e.tick < start || e.tick > end || (e.tick == end && e.after > 0))
            {
                continue;
            }
            if (e.tick == start && e.after == 0)
            {
                sent = true;
            }
            if (e.tick < end)
            {
                before_end = &e;
            }
            at_end = &e;
        }
        if (sent || (SameSetting(*before_end, at_start) && SameSetting(*at_end, at_start)))
        {
            continue;
        }

        // Send it with the loop start's other settings, ahead of its notes.
        auto past_settings = [start](const TrackEvent& e)
        {
            return e.tick > start ||
                   (e.tick == start && (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff));
        };
        const auto at = std::find_if(events.begin(), events.end(), past_settings);
        TrackEvent e = at_start;
        e.tick = start;
        e.after = 0;
        events.insert(at, e);
    }
}

uint32_t Performer::SecondsToTick(double seconds) const
{
    const auto it = std::lower_bound(tick_seconds_.begin(), tick_seconds_.end(), seconds);
    if (it != tick_seconds_.end())
    {
        return static_cast<uint32_t>(it - tick_seconds_.begin());
    }

    const double per_tick = kSubFrameSeconds / static_cast<double>(std::max(TicksPerSubFrame(), 1e-6f));
    return static_cast<uint32_t>(tick_seconds_.size() - 1 + std::ceil((seconds - tick_seconds_.back()) / per_tick));
}

void Performer::EndPerformance(bool finished)
{
    // A main loop that holds a sound ends kHoldSeconds after its last change, without loop markers.
    if (held_sound_end_)
    {
        out_.holds = true;
        out_.loop.reset();
        out_.loops = 0;
    }

    // A performance that stops at the end of a pass leaves out what the next pass started in its last tick.
    const std::optional<uint32_t> end_tick =
        held_sound_end_ ? std::optional<uint32_t>(std::min(SecondsToTick(*held_sound_end_), tick_)) : stop_tick_;
    if (!finished && end_tick)
    {
        for (std::vector<TrackEvent>& events : out_.tracks)
        {
            std::set<uint32_t> late;
            for (const TrackEvent& e : events)
            {
                if (e.kind == EventKind::kNoteOn && e.tick >= *end_tick)
                {
                    late.insert(e.note);
                }
            }
            std::erase_if(events, [&](const TrackEvent& e) { return e.tick > *end_tick || late.contains(e.note); });
            for (Channel& ch : channels_)
            {
                if (late.contains(ch.midi_id))
                {
                    ch.midi_on = false;
                }
            }
        }
        tick_ = *end_tick;
    }

    // A finished sequence plays its tails out; otherwise every note stops at the end.
    if (finished)
    {
        const uint64_t limit = subframe_ + static_cast<uint64_t>(kMaxTailSeconds / kSubFrameSeconds);
        while (AnySounding() && subframe_ < limit)
        {
            subframe_++;
            UpdateChannels();
        }
        sound_end_ = std::max(sound_end_, static_cast<double>(subframe_) * kSubFrameSeconds);
        if (!AnySounding())
        {
            sound_end_ = std::min(sound_end_, static_cast<double>(last_sounding_ + 1) * kSubFrameSeconds);
        }
    }
    for (std::size_t c = 0; c < channels_.size(); c++)
    {
        MidiNoteOff(c);
    }

    if (finished)
    {
        uint32_t last_event = 0;
        for (const std::vector<TrackEvent>& events : out_.tracks)
        {
            for (const TrackEvent& e : events)
            {
                if (e.kind != EventKind::kNoteOff)
                {
                    last_event = std::max(last_event, e.tick);
                }
            }
        }
        out_.seconds = std::max(sound_end_, TickSeconds(last_event));
        out_.end_tick = std::max(last_event, SecondsToTick(out_.seconds));
    }
    else
    {
        out_.seconds = TickSeconds(tick_);
        out_.end_tick = tick_;
    }

    // The note-offs at the end come at the end.
    for (std::vector<TrackEvent>& events : out_.tracks)
    {
        for (TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOff && e.tick > out_.end_tick)
            {
                e.tick = out_.end_tick;
            }
        }
    }
}

bool Performer::Advance()
{
    UpdateChannels();
    if (accumulator_ < 1.0f)
    {
        SweepBetweenTicks();
    }

    // JASTrack::seqMain: the ticks that are due, then the settings for the next sub-frame.
    while (accumulator_ >= 1.0f)
    {
        accumulator_ -= 1.0f;
        if (ticked_)
        {
            tick_++;
        }
        RecordTickTime();
        if (!ticked_)
        {
            EmitTempo();
        }
        const int ret = TickProc(0);
        ticked_ = true;
        UpdateSeq(0, false, 1.0f);
        if (ret < 0)
        {
            CloseTrack(0);
            return false;
        }

        for (std::size_t t = 0; t < tracks_.size(); t++)
        {
            RecordOutputs(static_cast<int>(t));
        }
    }

    accumulator_ += TicksPerSubFrame();
    UpdateSeq(0, false, 1.0f);
    subframe_++;

    return true;
}

Performance Performer::Run()
{
    // JAISeq starts the sequence on a first track, with the ports the game has set.
    voices_.fill(-1);
    slot_tracks_.fill(-1);
    const int root = NewTrack(-1, 0);
    Track& tr = tracks_[static_cast<std::size_t>(root)];
    tr.pc = sound_.start;
    tr.open = true;
    for (const auto& [port, value] : options_.ports)
    {
        tr.ports[port & 15] = value;
        tr.imported = static_cast<uint16_t>(tr.imported | (1u << (port & 15)));
    }

    bool finished = false;
    double steady_since = -1.0;
    for (;;)
    {
        if (!Advance())
        {
            finished = true;
            break;
        }
        if (pending_loop_)
        {
            DecideLoop();
            if (stop_)
            {
                break;
            }
        }

        // A sequence whose tracks only wait, with nothing sounding, has ended.
        if (silent_end_ || Stuck())
        {
            finished = true;
            out_.loop.reset();
            out_.loops = 0;
            break;
        }

        const double now = TickSeconds(tick_);
        if (steady_since < 0 && Steady())
        {
            steady_since = now;
        }
        if (steady_since >= 0 && !Steady())
        {
            steady_since = -1.0;
        }
        if (steady_since >= 0 && now - steady_since >= kHoldSeconds)
        {
            out_.holds = true;
            break;
        }

        // Treat a sequence that has been silent for longer than kHoldSeconds at the time limit as finished. It ends
        // with its last sound.
        if (static_cast<double>(subframe_) * kSubFrameSeconds >= kMaxSeconds)
        {
            finished =
                !AnySounding() && static_cast<double>(subframe_ - last_sounding_) * kSubFrameSeconds > kHoldSeconds;
            out_.truncated = !finished;
            break;
        }
        if (TicksPerSubFrame() <= 0.0f)
        {
            out_.warnings.push_back(timebase_ == 0 ? "timebase 0: the sequence stops advancing"
                                                   : "tempo 0: the sequence stops advancing");
            break;
        }
    }

    const PassStart next_pass = out_.loop && stop_ && !held_sound_end_ ? PassStartAt(out_.loop->end) : PassStart{};
    EndPerformance(finished);
    if (finished)
    {
        out_.loop.reset();
        out_.loops = 0;
    }
    if (out_.loop)
    {
        RestoreAtLoopStart(next_pass);
    }

    return out_;
}

// Scale factor that keeps rounded CC11 values within 127 for a maximum unscaled value of `peak` (Perform).
double LevelScale(double peak)
{
    return peak >= 127.5 ? 127.0 / peak : 1.0;
}

// Fit the parts that each MIDI track used into MIDI's 16 channels. The MIDI writer gives them out the same way.
std::array<uint8_t, 16> FitParts(const Performance& perf, const std::array<uint8_t, 16>& used)
{
    std::array<bool, 16> plays{};
    int channels = 0;
    for (int t = 0; t < 16; t++)
    {
        auto note_on = [](const TrackEvent& e)
        {
            return e.kind == EventKind::kNoteOn;
        };
        plays[static_cast<std::size_t>(t)] = std::ranges::any_of(perf.tracks[static_cast<std::size_t>(t)], note_on);
        channels += plays[static_cast<std::size_t>(t)] ? 1 : 0;
    }

    std::array<uint8_t, 16> limits;
    limits.fill(1);
    for (int t = 0; t < 16; t++)
    {
        if (plays[static_cast<std::size_t>(t)])
        {
            const int extra = std::min(std::max<int>(used[static_cast<std::size_t>(t)], 1) - 1, 16 - channels);
            limits[static_cast<std::size_t>(t)] = static_cast<uint8_t>(1 + extra);
            channels += extra;
        }
    }

    return limits;
}

} // namespace

Performance Perform(const SequenceSound& sound, BankSet& banks, const PerformOptions& options)
{
    if (sound.start >= sound.data.size())
    {
        throw FormatError("sequence starts past its data");
    }

    // Run again when ties take notes above their velocity, to start those notes higher. Run again when CC11 would pass
    // 127, scaling every value equally, or when the tracks' parts don't fit MIDI's channels. Four runs at most.
    std::array<uint8_t, 16> part_limits;
    part_limits.fill(16);
    double peak = 0.0;
    double applied = 0.0;
    bool limited = false;
    std::vector<Performer::LoopChoice> loop_choices;
    std::map<uint32_t, uint8_t> tie_velocities;
    bool ties_known = false;
    Performance performance;
    for (int pass = 0; pass < 4; pass++)
    {
        Performer performer(sound, banks, options, LevelScale(peak), part_limits, loop_choices, tie_velocities);
        performance = performer.Run();
        applied = peak;
        loop_choices = performer.LoopChoices();
        if (!ties_known)
        {
            ties_known = true;
            if (!performer.TieVelocities().empty())
            {
                tie_velocities = performer.TieVelocities();
                continue;
            }
        }

        bool parts_fit = true;
        if (!limited)
        {
            const std::array<uint8_t, 16> fitted = FitParts(performance, performer.PartsUsed());
            for (std::size_t t = 0; t < 16; t++)
            {
                parts_fit = parts_fit && fitted[t] >= performer.PartsUsed()[t];
            }
            if (!parts_fit)
            {
                part_limits = fitted;
                limited = true;
            }
        }
        if (parts_fit && LevelScale(performer.LevelPeak()) == LevelScale(peak))
        {
            break;
        }

        peak = performer.LevelPeak();
    }

    if (LevelScale(applied) < 1.0)
    {
        // CC11 acts squared. Scaling it by 127 / peak makes every note 40 log10(peak / 127) dB quieter.
        performance.level_cut_db = 40.0 * std::log10(applied / 127.0);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "volume exceeds MIDI range (SoundFont playback attenuated by %.1f dB; SFZ restores the gain)",
                      performance.level_cut_db);
        performance.approximations[what] = NoteCount(performance);
    }

    return performance;
}

uint32_t NoteCount(const Performance& perf)
{
    uint32_t notes = 0;
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOn)
            {
                notes++;
            }
        }
    }

    return notes;
}

} // namespace dolphinsf2
