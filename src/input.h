// SPDX-License-Identifier: MIT

// The games that dolphinsf2 converts, as it opens them: a JAudio game, from its start-up archive, its disc image or its
// extracted disc, or one of the MusyX games that dolphinsf2 converts, from its disc image or its extracted disc.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/sequence_export/frame.h"

#include "jaudio.h"
#include "musyx_convert.h"

namespace dolphinsf2
{

// A game being converted: a JAudio game (ntdlib's GameInput), or one of the MusyX games that dolphinsf2 converts.
class Input
{
public:
    // Open the game at `path`: a disc image, a start-up archive, or the main.dol of an extracted disc. Throw
    // FormatError, with what to do where it can say, if it's none of them.
    static Input Open(const std::filesystem::path& path);

    // Print the warnings of a JAudio game's files, each after `name`. A MusyX game prints none.
    void PrintWarnings(const std::string& name) const;

    // The game's sequences in order. For a MusyX game, these are the songs and then the effects.
    std::vector<ntdlib::sequence_export::Sequence> Sequences() const;

    // Convert sequence `index` of Sequences, with the errors of the banks or the instruments since the last conversion
    // as warnings.
    Conversion Convert(std::size_t index, const PerformOptions& perform);

private:
    Input() = default;

    // Open the game on the disc image or the extracted disc at `path`: a JAudio game, or one of the MusyX games that
    // dolphinsf2 converts. Only a MusyX game requires the disc's main.dol. Throw FormatError if it's neither.
    static Input OpenDisc(const std::filesystem::path& path);

    // One of the two is set.
    std::unique_ptr<GameInput> jaudio_;
    std::unique_ptr<musyx::GameInput> musyx_;
};

} // namespace dolphinsf2
