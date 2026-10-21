// SPDX-License-Identifier: MIT

// The games that dolphinsf2 converts (see input.h).

#include "input.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/disc/image.h"
#include "ntdlib/sequence_export/frame.h"
#include "xsflib/ascii.h"
#include "xsflib/files.h"

#include "disc.h"
#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"

namespace dolphinsf2
{
namespace
{

// Whether the disc reader takes the file at `path` for a GameCube disc image.
bool ReadsAsDisc(const std::filesystem::path& path)
{
    try
    {
        LoadDisc(path, false);
    }
    catch (const FormatError&)
    {
        return false;
    }

    return true;
}

} // namespace

Input Input::Open(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        throw FormatError("can't read it");
    }

    // An extracted disc keeps its main.dol in its sys folder.
    if (xsflib::AsciiLower(xsflib::Utf8(path.filename())) == "main.dol")
    {
        return OpenDisc(path.parent_path().parent_path());
    }

    std::vector<uint8_t> header(0x40);
    f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(f.gcount()));

    // A disc image: a GameCube or Wii disc, or a container that holds one. The disc reader reads a plain GameCube
    // image, and says why it refuses the others.
    if (!ntdlib::disc::ContainerFormat(header).empty() || ntdlib::disc::DiscConsole(header))
    {
        return OpenDisc(path);
    }

    // A start-up archive: JAudio2's BAA, or JAudio1's AAF.
    if (IsStartUpArchive(header))
    {
        Input input;
        input.jaudio_ = std::make_unique<GameInput>(Game::FromFolder(path, kProgram));

        return input;
    }

    // Some GameCube discs that never went on sale leave the magic word out of their header. The disc reader takes such
    // a disc when the rest of it is intact.
    if (ReadsAsDisc(path))
    {
        return OpenDisc(path);
    }

    throw FormatError(
        "not a JAudio start-up archive (.baa or .aaf), a GameCube disc image or an extracted disc's main.dol");
}

void Input::PrintWarnings(const std::string& name) const
{
    if (jaudio_)
    {
        jaudio_->PrintWarnings(name);
    }
}

std::vector<ntdlib::sequence_export::Sequence> Input::Sequences() const
{
    std::vector<ntdlib::sequence_export::Sequence> sequences;
    if (jaudio_)
    {
        const std::vector<SequenceSound>& sounds = jaudio_->Files().Sequences();
        for (std::size_t i = 0; i < sounds.size(); i++)
        {
            sequences.push_back({i, sounds[i].name});
        }
    }
    else
    {
        const std::vector<musyx::GameSong>& songs = musyx_->Files().Songs();
        for (std::size_t i = 0; i < songs.size(); i++)
        {
            sequences.push_back({i, songs[i].name});
        }
    }

    return sequences;
}

Conversion Input::Convert(std::size_t index, const PerformOptions& perform)
{
    return jaudio_ ? jaudio_->Convert(index, perform, kProgram) : musyx_->Convert(index, perform);
}

Input Input::OpenDisc(const std::filesystem::path& path)
{
    Input input;
    if (std::optional<Game> game = Game::FromDiscFiles(LoadDisc(path, false), kProgram))
    {
        input.jaudio_ = std::make_unique<GameInput>(std::move(*game));
        return input;
    }
    if (std::optional<musyx::Game> game = musyx::Game::Open(LoadDisc(path, true)))
    {
        input.musyx_ = std::make_unique<musyx::GameInput>(std::move(*game));
        return input;
    }

    throw FormatError(
        "no JAudio start-up archive (.baa or .aaf) on the disc, and not one of the MusyX games that dolphinsf2 "
        "converts");
}

} // namespace dolphinsf2
