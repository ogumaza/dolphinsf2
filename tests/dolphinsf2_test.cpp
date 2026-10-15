// SPDX-License-Identifier: MIT

// Unit tests for dolphinsf2, without game data. The tests build small JAudio2 archives and MusyX song groups in memory.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/dol.h"
#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "ntdlib/musyx/song.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "convert.h"
#include "envelope.h"
#include "game.h"
#include "instruments.h"
#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"
#include "musyx_player.h"
#include "musyx_voices.h"
#include "performance.h"
#include "wave.h"

namespace
{

using namespace dolphinsf2;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;
using xsflib::testing::failures;

namespace files = ntdlib::testing::jaudio_files;
using files::Bytes;

// The test game's wave archive, test.aw, by its name.
std::optional<std::vector<uint8_t>> ReadTestAw(const std::string& name)
{
    return name == "test.aw" ? std::optional(files::AwFile()) : std::nullopt;
}

// A test game: its archive and the banks and waves it plays.
struct TestGame
{
    explicit TestGame(const std::vector<uint8_t>& bank, const std::vector<std::vector<uint8_t>>& sequences)
        : archive(SoundArchive::Parse(files::StartUpArchive(bank, sequences))), banks(archive, ReadTestAw)
    {
    }

    SequenceSound Sound(uint32_t index) const
    {
        return {"S" + std::to_string(index), archive.Collection(), *archive.CollectionSequence(0, index), 1.0f};
    }

    SoundArchive archive;
    BankSet banks;
};

// Sequence data addresses commands from the start of the collection that holds it. The first sequence of a collection
// of one starts at kFirstSequence.
constexpr uint32_t kFirstSequence = 20;

// Where the tests put the commands of a track that the first track opens.
constexpr uint32_t kChild = kFirstSequence + 0x20;

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

    // A gate note of fewer than 128 ticks. Bit 7 of `flags` sweeps from the latest key, and bit 6 ties the next note to
    // this one.
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

    // The effect send. It feeds the game's echo.
    Sequence& FxMix(uint32_t byte)
    {
        b_.U8(0xb8).U8(2).U8(byte);

        return *this;
    }

