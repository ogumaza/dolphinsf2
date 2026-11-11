// SPDX-License-Identifier: MIT

// The performance that MusyX's sequencer leaves (see musyx_player.cpp): the ends and bends of the notes, the marks
// of the loop and the cuts of the notes' tails.

#include <algorithm>
#include <cmath>

#include "musyx_player_parts.h"

namespace dolphinsf2::musyx::player
{

namespace
{

// Whether a reset of all controllers (CC121) sets events of `kind` back to their defaults. RP-015 keeps the program,
// the bank, the volume, the pan, the sound controllers 70 to 79 and the effect depths 91 to 95. Other players reset
// more of the controllers that RP-015 doesn't name, so those count as reset.
bool ResetsToDefault(EventKind kind)
{
    if (kind == EventKind::kPreset || kind == EventKind::kVolume || kind == EventKind::kPan ||
        kind == EventKind::kReverb || kind == EventKind::kChorus)
    {
        return false;
    }
    if (!seqlib::midi::IsControlKind(kind))
    {
        return true;
    }

    const int controller = static_cast<int>(kind) - seqlib::midi::kFirstControlKind;
    return controller != 0 && controller != 32 && (controller < 70 || controller > 79) &&
           (controller < 91 || controller > 95) && controller < 120;
}

} // namespace

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

VoiceEnds Player::EndsOf(const NoteRecord& n, double on_seconds, double off_seconds)
{
    VoiceEnds ends;
    for (const KeyVoice& kv : n.voices)
    {
        const MacroVoice& v = instruments_.Voice(kv);
        ends.approximated.insert(v.approximations.begin(), v.approximations.end());
        if (!effect_ && !v.started.empty())
        {
            ends.approximated.insert(kCommandsLeftOut);
        }
        if (!effect_ && (v.volume_controller || v.reverb_controller || v.chorus_controller))
        {
            ends.approximated.insert("volumes and sends that a macro sets left out");
        }
        if (!effect_ && v.reverb_source)
        {
            ends.approximated.insert(kReverbSourcesLeftOut);
        }
        if (!effect_ && v.pan_last && v.pan_moves)
        {
            ends.approximated.insert("pans that move keep their first pan");
        }
        if (!v.sample)
        {
            continue;
        }

        const double end = VoiceEnd(v, kv, on_seconds, off_seconds, ends.approximated);
        ends.latest = std::max(ends.latest.value_or(end), end);
        ends.earliest = std::min(ends.earliest.value_or(end), end);

        if (FadesThroughRelease(v))
        {
            ends.faded = std::max(ends.faded, FadeEnd(v, kv.key, on_seconds));
        }
        else if (v.key_off == KeyOff::kFade && end > off_seconds)
        {
            // The fade runs from the release of the note, not from the note-off in the file.
            ends.faded = std::max(ends.faded, off_seconds + TailOf(v));
        }
        else
        {
            ends.tail = std::max(ends.tail, TailOf(v));

            // A voice that ends before the note-off has decayed to silence or stopped. Its release doesn't sound.
            if (v.key_off == KeyOff::kRelease && v.envelope && !v.envelope->dls && end >= off_seconds)
            {
                ends.linear_ms = std::max(ends.linear_ms, v.envelope->release_ms);
            }
        }
        if (v.key_off == KeyOff::kFade && v.release_fade.releases && v.envelope && !v.envelope->dls &&
            v.envelope->release_ms > 0.0 && end >= off_seconds)
        {
            ends.fades_to_release = true;
        }
    }

    return ends;
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
        VoiceEnds ends = EndsOf(n, on_seconds, off_seconds);
        const double end_seconds = ends.latest.value_or(off_seconds);
        const double silent = std::max(end_seconds + ends.tail, ends.faded);
        std::optional<uint8_t> part;
        const bool sweeps = std::any_of(n.voices.begin(), n.voices.end(),
                                        [this](const KeyVoice& kv) { return !instruments_.Voice(kv).sweeps.empty(); });
        if (!effect_ && sweeps)
        {
            part = BendPart(n, on_seconds, silent);
            if (!part)
            {
                ends.approximated.insert(kCommandsLeftOut);
            }
        }

        for (const std::string& a : ends.approximated)
        {
            perf.approximations[a]++;
        }
        if (ends.latest && ends.earliest && *ends.latest - *ends.earliest > 0.001)
        {
            perf.approximations["layered voices that end at different times end together"]++;
        }

        const int64_t end = std::max(n.on, end_seconds == off_seconds ? off : TickAt(end_seconds));
        TrackEvent e{.kind = EventKind::kNoteOff, .key = n.key};
        e.note = n.id;
        e.part = part.value_or(0);
        Record(n.channel, end, e, end == n.on ? std::optional(Rank::kEmptyNoteOff) : std::nullopt);
        sound_end = std::max(sound_end, silent);
        if (!effect_)
        {
            tails_.push_back({.channel = n.channel,
                              .part = e.part,
                              .on = on_seconds,
                              .off = end_seconds,
                              .silent = silent,
                              .linear_ms = ends.linear_ms,
                              .fades_to_release = ends.fades_to_release});
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

    std::sort(events.begin(), events.end(), EarlierEvent);
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
            if (b.channel == channel && b.part == part)
            {
                AddBends(b, events, added);
            }
        }
    }
    events.insert(events.end(), added.begin(), added.end());
}

