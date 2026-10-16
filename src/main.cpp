// SPDX-License-Identifier: MIT

// dolphinsf2: convert the music of GameCube games that use JAudio or MusyX to MIDI, SoundFont and SFZ. It converts the
// files of Wii and Wii U games that use JAudio too.
//
//     dolphinsf2 [options] <start-up archive | disc image | main.dol> [...]
//
// Output goes beside each input. Windows accepts files dropped on the executable; macOS uses Dolphinsf2.app.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/disc/image.h"
#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/sequence_export/frame.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"

#include "disc.h"
#include "jaudio.h"
#include "musyx_convert.h"
#include "musyx_game.h"
#include "musyx_voices.h"

namespace fs = std::filesystem;
namespace frame = ntdlib::sequence_export;
using namespace dolphinsf2;

namespace
{

// Print usage and options.
void Usage()
{
    std::puts(
        "dolphinsf2: convert the music of GameCube games that use JAudio or MusyX to\n"
        "MIDI, SoundFont 2 and SFZ. It converts the files of Wii and Wii U games that\n"
        "use JAudio too.\n"
        "\n"
        "usage: dolphinsf2 [options] <start-up archive | disc image | main.dol> [...]\n"
        "\n"
        "Give it a GameCube game's disc image (.iso or .gcm, NKit included). For a\n"
        "game extracted with Dolphin, give its sys/main.dol. For a JAudio game, its\n"
        "start-up archive works too: a .baa file, with the Seqs and Waves folders beside\n"
        "it, or a .aaf file, with the Seqs and Banks folders. For a Wii or Wii U game,\n"
        "give its start-up archive. Each sequence gets <name>.mid, <name>.sf2, and\n"
        "per-track SFZ files in sfz/<name>. Shared samples go in sfz/samples. Output is\n"
        "written to <input name>_dolphinsf2 beside the input. You can also drop files\n"
        "onto dolphinsf2.exe on Windows or Dolphinsf2.app on macOS.\n"
        "\n"
        "options:\n"
        "  --output DIR   write to DIR (one input only)\n"
        "  --only REGEX   convert matching sequence names (for example --only BGM)\n"
        "  --loops N      repeat the main loop N times (default 1)\n"
        "                 MIDI loop markers are included for players that support them\n"
        "  --seed N       initial game random state (default 0; JAudio only)\n"
        "  --port N=V     set port N (0-15) of each sequence to V before it starts;\n"
        "                 repeat for more ports (JAudio only)\n"
        "  --voices N     drop the notes that the game drops for lack of a voice,\n"
        "                 with N voices (the game has 64). The result only\n"
        "                 approximates the game. (JAudio only)\n"
        "  --voices off   keep every note (default)\n"
        "  --list         list sequences without converting\n"
        "  --quiet        print errors only\n"
        "  --version      show the version\n");
}

// Take --port N=V into `perform`: port N (0-15) of each sequence. Returns false for any other option.
bool PortOption(const std::string& option, frame::OptionReader& reader, PerformOptions& perform)
{
    if (option != "--port")
    {
        return false;
    }

    const auto [index, number] = frame::ParseAssignment(option, reader.Value(), 15, std::numeric_limits<int16_t>::min(),
                                                        std::numeric_limits<uint16_t>::max());
    perform.ports[index] = static_cast<uint16_t>(number);

    return true;
}

// Take dolphinsf2's options beyond the frame's into `perform`: --port and --voices. Returns false for any other option.
bool ExtraOption(const std::string& option, frame::OptionReader& reader, PerformOptions& perform)
{
    return PortOption(option, reader, perform) || frame::VoicesOption(option, reader, perform);
}

// An input being converted: a JAudio game and the banks that its sequences play, or a MusyX game and the voices of
// its song group. It counts the errors of the banks or the voices that the conversions have reported.
struct Input
{
    explicit Input(Game opened)
        : game(std::move(opened)),
          banks(std::make_unique<BankSet>(game->Archive(),
                                          [this](const std::string& name) { return game->ReadWaveArchive(name); }))
    {
    }

    explicit Input(musyx::Game opened)
        : musyx_game(std::move(opened)), voices(std::make_unique<musyx::Instruments>(musyx_game->Group()))
    {
    }

    const std::vector<std::string>& Errors() const
    {
        return banks ? banks->Errors() : voices->Errors();
    }