    // Oscillator `osc`'s scale, in 16383ths.
    Sequence& OscillatorScale(uint32_t osc, uint32_t scale)
    {
        b_.U8(0xe7).U8(osc).U16(scale);

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

// Join the problems that one of seqlib's checkers reports, or return "" if there are none.
std::string Joined(const std::vector<std::string>& problems)
{
    std::string s;
    for (const std::string& p : problems)
    {
        s += (s.empty() ? "" : "; ") + p;
    }

    return s;
}

void TestDecodeWave()
{
    const SoundArchive archive = SoundArchive::Parse(files::StartUpArchive(files::BankV0File(0, 100), {}));
    const WaveSystem* ws = archive.FindWaveSystem(0);

    const std::optional<Pcm> pcm =
        ws != nullptr ? std::optional(DecodeWave(ws->waves.at(0), files::AwFile())) : std::nullopt;

    Check(pcm && pcm->Length() == 64 && pcm->loop && pcm->channels[0][8] == 0x4000, "PCM16 decodes big-endian");
}

void TestFindSound()
{
    const Bank v0 = Bank::Parse(files::BankV0File(7, 100), 0);
    const Bank v1 = Bank::Parse(files::BankV1File(129, 0, 600), 0);

    const std::optional<NoteSound> drum = FindSound(v0, 0xe4, 36, 100);
    const std::optional<NoteSound> low = FindSound(v1, 0, 60, 40);
    const std::optional<NoteSound> high = FindSound(v1, 0, 60, 100);

    Check(drum && drum->drum && drum->pitch == 2.0f && drum->volume == 0.5f, "a drum plays its region");
    Check(drum && std::abs(drum->pan - 1.0f) < 1e-6f && drum->direct_release == 200, "PER2 gives pan and release");
    Check(!FindSound(v0, 0xe4, 37, 100), "a drum set plays only its keys");
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

    s.Release(0, 0);
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
    // Without a release table, an oscillator stops at once. A direct release fades it. So does a game's default
    // release, 0x10 in Four Swords Adventures: linearly, over 16 units.
    Oscillator bare;
    bare.attack = {{0, 0, 0x7fff}, {14, 0, 0}};
    OscillatorState cut;
    OscillatorState fade;
    OscillatorState fallback;
    cut.Start(bare);
    fade.Start(bare);
    fallback.Start(bare);

    cut.Release(0, 0);
    fade.Release(100, 0);
    fallback.Release(0, 0x10);
    fallback.Advance(8.0f);
    const float halfway = fallback.Value();
    fallback.Advance(9.0f);

    Check(cut.Stopped(), "no release table stops at once");
    Check(!fade.Stopped(), "a direct release fades");
    Check(std::abs(halfway - 0.5f) < 0.001f, "the default release falls linearly");
    Check(fallback.Stopped(), "the default release stops at its end");
}

void TestEnvelopeFit()
{
    Oscillator osc;
    osc.attack = {{0, 0, 0x7fff}, {0, 600, 0x7fff}, {14, 0, 0}};
    osc.release = {{0, 600, 0}, {15, 0, 0}};

    const EnvelopeCurve curve = TraceEnvelope({&osc}, 0, 0);
    const Sf2Envelope e = ConvertEnvelope(curve);

    Check(curve.holds, "the curve holds");
    Check(e.attack == -12000 && e.sustain == 0, "an instant attack to full sustain");
    // A linear fall over a second reaches -20 dB at 0.9 s: 100 dB at 4.5 s.
    Check(std::abs(e.release - ToTimecents(4500.0, -12000, 8000)) <= 20, "a linear release fits at -20 dB");
}

void TestReleaseAfterDecayToSilence()
{
    // Two attacks that fall to silence over 3 s. One holds there and one stops. Their notes are released before then,
    // so the release table gives their release.
    Oscillator holds;
    holds.attack = {{0, 0, 0x7fff}, {0, 1800, 0}, {14, 0, 0}};
    holds.release = {{0, 600, 0}, {15, 0, 0}};
    Oscillator stops = holds;
    stops.attack.back().mode = 15;

    const Sf2Envelope held = ConvertEnvelope(TraceEnvelope({&holds}, 0, 0));
    const Sf2Envelope stopped = ConvertEnvelope(TraceEnvelope({&stops}, 0, 0));

    const int release = ToTimecents(4500.0, -12000, 8000);
    Check(std::abs(held.release - release) <= 20, "a curve that holds in silence keeps its release");
    Check(std::abs(stopped.release - release) <= 20, "a curve that stops keeps its release");
}

void TestPerformance()
{
    // The first track opens track 0. Track 0 plays two notes and finishes.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).NoteOn(64, 1, 80).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

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

void TestOscillatorScaleArguments()
{
    // Track 0 scales its pitch envelope by 1.1 with 0xe7, then plays one note. Twilight Princess's command table gives
    // 0xe7 a byte and a 16-bit value. A parser that reads no arguments for 0xe7 plays the bytes 01 46 66 as key 1.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).OscillatorScale(1, 0x4666).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});

    int notes = 0;
    for (std::size_t slot = 0; slot < perf.tracks.size(); slot++)
    {
        notes += Count(perf, slot, EventKind::kNoteOn);
    }
    Check(notes == 1, "0xe7 takes a byte and a 16-bit value");
}

void TestSweepAndTie()
{
    // Track 0 sweeps up an octave from the latest key over a beat, and ties the next note to it. That note sweeps on
    // from where the first ended, at half the velocity.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).LatestKey(60).Gate(72, 0xc0, 100, 48).Gate(72, 0x80, 50, 48).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

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
    rising.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (rising.Here() < kChild)
    {
        rising.Byte(0xfe);
    }
    rising.Bank(0).Program(0).Gate(60, 0x40, 50, 24).Gate(60, 0x00, 100, 24).Finish();
    TestGame rising_game(files::BankV0File(0, 60), {rising.Data()});

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
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
        while (s.Here() < kChild)
        {
            s.Byte(0xfe);
        }
        s.Bank(0).Program(0).Gate(60, 0x40, 50, ticks).Gate(60, 0x00, 127, 24).Finish();
        TestGame game(files::BankV1File(0, attack, 600), {s.Data()});

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
    s.OpenTrack(0, kChild).Timebase(24).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).Gate(60, 0x00, 100, 24).Gate(62, 0x00, 100, 24).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

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
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Wait(200);
    s.Jump(s.Here() - 4);
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0);
    const uint32_t loop = s.Here();
    s.NoteOn(60, 1, 100).Wait(48).NoteOff(1).NoteOn(62, 1, 100).Wait(48).NoteOff(1).Jump(loop);
    TestGame game(files::BankV0File(0, 60), {s.Data()});

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
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
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
    TestGame game(files::BankV0File(0, 60), {data});
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
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0xe4).Volume(0x40).NoteOn(36, 1, 127).Wait(10).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

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

void TestTailsKeepTheirSettings()
{
    // Track 0 releases a note, lowers its volume 12 ticks later while the release still sounds, then plays a note.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Wait(12).Volume(0x20).NoteOn(64, 1, 100);
    s.Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 600), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});

    std::optional<uint8_t> first, second;
    bool tail_changed = false;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            (e.key == 60 ? first : second) = e.part;
        }
        tail_changed = tail_changed || (e.kind == EventKind::kVolume && e.tick > 48 && e.part == first);
    }
    Check(first && second && *first != *second, "the next note plays on another part while the release sounds");
    Check(!tail_changed, "the release keeps its volume");
}

