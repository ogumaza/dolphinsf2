// SPDX-License-Identifier: MIT

// Yay0, a scheme of Nintendo's that packs files with references back to earlier bytes. Nintendo Puzzle Collection packs
// the modules of its code with it.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace dolphinsf2
{

// Whether `file` starts with Yay0's header: "Yay0".
bool IsYay0(std::span<const uint8_t> file);

// Unpack the Yay0 data in `file`. The header holds four big-endian words: "Yay0", the unpacked size, the offset of the
// table of references and the offset of the table of bytes. Words of flags follow the header. Each flag, from a word's
// top bit down, gives the next step. A set flag takes the next byte from the table of bytes. A clear flag takes the
// next big-endian half from the table of references, and copies earlier output. The half's low 12 bits give the
// distance back, less 1. Its top 4 bits give the length, less 2. A length of 0 there takes the length, less 18, from
// the table of bytes. Throw FormatError if the file is damaged, or if it unpacks to more than 64 MiB.
std::vector<uint8_t> UnpackYay0(std::span<const uint8_t> file);

} // namespace dolphinsf2
