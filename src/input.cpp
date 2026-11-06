// SPDX-License-Identifier: MIT

// The games that dolphinsf2 converts (see input.h).

#include "input.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// The games of the first JAudio whose sound effects dolphinsf2 converts, by the SHA-1s of their start-up archives
// (GameOptions::jaudio1_effects). The European releases of The Wind Waker, The Wind Waker HD and Pac-Man Vs. and the
// Japanese release of Pac-Man Vs. have the USA releases' archives. Each of these games plays its effects in its
// effects sequence: the first sequence of its sound table, or the one that ntdlib's rules for the game name.
// dolphinsf2 leaves out the effects of the other games.
constexpr std::array<std::string_view, 10> kEffectsGames{
    "65ea70655099a196e0707bf4e4ce61e7818553d9", // The Wind Waker
    "0a0f93a5bb97313d895e0a5a16604ad77f567547", // The Wind Waker, Japan
    "c66c0103a5c7c1292b5fc6c3371ed1458035cde7", // The Wind Waker HD
    "aa3077a9dafc178e46b80b4e5d975550cb28c517", // The Wind Waker HD, Japan
    "4a60396b9c35333f3083848be1f49c82bf9ba4df", // Luigi's Mansion
    "7436b12c7470a2ed14f865e0a6aa96ccfa1af758", // Pac-Man Vs.
    "a8b63d2f333c34aaf9cb859f37f23011149af917", // Pikmin 2
    "6747c3c7bce964bc62f817e1c8c32711c905c4c3", // Pikmin 2 on the Wii
    "00c63687e6963ad89e1d86f02070e1f552623632", // Super Mario Sunshine
    "9ae6b40b8e9eacfda6ed0b5d59ff1e680373a018", // Pikmin's bank file
};

// Whether `game` is one of kEffectsGames.
bool ListsEffects(const Game& game)
{
    return std::find(kEffectsGames.begin(), kEffectsGames.end(), game.ArchiveSha1()) != kEffectsGames.end();
}

// The JAudio game among the files of `disc`, or nothing if the disc has no start-up archive. Throw FormatError if the
// archive can't be read.
std::optional<Game> JaudioGameOn(const std::shared_ptr<ntdlib::disc::GameFiles>& disc)
{
    std::optional<Game> game = Game::FromDiscFiles(disc, kProgram);
    if (!game)
    {
        return std::nullopt;
    }

    // FromDiscFiles has read the archive already. A game of kEffectsGames opens a second time, with its effects.
    return ListsEffects(*game) ? Game::FromDiscFiles(disc, kProgram, {.jaudio1_effects = true}) : std::move(game);
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

    // A start-up archive: JAudio2's BAA, or JAudio1's AAF, compressed with Yaz0 or not. A game of kEffectsGames opens
    // a second time, with its effects.
    if (IsStartUpArchiveFile(path))
    {
        Game game = Game::FromFolder(path, kProgram);
        Input input;
        input.jaudio_ = std::make_unique<GameInput>(
            ListsEffects(game) ? Game::FromFolder(path, kProgram, {.jaudio1_effects = true}) : std::move(game));

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
    // Pikmin's sequence index and the MusyX games' tables are in main.dol.
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