void TestHeldNoteKeepsItsPart()
{
    // Track 0 plays two notes, releases one, and lowers its volume 12 ticks later while it still holds the other.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).NoteOn(60, 1, 100).NoteOn(64, 2, 100).Wait(48).NoteOff(1).Wait(12).Volume(0x20);
    s.Wait(48).NoteOff(2).Finish();
    TestGame game(files::BankV0File(0, 600), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {});

    std::optional<uint8_t> first, second, volume;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            (e.key == 60 ? first : second) = e.part;
        }
        if (e.kind == EventKind::kVolume && e.tick > 48)
        {
            volume = e.part;
        }
    }
    const std::string approximation =
        "released notes take later settings of their track while it holds a note on their part";
    Check(first && second && *first == *second, "both notes play on one part");
    Check(volume && volume == second, "the held note takes the track's volume");
    Check(perf.approximations.contains(approximation), "the release takes the volume too, as an approximation");
}

void TestDefaultKeepsEveryNote()
{
    // Nine tracks each hold seven notes from tick 0 to tick 48. The first track plays seven more from tick 12: 70 notes
    // at once, six more than the game's 64 voices.
    constexpr uint32_t kNotes = kFirstSequence + 0x80;
    Sequence s;
    for (uint32_t t = 0; t < 9; t++)
    {
        s.OpenTrack(t, kNotes);
    }
    s.Timebase(48).Tempo(120).Bank(0).Program(0).Wait(12);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOn(59 + id, id, 100);
    }
    s.Wait(48);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOff(id);
    }
    s.Finish();
    while (s.Here() < kNotes)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOn(59 + id, id, 100);
    }
    s.Wait(48);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOff(id);
    }
    s.Finish();
    TestGame game(files::BankV0File(0, 600), {s.Data()});
    PerformOptions game_voices;
    game_voices.voice_limit = 64;

    const Performance kept = Perform(game.Sound(0), game.banks, {});
    const Performance limited = Perform(game.Sound(0), game.banks, game_voices);

    // The notes that end before tick 48, the earliest note-off.
    const auto cut_short = [](const Performance& perf)
    {
        int n = 0;
        for (const std::vector<TrackEvent>& track : perf.tracks)
        {
            for (const TrackEvent& e : track)
            {
                n += e.kind == EventKind::kNoteOff && e.tick < 48 ? 1 : 0;
            }
        }
        return n;
    };
    const std::string approximation = "notes cut when the game's 64 voices ran out";
    Check(cut_short(kept) == 0 && !kept.approximations.contains(approximation),
          "by default every note plays to its end");
    Check(cut_short(limited) == 6 && limited.approximations.contains(approximation),
          "with --voices 64, six notes are cut as in the game");
}