void Player::AddBends(const BentNote& b, const std::vector<ChannelEvent>& events, std::vector<ChannelEvent>& added)
{
    // The points of the curve and the moves of the wheel during the note, in order of their ticks. A point holds the
    // bend of the curve. A move holds the bend of the wheel.
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
    std::stable_sort(changes.begin(), changes.end(), [](const Change& x, const Change& y) { return x.tick < y.tick; });

    // MusyX adds the sweep to the DSP's pitch after the wheel. The wheel's move therefore scales the DSP's
    // pitch without the sweep.
    double curve = b.curve.front().semitones;
    double moved = b.wheel;
    for (const Change& change : changes)
    {
        (change.wheel ? moved : curve) = change.semitones;
        const double ratio = std::pow(2.0, moved / 12.0) + std::pow(2.0, curve / 12.0) - std::pow(2.0, b.wheel / 12.0);
        TrackEvent bend{.kind = EventKind::kPitch};
        bend.semitones = static_cast<float>(12.0 * std::log2(std::max(ratio, 1.0 / 4096.0)));
        bend.part = b.part;
        added.push_back({change.tick, Rank::kSetting, order_++, bend});
    }

    // At the end, the bend of the part comes back to the bend of the channel.
    TrackEvent back{.kind = EventKind::kPitch};
    back.semitones = wheel;
    back.part = b.part;
    added.push_back({b.end, Rank::kSetting, order_++, back});
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

    // The voice lasts at least until its attack has risen as far as it has to (RiseMs).
    end = std::max(end, on_seconds + RiseMs(v, kv, (off_seconds - on_seconds) * 1000.0) / 1000.0);

    // A voice that releases itself, or that plays on as if held, ends at its stop. No voice sounds after its stop or
    // after its envelope has decayed to silence.
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

    // The DSP plays the voice at its highest pitch at most, as the voice's zones do (AtHighestPitch).
    if (sample != nullptr && sample->pcm.sample_rate > 0 && AtHighestPitch(v, *sample, key))
    {
        ratio = HighestPitchHz() * kNominalRate / (kOutputRate * sample->pcm.sample_rate);
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

double Player::NoteMs(const EffectNote& note, double start, double end)
{
    double tail = 0.0;
    for (const KeyVoice& kv : note.voices)
    {
        tail = std::max(tail, TailOf(instruments_.Voice(kv)));
    }

    return (end - start + tail) * 1000.0;
}

void Player::BendNote(const EffectNote& note, uint8_t channel, double start, double end)
{
    // The pitch wheel bends over the range of the note's first voice.
    const double wheel = Semitones(channel, wheel_[channel]);
    const double ms = NoteMs(note, start, end);

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
    const double ms = NoteMs(note, start, end);
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
        Controller(channel, kPanController, value, p.ms == 0.0 ? 0 : TickAt(start + p.ms / 1000.0));
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
        case kVolumeController:
            controller = value14(effect.volume_controller);
            break;

        case kPanController:
            controller = value14(effect.pan_controller);
            break;

        case kReverbController:
            controller = v.reverb_controller ? *v.reverb_controller * kFullInput : value14(effect.reverb);
            break;

        case kChorusController:
            controller = v.chorus_controller ? *v.chorus_controller * kFullInput : value14(effect.chorus);
            break;

        default:
            if (!v.reverb_source->variable && v.reverb_source->controller != kModWheel)
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
    // A reset of all controllers sets some values back, and a setting after it goes out even if it repeats one before.
    for (std::size_t c = 0; c < kChannels; c++)
    {
        std::vector<ChannelEvent>& events = events_[c];
        AddFadeSteps(events);
        AddBentParts(static_cast<uint8_t>(c), events);
        std::sort(events.begin(), events.end(), EarlierEvent);

        // Each part of the channel keeps its last values. The parts take the same CC7 and CC11.
        std::map<std::pair<uint8_t, EventKind>, int> last;
        std::map<uint8_t, float> pitch;
        ChannelLevels levels;
        for (ChannelEvent& ce : events)
        {
            TrackEvent e = ce.event;
            e.tick = static_cast<uint32_t>(std::max<int64_t>(ce.tick, 0));
            MapLevel(c, e, levels);

            if (e.kind == seqlib::midi::ControlKind(kResetControllers))
            {
                // A reset of all controllers always goes out. It sets the pitch wheel and the controllers that RP-015
                // names back, so a later value equal to the last one before it goes out again.
                pitch[e.part] = 0.0f;
                std::erase_if(last, [&e](const auto& entry)
                              { return entry.first.first == e.part && ResetsToDefault(entry.first.second); });
            }
            else if (e.kind == EventKind::kPitch)
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
    const ntdlib::sequence_export::LoopRules kRules;
    const auto loops = static_cast<std::size_t>(std::max(options.loops, 1));
    std::optional<std::pair<std::size_t, std::size_t>> marked; // the first pass and the count
    for (std::size_t n = 1; !marked && 2 * n <= passes; n++)
    {
        for (std::size_t k = 0; k + 2 * n <= passes && k + n * loops <= passes; k++)
        {
            const int64_t s = starts[k];
            const int64_t e = starts[k + n];
            if (StateRepeats(s, e) && ntdlib::sequence_export::RepeatsAsTheGame(perf, {}, static_cast<uint32_t>(s),
                                                                                static_cast<uint32_t>(e), kRules))
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
        ntdlib::sequence_export::RestoreAtLoopStart(perf, {}, next, kRules);
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
    const double release = last.linear_ms / 1000.0;
    if (release > 0.0 && std::abs(last.off + release - silent) < 1e-9)
    {
        for (double t = last.off + kFaderStepSeconds; t < silent - kFaderStepSeconds / 2.0; t += kFaderStepSeconds)
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
        const auto kCutSteps = static_cast<int>(std::lround(kShortCutSeconds / kFaderStepSeconds));
        for (int i = 1; i < kCutSteps; i++)
        {
            cut.steps.emplace_back(silent - kShortCutSeconds + i * kFaderStepSeconds,
                                   1.0 - static_cast<double>(i) / kCutSteps);
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
    const std::vector<TailCut> cuts = TailCuts();
    for (const TailCut& cut : cuts)
    {
        if (!KeepsCut(perf, cuts, cut))
        {
            continue;
        }

        for (const uint8_t part : cut.parts)
        {
            CutPart(perf.tracks[cut.channel], cut, part);
        }
    }
}

std::vector<TailCut> Player::TailCuts() const
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
        for (std::size_t j = 0; j <= notes.size(); j++)
        {
            if (last != nullptr && (j == notes.size() || (notes[j]->on - last->silent >= kFaderStepSeconds &&
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

    return cuts;
}

bool Player::KeepsCut(const Performance& perf, const std::vector<TailCut>& cuts, const TailCut& cut) const
{
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

    const int64_t from = TickAt(cut.steps.front().first);
    const int64_t at = TickAt(cut.at);
    if (at > perf.end_tick)
    {
        return false;
    }
    if (perf.loop)
    {
        const Performance::Loop loop = *perf.loop;
        if (spans(from, at, loop.start) || spans(from, at, loop.end))
        {
            return false;
        }
        const int64_t length = static_cast<int64_t>(loop.end) - loop.start;
        if (from > loop.start && at < loop.end &&
            std::none_of(cuts.begin(), cuts.end(), [&](const TailCut& later) { return same_cut(cut, later, length); }))
        {
            return false;
        }
    }

    // CC11 mustn't move during the fade.
    const std::vector<TrackEvent>& events = perf.tracks[cut.channel];
    const auto moves_cc11 = [&](const TrackEvent& e)
    {
        return (e.kind == EventKind::kExpression || e.kind == seqlib::midi::ControlKind(kResetControllers)) &&
               e.tick >= from && e.tick <= at;
    };
    return std::none_of(events.begin(), events.end(), moves_cc11);
}

void Player::CutPart(std::vector<TrackEvent>& events, const TailCut& cut, uint8_t part) const
{
    const int64_t from = TickAt(cut.steps.front().first);
    const int64_t at = TickAt(cut.at);

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
        return;
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
    added.push_back(
        {.tick = tick, .kind = EventKind::kExpression, .value = static_cast<uint8_t>(expression), .part = part});

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

} // namespace dolphinsf2::musyx::player
