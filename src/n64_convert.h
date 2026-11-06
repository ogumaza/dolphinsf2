// SPDX-License-Identifier: MIT

// The conversion of a piece of music of the Nintendo 64's sound library to MIDI, SoundFont and SFZ.

#pragma once

#include "fade_fit.h"
#include "jaudio.h"
#include "n64_player.h"
#include "n64_sound.h"

namespace dolphinsf2::n64
{

using VolumeShape = FadeShape;

// The length in milliseconds of a usual note, where a piece gives no better figure.
constexpr double kUsualNoteMs = 250.0;

// The shape of `envelope` with decay index `release`, for notes of about `note_ms`. The library fades the envelope
// linearly in amplitude, to the square of each point's value. A release falls linearly in amplitude too, from the level
// that the note holds, in a time in proportion to that level. A SoundFont's release has one rate. The rate fits a note
// of `note_ms`. The decay and the release are fitted to follow those lines (FitFades and FitRelease). An envelope that
// goes back to an earlier point holds where it goes back.
VolumeShape ShapeOf(const std::vector<EnvelopePoint>& envelope, uint8_t release, double note_ms = kUsualNoteMs);

// Play `piece` and build its MIDI, SoundFont and SFZ files from the banks of `data`. Throw FormatError if the piece
// can't be played.
Conversion ConvertPiece(const Piece& piece, SoundData& data, const PerformOptions& options);

} // namespace dolphinsf2::n64