void TestConversion()
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
    while (s.Here() < kChild)
    {
        s.Byte(0xfe);
    }
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Conversion c = ConvertSequence(game.Sound(0), game.banks, {});

    const std::string midi_problems = Joined(seqlib::midi::Problems(c.midi));
    const std::string sf2_problems = Joined(seqlib::sf2::Check(c.sf2));
    const seqlib::midi::File m = midi_problems.empty() ? seqlib::midi::Read(c.midi) : seqlib::midi::File{};
    Check(c.midi.size() > 14 && std::memcmp(c.midi.data(), "MThd", 4) == 0, "the MIDI file has a header");
    Check(
        c.sf2.size() > 12 && std::memcmp(c.sf2.data(), "RIFF", 4) == 0 && std::memcmp(c.sf2.data() + 8, "sfbk", 4) == 0,
        "the SoundFont is a RIFF sfbk");
    Check(c.samples == 1, "the SoundFont holds the one wave it plays");
    Check(c.sfz.files.size() == 1 && c.sfz.samples.size() == 1, "one SFZ file and its sample");
    Check(midi_problems.empty() && seqlib::midi::Notes(m).size() == 1,
          "an independent reader reads the MIDI file's note: " + midi_problems);
    Check(sf2_problems.empty(), "an independent reader accepts the SoundFont: " + sf2_problems);
    const std::string sfz_problem = ntdlib::testing::SfzProblem(c.sfz, seqlib::midi::TrackNames(m));
    Check(sfz_problem.empty(), "the SFZ files match the MIDI file: " + sfz_problem);
}

// macOS writes an AppleDouble file, named "._" and the file's name, beside each file that it copies to a FAT or exFAT
// drive. The search for the sequence archive leaves those files out.
void TestFolderWithAppleDoubleFiles()
{
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / ("dolphinsf2_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(folder / "Seqs");
    const files::SoundTableBlocks sounds = files::SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}}}}}});
    const std::vector<uint8_t> music = Sequence(0).Tempo(120).Finish().Data();
    xsflib::WriteFile(folder / "test.baa", files::StartUpArchive(files::BankV0File(0, 60), {}, sounds));
    xsflib::WriteFile(folder / "Seqs" / "test.arc", files::RarcFile({{7, "test.bms", music}}));

    // An AppleDouble file starts with its magic number. Its name sorts before the name of the file it describes.
    xsflib::WriteFile(folder / "Seqs" / "._test.arc", Bytes().U32(0x00051607).U32(0x00020000).Data());

    const Game game = Game::FromFolder(folder / "test.baa");

    Check(game.Warnings().empty(), "an AppleDouble file is no sequence archive: " + Joined(game.Warnings()));
    Check(game.Sequences().size() == 1 && game.Sequences()[0].name == "BGM_FIELD", "the music is in the real archive");

    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

// Both releases of Four Swords Adventures play their system sounds, category 0, at 0.7, and release an oscillator
// without a release over 0x10. Their echo repeats a sound every 64 sub-frames. Another game's start-up archive gets the
// defaults.
void TestGameRules()
{
    const GameRules usa = RulesOf("8c55097f2cc9f5800fd8ab96da4e3b09727e0a39");
    const GameRules japan = RulesOf("aab07cea25ba957374524c4f29d36b72e6d36a67");
    const GameRules other = RulesOf(std::string(40, '0'));

    Check(usa.category_volumes == std::map<uint8_t, float>{{0, 0.7f}}, "the USA release plays category 0 at 0.7");
    Check(usa.default_release == 0x10, "the USA release releases a bare oscillator over 0x10");
    Check(usa.echo && usa.echo->delay == 64 && usa.echo->feedback == 0x3fff / 32768.0f && usa.echo->volume == 0.75f &&
              usa.echo->voice_level == 0.732421875f,
          "the USA release's echo has a delay of 64 sub-frames, a filter of 0x3fff and a volume of 0x6000");
    Check(japan.category_volumes == usa.category_volumes && japan.default_release == 0x10 && japan.echo &&
              japan.echo->delay == 64,
          "the Japanese release has the same rules");
    Check(other.category_volumes.empty() && other.default_release == 0 && !other.echo,
          "another game gets the defaults");
}

