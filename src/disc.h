// SPDX-License-Identifier: MIT

// GameCube disc images and extracted discs, read with ntdlib's disc reader and dolphinsf2's messages.

#pragma once

#include <filesystem>
#include <memory>

#include "ntdlib/disc/game.h"

namespace dolphinsf2
{

// Load the files of the GameCube disc image or the extracted disc at `path`, and with `system_files` its main.dol too.
// Throw FormatError if the disc can't be read.
std::shared_ptr<ntdlib::disc::GameFiles> LoadDisc(const std::filesystem::path& path, bool system_files);

} // namespace dolphinsf2