    std::optional<Game> game;
    std::unique_ptr<BankSet> banks;
    std::optional<musyx::Game> musyx_game;
    std::unique_ptr<musyx::Instruments> voices;
    std::size_t errors_reported = 0;
};

// Open the game on the disc image or the extracted disc at `path`: a JAudio game, or one of the MusyX games that
// dolphinsf2 converts. Only a MusyX game requires the disc's main.dol. Throw FormatError if it's neither.
std::unique_ptr<Input> OpenDisc(const fs::path& path)
{
    if (std::optional<Game> game = Game::FromDiscFiles(LoadDisc(path, false), kProgram))
    {
        return std::make_unique<Input>(std::move(*game));
    }
    if (std::optional<musyx::Game> game = musyx::Game::Open(LoadDisc(path, true)))
    {
        return std::make_unique<Input>(std::move(*game));
    }

    throw FormatError(
        "no JAudio start-up archive (.baa or .aaf) on the disc, and not one of the MusyX games that dolphinsf2 "
        "converts");
}

// Open `input`: a disc image, a start-up archive, or the main.dol of an extracted disc. Throw FormatError, with what to
// do where it can say, if it's none of them.
std::unique_ptr<Input> OpenGame(const fs::path& input)
{
    std::ifstream f(input, std::ios::binary);
    if (!f)
    {
        throw FormatError("can't read it");
    }

    // An extracted disc keeps its main.dol in its sys folder.
    if (xsflib::AsciiLower(xsflib::Utf8(input.filename())) == "main.dol")
    {
        return OpenDisc(input.parent_path().parent_path());
    }

    std::vector<uint8_t> header(0x40);
    f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(f.gcount()));

    // A disc image: a GameCube or Wii disc, or a container that holds one. The disc reader reads a plain GameCube
    // image, and says why it refuses the others.
    if (!ntdlib::disc::ContainerFormat(header).empty() || ntdlib::disc::DiscConsole(header))
    {
        return OpenDisc(input);
    }
    // A start-up archive: JAudio2's BAA, or JAudio1's AAF.
    if ((header.size() < 4 || std::string(header.begin(), header.begin() + 4) != "AA_<") &&
        !ntdlib::jaudio::IsAaf(header))
    {
        throw FormatError(
            "not a JAudio start-up archive (.baa or .aaf), a GameCube disc image or an extracted disc's main.dol");
    }

    return std::make_unique<Input>(Game::FromFolder(input, kProgram));
}

// Open `input`, named `name`, and print its warnings. Return nothing if it doesn't exist.
std::optional<std::unique_ptr<Input>> Open(const fs::path& input, const std::string& name)
{
    std::error_code error;
    if (!fs::exists(input, error))
    {
        return std::nullopt;
    }

    std::unique_ptr<Input> opened = OpenGame(input);
    if (opened->game)
    {
        for (const std::string& w : opened->game->Archive().Warnings())
        {
            std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), w.c_str());
        }
        for (const std::string& w : opened->game->Warnings())
        {
            std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), w.c_str());
        }
    }

    return opened;
}

// The game's sequences, or its songs, in order.
std::vector<frame::Sequence> Sequences(const std::unique_ptr<Input>& input)
{
    std::vector<frame::Sequence> sequences;
    if (input->game)
    {
        for (std::size_t i = 0; i < input->game->Sequences().size(); i++)
        {
            sequences.push_back({i, input->game->Sequences()[i].name});
        }
    }
    else
    {
        for (std::size_t i = 0; i < input->musyx_game->Songs().size(); i++)
        {
            sequences.push_back({i, input->musyx_game->Songs()[i].name});
        }
    }

    return sequences;
}

// Convert the game's sequence or song `index`, with the errors of the banks or the voices since the last conversion as
// warnings.
Conversion Convert(std::unique_ptr<Input>& input, std::size_t index, const PerformOptions& perform)
{
    Conversion c = input->game ? ConvertSequence(input->game->Sequences()[index], *input->banks, perform, kProgram)
                               : musyx::ConvertSong(*input->musyx_game, index, *input->voices, perform);
    for (; input->errors_reported < input->Errors().size(); input->errors_reported++)
    {
        c.warnings.push_back(input->Errors()[input->errors_reported]);
    }

    return c;
}

} // namespace

int main(int argc, char** argv)
{
    frame::Converter<PerformOptions, std::unique_ptr<Input>, Conversion> converter;
    converter.program = "dolphinsf2";
    converter.version_line = "dolphinsf2 " DOLPHINSF2_VERSION;
    converter.input_word = "input";
    converter.folder_message = " is a folder: give its .baa or .aaf file";
    converter.samples_folder = kSfzSamplesFolder;
    converter.usage = Usage;
    converter.option = ExtraOption;
    converter.open = Open;
    converter.sequences = Sequences;
    converter.convert = Convert;

    auto run = [&](const std::vector<std::string>& args)
    {
        return frame::Run(args, converter);
    };
    return frame::RunMain(argc, argv, run);
}