// A note that sends to Four Swords Adventures' echo lengthens its sequence by the repeats that the game plays at 12 or
// above. The test wave peaks at 16384. At a send of 22/127 and the centre's pan gain, the line takes in 1,470 a side.
// The first repeat plays at 0.375 of that, 551. The 6th plays at 17, and the 7th at 8.6. A note of 12 ticks lasts less
// than a delay. No repeat overlaps it. A note of 48 ticks lasts three delays. The line then holds the send and its
// earlier repeats. They add by power to 1.15 times the send. Its 6th repeat after the note is the last at 12 or above.
// Added as levels, they would give 1.875 times the send and a 7th repeat. A note without a send gets no echo.
void TestEchoLengthensTheEnd()
{
    const auto note = [](uint32_t send, uint32_t ticks)
    {
        Sequence s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish();
        while (s.Here() < kChild)
        {
            s.Byte(0xfe);
        }
        s.Bank(0).Program(0).FxMix(send).NoteOn(60, 1, 127).Wait(ticks).NoteOff(1).Finish();

        return s.Data();
    };
    const auto repeats = [](const std::vector<uint8_t>& sequence)
    {
        TestGame game(files::BankV0File(0, 0), {sequence});
        SequenceSound with_echo = game.Sound(0);
        with_echo.echo = RulesOf("8c55097f2cc9f5800fd8ab96da4e3b09727e0a39").echo;
        const Performance dry = Perform(game.Sound(0), game.banks, {});
        const Performance wet = Perform(with_echo, game.banks, {});

        return (wet.seconds - dry.seconds) / (64 * kSubFrameSeconds);
    };

    const double short_note = repeats(note(22, 12));
    const double long_note = repeats(note(22, 48));
    const double unsent = repeats(note(0, 12));

    Check(std::abs(short_note - 6.0) < 0.05, "a short note's echo adds six repeats: " + std::to_string(short_note));
    Check(std::abs(long_note - 6.0) < 0.05, "a long note's echo adds six repeats: " + std::to_string(long_note));
    Check(unsent == 0.0, "a note without a send gets no echo");
}

// A DOL with `data` in its first data section, loaded at `address`.
std::vector<uint8_t> DolWithData(uint32_t address, const std::vector<uint8_t>& data)
{
    constexpr std::size_t kHeader = 0x100;
    std::vector<uint8_t> dol(kHeader);
    xsflib::StoreBe32(&dol[0x1c], static_cast<uint32_t>(kHeader));
    xsflib::StoreBe32(&dol[0x64], address);
    xsflib::StoreBe32(&dol[0xac], static_cast<uint32_t>(data.size()));
    dol.insert(dol.end(), data.begin(), data.end());

    return dol;
}

// The bytes of `text`, without a terminating zero.
std::vector<uint8_t> TextBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// A file's number in a song list is twice its index. The index counts in base 26, with the letters A to Z, through
// paths of three letters.
void TestMusyxPaths()
{
    constexpr uint32_t kFgh = 2 * ((5 * 26 + 6) * 26 + 7);
    Check(musyx::PathOf(kFgh) == "F/G/H" && musyx::PathOf(kFgh + 1) == "F/G/H",
          "the number's lowest bit names no other file");
    Check(musyx::PathOf(kFgh + 2) == "F/G/I" && musyx::PathOf(kFgh + 52) == "F/H/H", "the last letter counts first");
    Check(musyx::PathOf(kFgh + 6) == "F/G/K", "a group's fourth file is 6 past its first");
    Check(musyx::PathOf(2 * (26 * 26 * 26 - 1)) == "Z/Z/Z", "the last path");
}

