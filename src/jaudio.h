// SPDX-License-Identifier: MIT

// The conversion of JAudio games, from ntdlib's sequence_export: a game's sound files, the sequence player, the
// instruments and their envelopes, and the MIDI, SoundFont and SFZ files. The names here are the ones that dolphinsf2
// uses.

#pragma once

#include "ntdlib/sequence_export/jaudio/convert.h"
#include "ntdlib/sequence_export/jaudio/envelope.h"
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sfz.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/sequence_export/jaudio/wave.h"

namespace dolphinsf2
{

// The name that dolphinsf2's messages and files give it.
inline constexpr char kProgram[] = "dolphinsf2";

using ntdlib::sequence_export::jaudio::Conversion;
using ntdlib::sequence_export::jaudio::EventKind;
using ntdlib::sequence_export::jaudio::FormatError;
using ntdlib::sequence_export::jaudio::Game;
using ntdlib::sequence_export::jaudio::GameInput;
using ntdlib::sequence_export::jaudio::GameOptions;
using ntdlib::sequence_export::jaudio::IsStartUpArchive;
using ntdlib::sequence_export::jaudio::kSfzSamplesFolder;
using ntdlib::sequence_export::jaudio::MidiReport;
using ntdlib::sequence_export::jaudio::Pcm;
using ntdlib::sequence_export::jaudio::Performance;
using ntdlib::sequence_export::jaudio::PerformOptions;
using ntdlib::sequence_export::jaudio::PresetKey;
using ntdlib::sequence_export::jaudio::SequenceSound;
using ntdlib::sequence_export::jaudio::SfzInstruments;
using ntdlib::sequence_export::jaudio::SfzSamples;
using ntdlib::sequence_export::jaudio::SoundFontSamples;
using ntdlib::sequence_export::jaudio::ToTimecents;
using ntdlib::sequence_export::jaudio::TrackEvent;

} // namespace dolphinsf2
