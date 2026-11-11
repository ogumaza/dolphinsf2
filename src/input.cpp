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
#include "n64_game.h"
#include "red_music.h"

namespace dolphinsf2
{
namespace
{

// The JAudio game among the files of `disc`, or nothing if the disc has no start-up archive. The effects of a game
// whose rules list them (GameRules::known_effects) follow its music. Throw FormatError if the archive can't be read.
std::optional<Game> JaudioGameOn(const std::shared_ptr<ntdlib::disc::GameFiles>& disc)
{
    return Game::FromDiscFiles(disc, kProgram, {.known_effects = true});
}

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

    // An extracted disc keeps its main.dol in its sys folder. A relative path such as sys/main.dol or ./main.dol names
    // the folder above sys only once it is absolute and normal.
    if (xsflib::AsciiLower(xsflib::Utf8(path.filename())) == "main.dol")
    {
        return OpenDisc(std::filesystem::absolute(path).lexically_normal().parent_path().parent_path());
    }

    std::vector<uint8_t> header(0x40);
    f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(f.gcount()));

    // A disc image: a GameCube or Wii disc, or a container that holds one. The disc reader reads a GameCube image,
    // plain or in a container, and says why it refuses the others.
    if (!ntdlib::disc::ContainerFormat(header).empty() || ntdlib::disc::DiscConsole(header))
    {
        return OpenDisc(path);
    }

    // A start-up archive: JAudio2's BAA, or JAudio1's AAF, compressed with Yaz0 or not. The effects of a game whose
    // rules list them follow its music.
    if (IsStartUpArchiveFile(path))
    {
        Input input;
        input.jaudio_ = std::make_unique<GameInput>(Game::FromFolder(path, kProgram, {.known_effects = true}));

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
    else if (n64_)
    {
        const std::vector<n64::Piece>& pieces = n64_->Pieces();
        for (std::size_t i = 0; i < pieces.size(); i++)
        {
            sequences.push_back({i, pieces[i].name});
        }
    }
    else if (red_)
    {
        const std::vector<std::string>& pieces = red_->Pieces();
        for (std::size_t i = 0; i < pieces.size(); i++)
        {
            sequences.push_back({i, pieces[i]});
        }
    }
    else
    {
        // The effects follow the songs (musyx::GameInput::Convert).
        const std::vector<musyx::GameSong>& songs = musyx_->Files().Songs();
        for (std::size_t i = 0; i < songs.size(); i++)
        {
            sequences.push_back({i, songs[i].name});
        }

        const std::vector<musyx::GameEffect>& effects = musyx_->Files().Effects();
        for (std::size_t i = 0; i < effects.size(); i++)
        {
            sequences.push_back({songs.size() + i, effects[i].name});
        }
    }

    return sequences;
}

Conversion Input::Convert(std::size_t index, const PerformOptions& perform)
{
    if (jaudio_)
    {
        return jaudio_->Convert(index, perform, kProgram);
    }

    if (red_)
    {
        return red_->Convert(index, perform);
    }

    return n64_ ? n64_->Convert(index, perform) : musyx_->Convert(index, perform);
}

Input Input::OpenDisc(const std::filesystem::path& path)
{
    // Pikmin's sequence index and the tables of the MusyX games and the N64 library's games are in main.dol.
    Input input;
    const std::shared_ptr<ntdlib::disc::GameFiles> disc = LoadDisc(path, true);
    if (std::optional<Game> game = JaudioGameOn(disc))
    {
        input.jaudio_ = std::make_unique<GameInput>(std::move(*game));
        return input;
    }
    if (std::optional<musyx::Game> game = musyx::Game::Open(disc))
    {
        input.musyx_ = std::make_unique<musyx::GameInput>(std::move(*game));
        return input;
    }
    if ((input.n64_ = n64::GameInput::Open(disc)))
    {
        return input;
    }
    if ((input.red_ = red::GameInput::Open(disc)))
    {
        return input;
    }

    throw FormatError(
        "no JAudio start-up archive (.baa or .aaf) or Red music on the disc, and not one of the MusyX games or the "
        "games with the Nintendo 64's sound library that dolphinsf2 converts");
}

} // namespace dolphinsf2