// Song lists in both of the games' forms. The first list's entries of 16 bytes give the address of each song's name.
// The second list's entry of 0x24 bytes holds its name in its first 0x18 bytes. Each entry ends with the song's file
// number, its ID in the low half of a word, and its volume in the top byte of the last word.
void TestMusyxSongLists()
{
    constexpr uint32_t kBase = 0x80100000;
    Bytes b;
    b.U32(kBase + 0x40).U32(8112).U32(3).U32(90u << 24);
    b.U32(kBase + 0x50).U32(8115).U32(7).U32(127u << 24);
    b.PadTo(0x40).Append(TextBytes("opening.sng")).PadTo(0x50).Append(TextBytes("theme")).PadTo(0x60);
    b.Append(TextBytes("fanfare.sng")).PadTo(0x78).U32(8162).U32(0x10005).U32(0x40123456);
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<musyx::GameSong> by_address = musyx::ReadSongList(*dol, {kBase, 2, 0x10, false});
    const std::vector<musyx::GameSong> inline_names = musyx::ReadSongList(*dol, {kBase + 0x60, 1, 0x24, true});
    const std::string past_the_data = ErrorOf([&] { musyx::ReadSongList(*dol, {kBase + 0x100, 1, 0x10, false}); });

    if (Check(by_address.size() == 2, "the list of names by address has two songs"))
    {
        const musyx::GameSong& opening = by_address[0];
        const musyx::GameSong& theme = by_address[1];
        Check(opening.name == "opening" && opening.path == "G/A/A" && opening.id == 3 && opening.volume == 90,
              "the first song: its name without .sng, its file, its ID and its volume");
        Check(theme.name == "theme" && theme.path == "G/A/B" && theme.id == 7 && theme.volume == 127,
              "a name without .sng stays whole");
    }
    if (Check(inline_names.size() == 1, "the list of names in its entries has one song"))
    {
        const musyx::GameSong& fanfare = inline_names[0];
        Check(fanfare.name == "fanfare" && fanfare.path == "G/A/Z" && fanfare.id == 5 && fanfare.volume == 0x40,
              "the entry's name, and the low half of the ID's word and the top byte of the volume's");
    }
    Check(past_the_data == "main.dol has no data at the song list's address",
          "a list past the DOL's data fails: " + past_the_data);
}

// MusyX turns a DLS envelope's timecents into whole milliseconds, and keeps their low 16 bits. The table's lowest
// value stands for no time.
void TestDlsMilliseconds()
{
    Check(musyx::DlsMilliseconds(0.0) == 1000.0 && musyx::DlsMilliseconds(1200.0) == 2000.0 &&
              musyx::DlsMilliseconds(-1200.0) == 500.0,
          "1200 timecents double the time");
    Check(musyx::DlsMilliseconds(100.0) == 1059.0, "the time is truncated to whole milliseconds");
    Check(musyx::DlsMilliseconds(-1.0e9) == 0.0, "the lowest value is no time");
    Check(musyx::DlsMilliseconds(7200.0) == 64000.0 && musyx::DlsMilliseconds(8400.0) == 128000.0 - 65536.0,
          "a time keeps its low 16 bits");
    Check(musyx::DlsMilliseconds(38400.0) == 65535.0, "a time past 31 bits saturates before it's cut to 16 bits");
}

// A SoundMacro step with command `op` and bytes 1 to 7 of its two words.
ntdlib::musyx::MacroStep Step(ntdlib::musyx::MacroOp op, std::array<uint32_t, 7> b = {})
{
    ntdlib::musyx::MacroStep step;
    step.word0 = static_cast<uint32_t>(op) | b[0] << 8 | b[1] << 16 | b[2] << 24;
    step.word1 = b[3] | b[4] << 8 | b[5] << 16 | b[6] << 24;

    return step;
}

