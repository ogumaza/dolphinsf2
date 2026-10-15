// SPDX-License-Identifier: MIT

// JAudio2's oscillators (JASOscillator), the envelopes that move a note's volume or pitch over time, and their
// SoundFont 2 and SFZ equivalents.

#pragma once

#include <cstdint>
#include <vector>

#include "seqlib/sf2/units.h"

#include "jaudio.h"

namespace dolphinsf2
{

// The rate the DSP program mixes at: the GameCube's "32 kHz", 108 MHz / 3372.
constexpr double kOutputRate = 32028.5;

// JAudio2 updates its sequences and channels every 80 output samples, a sub-frame of the DSP program's 560-sample
// frame.
constexpr int kSubFrameSamples = 80;
constexpr double kSubFrameSeconds = kSubFrameSamples / kOutputRate;

// The output rate as JAudio2 calculates it (JASDriver::setOutputRate): 32000 Hz times 1.0008897, in single precision
// (Four Swords Adventures' main.dol keeps both at 0x8053b080 and 0x8053b088). Tempo, oscillators and LFOs scale by it.
constexpr float kDacRate = 32000.0f * 1.0008897f;

// An oscillator's table moves on by this much each sub-frame at a rate of 1 (JASChannel::updateDSPChannel, with 48000
// at 0x8053b084). 600 units make a second.
constexpr float kOscStep = 48000.0f / kDacRate;

// An oscillator's state as a note plays it (JASOscillator).
class OscillatorState
{
public:
    // Start `osc`'s attack table. A null table holds the level at 1 until the release.
    void Start(const Oscillator& osc);

    // Move on by `step` units times the oscillator's rate. Each sub-frame does this (incCounter).
    void Advance(float step);

    // Start the release: the release table, or a direct release, whose time is in the low 14 bits and whose curve is in
    // the top two. With neither, the oscillator takes `default_release` as its direct release. Some builds of
    // JASOscillator::release do that. With 0 for that too, it stops at once.
    void Release(uint16_t direct_release, uint16_t default_release);

    // The oscillator's output: its level times its scale, plus its vertex (getValue).
    float Value() const;

    bool Valid() const
    {
        return osc_ != nullptr;
    }

    bool Stopped() const
    {
        return state_ == State::kStopped;
    }

    bool Releasing() const
    {
        return state_ == State::kRelease || state_ == State::kDirectRelease;
    }

    bool Holding() const
    {
        return state_ == State::kHold;
    }

private:
    enum class State
    {
        kStopped,
        kAttack,
        kHold,
        kRelease,
        kDirectRelease,
    };

    // Reach the points that the counter has passed, and interpolate the level (update).
    void Update();

    // Interpolate between the segment's start and its target along the segment's curve (updateCurrentValue).
    void Interpolate(float duration);

    const Oscillator* osc_ = nullptr;
    State state_ = State::kStopped;
    float counter_ = 0.0f; // units into the current segment
    float level_ = 0.0f;
    float from_ = 0.0f;   // the segment's start level
    float target_ = 0.0f; // the segment's target level
    std::size_t point_ = 0;
    int curve_ = 0;
    uint16_t direct_release_ = 0;
};

// The level over time of a note's volume oscillators, multiplied together, from note-on with no release, and after a
// release. The release starts from the held level. Where the curve stops or falls to silence, it starts from the peak.
struct EnvelopeCurve
{
    std::vector<float> attack;  // each sub-frame's level from note-on, up to where it holds or stops
    bool holds = false;         // it holds at its last level until the release
    std::vector<float> release; // each sub-frame's level after the release, until it stops
};

// Trace the curve of `volume_oscillators` (their targets are the volume) with `direct_release` (0 for none), and the
// game's `default_release` for an oscillator without a release (OscillatorState::Release). No oscillator gives a level
// of 1 that stops at the release. A direct release fades it instead.
EnvelopeCurve TraceEnvelope(const std::vector<const Oscillator*>& volume_oscillators, uint16_t direct_release,
                            uint16_t default_release);

// SoundFont volume envelope generators (timecents; sustain in centibels of attenuation), with the attenuation that
// brings the envelope's peak to full level.
struct Sf2Envelope
{
    int delay = -12000, attack = -12000, hold = -12000, decay = -12000, sustain = 0, release = -12000;
    double peak_db = 0.0; // the envelope's highest level
};

// Fit SoundFont's envelope to a traced curve. Like JAudio's linear segments, the attack rises linearly in amplitude.
// The decay and release fall linearly in dB. Each is fitted to reach the level the game's curve reaches when it has
// fallen half its range in dB, or 20 dB if it falls to silence.
Sf2Envelope ConvertEnvelope(const EnvelopeCurve& curve);

// SFZ amplitude envelope (ampeg_*): times in seconds, sustain as a percentage of full amplitude.
struct SfzEnvelope
{
    double delay = 0.0, attack = 0.0, hold = 0.0, decay = 0.0, sustain = 100.0, release = 0.0;
    double peak_db = 0.0;
};

// Convert to sfizz's SFZ envelope, fitted as for SoundFont. Over their time, sfizz's decays and releases fall by a
// factor of e^-9 (78.17 dB).
SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeCurve& curve);

using seqlib::sf2::ToTimecents;

} // namespace dolphinsf2