// A song group with one program. Program 0 plays macro 1. The macro picks the DLS volume curve without ITD, sets DLS
// envelope 2, starts sample 1, waits for the note's release and releases the envelope. Envelope 2 starts at full level
// at once, holds it and releases over 100 ms. Sample 1 is a PCM16 sine of 3200 samples at 32000 Hz, at key 60, that
// loops from sample 800 to its end. Song 5's MIDI setup gives channel 0 a volume of 100.
musyx::SongGroup TestSongGroup()
{
    using ntdlib::musyx::MacroOp;
    musyx::SongGroup group;
    group.group.pages.push_back({.object = 1, .priority = 0, .max_voices = 8, .program = 0});
    ntdlib::musyx::MidiSetup setup;
    setup.song = 5;
    setup.channels[0].volume = 100;
    group.group.midi_setups.push_back(setup);

    // The macro, and its envelope's table. The table is little-endian. Its attack and decay of INT32_MIN take no time,
    // its sustain of 0x1000 is full level, its release takes 100 ms, and its scales of INT32_MIN are off.
    group.pool.macros[1] = {
        Step(MacroOp::kModeSelect, {1, 1}),
        Step(MacroOp::kSetAdsr, {2, 0, 1}),
        Step(MacroOp::kStartSample, {1}),
        Step(MacroOp::kWaitMs, {1, 0, 0, 0, 0, 0xff, 0xff}),
        Step(MacroOp::kKeyOff),
        Step(MacroOp::kEnd),
    };
    group.pool.tables[2] = {0, 0, 0, 0x80, 0, 0, 0, 0x80, 0x00, 0x10, 100, 0, 0, 0, 0, 0x80, 0, 0, 0, 0x80};

    ntdlib::musyx::SampleInfo sample;
    sample.id = 1;
    sample.key = 60;
    sample.rate = 32000;
    sample.format = ntdlib::musyx::SampleFormat::kPcm16;
    sample.length = 3200;
    sample.loop_start = 800;
    sample.loop_length = 2400;
    group.samples.push_back(sample);
    Bytes data;
    for (uint32_t i = 0; i < sample.length; i++)
    {
        data.U16(static_cast<uint16_t>(std::lround(8000.0 * std::sin(i * 2.0 * std::numbers::pi / 80.0))));
    }
    group.sample_data = data.Data();

    return group;
}

// A song of one track on channel 0. The track plays pattern 0 at tick 0 with a volume of 100, then loops back to it or
// ends at tick 768. The pattern plays key 60 for half a beat. With `volume_change`, it then sets the channel's volume
// to 64.
ntdlib::musyx::Song TestSong(bool loops, bool volume_change)
{
    ntdlib::musyx::Song song;
    song.tempo = 120;
    ntdlib::musyx::Track track;
    track.entries.push_back({.tick = 0, .pattern = 0, .volume = 100});
    track.entries.push_back(
        {.tick = 768, .pattern = loops ? ntdlib::musyx::kLoopBack : ntdlib::musyx::kEndOfTrack, .loop_target = 0});
    song.tracks.push_back(track);

    ntdlib::musyx::Pattern pattern;
    pattern.events.push_back(
        {.tick = 0, .kind = ntdlib::musyx::PatternEventKind::kNote, .key = 60, .velocity = 100, .length = 192});
    if (volume_change)
    {
        pattern.events.push_back(
            {.tick = 192, .kind = ntdlib::musyx::PatternEventKind::kController, .controller = 7, .value = 64});
    }
    pattern.end_tick = 768;
    song.patterns.push_back(pattern);

    return song;
}

// The values of the CC7 events of channel 0.
std::vector<int> VolumesOf(const Performance& perf)
{
    std::vector<int> volumes;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volumes.push_back(e.value);
        }
    }

    return volumes;
}

// The game's song table gives each song a volume. MusyX multiplies the channels' volumes by it inside the volume curve.
// Each CC7 then takes the song's volume in 127ths: the MIDI setup's 100, the track entry's 100 and the pattern's 64. A
// game's level multiplies them too.
void TestMusyxSongVolume()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group);
    const ntdlib::musyx::Song song = TestSong(false, true);

    const Performance full = musyx::PlaySong(song, {"full", "G/A/A", 5, 127}, instruments, {});
    const Performance quieter = musyx::PlaySong(song, {"quieter", "G/A/A", 5, 90}, instruments, {});
    const Performance lower = musyx::PlaySong(song, {"lower", "G/A/A", 5, 127, 0.8064}, instruments, {});
    const Performance both = musyx::PlaySong(song, {"both", "G/A/A", 5, 90, 0.8064}, instruments, {});

    Check(VolumesOf(full) == std::vector<int>{100, 64}, "a song at 127 keeps its channels' volumes");
    Check(VolumesOf(quieter) == std::vector<int>{71, 45}, "a song at 90 scales them by 90/127, rounded");
    Check(VolumesOf(lower) == std::vector<int>{81, 52}, "a game's level of 0.8064 scales them by that factor");
    Check(VolumesOf(both) == std::vector<int>{57, 37}, "the song's volume and the game's level multiply");
}

// A song that loops plays at the output's rate, marks its loop and converts to files that independent readers accept.
void TestMusyxSong()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group);

    const Conversion c = musyx::ConvertSong(TestSong(true, false), {"loop", "G/A/A", 5, 127}, instruments, {});

    const Performance& perf = c.performance;
    const double bpm = 120.0 * musyx::kOutputRate / musyx::kNominalRate;
    Check(!perf.tempo.empty() && std::abs(perf.tempo[0].bpm - bpm) < 0.01,
          "the song plays 0.089% fast, at the output's rate: " +
              (perf.tempo.empty() ? std::string("no tempo") : std::to_string(perf.tempo[0].bpm)));
    Check(perf.loop && perf.loop->start == 0 && perf.loop->end == 768 && perf.end_tick == 768,
          "the loop runs from tick 0 to 768");
    Check(Count(perf, 0, EventKind::kNoteOn) == 1 && Count(perf, 0, EventKind::kNoteOff) == 1,
          "the pass plays its note once");
    Check(perf.approximations.empty(), "the conversion reports no approximation");
    Check(c.samples == 1 && c.sfz.files.size() == 1 && c.sfz.samples.size() == 1,
          "the SoundFont and the SFZ files hold the one sample");
    Check(ntdlib::testing::CheckConversion(c, "loop") == 0, "independent readers accept the files");
}

// A tempo table starts again in each pass. The run goes on past the end to find the loop, but the performance keeps
// no tempo change after the end. The table here slows the song to 60 beats a minute halfway through each pass.
void TestMusyxTempoTable()
{
    const musyx::SongGroup group = TestSongGroup();
    musyx::Instruments instruments(group);
    ntdlib::musyx::Song song = TestSong(true, false);
    song.tempo_changes = {{.tick = 0, .tempo = 120}, {.tick = 384, .tempo = 60}};

    const Performance perf = musyx::PlaySong(song, {"tempo", "G/A/A", 5, 127}, instruments, {});

    Check(perf.loop && perf.loop->start == 0 && perf.loop->end == 768 && perf.end_tick == 768,
          "the loop runs from tick 0 to 768");
    std::string ticks;
    for (const Performance::Tempo& t : perf.tempo)
    {
        ticks += " " + std::to_string(t.tick);
    }
    Check(ticks == " 0 384 768", "the tempo changes at ticks 0, 384 and 768, the pass's end:" + ticks);
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

        failures += ntdlib::testing::CheckConversion(c, sound.name);
        looping += c.performance.loop ? 1 : 0;
    }

    std::printf("%s: %zu sequences converted and checked, %d loop\n", path, game->Sequences().size(), looping);
}

} // namespace

int main(int argc, char** argv)
{
    TestDecodeWave();
    TestFindSound();
    TestOscillator();
    TestOscillatorWithoutReleaseTable();
    TestEnvelopeFit();
    TestReleaseAfterDecayToSilence();
    TestPerformance();
    TestOscillatorScaleArguments();
    TestSweepAndTie();
    TestTieBelowHearing();
    TestGateTickRate();
    TestLoop();
    TestPortsAndRegisters();
    TestDrumAndVolume();
    TestTailsKeepTheirSettings();
    TestHeldNoteKeepsItsPart();
    TestDefaultKeepsEveryNote();
    TestConversion();
    TestFolderWithAppleDoubleFiles();
    TestGameRules();
    TestEchoLengthensTheEnd();
    TestMusyxPaths();
    TestMusyxSongLists();
    TestDlsMilliseconds();
    TestMusyxSongVolume();
    TestMusyxSong();
    TestMusyxTempoTable();
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
