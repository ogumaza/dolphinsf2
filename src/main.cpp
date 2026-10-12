// SPDX-License-Identifier: MIT

// dolphinsf2: convert the sequences of GameCube games that use JAudio2 to MIDI, SoundFont and SFZ.
//
//     dolphinsf2 [options] <archive.baa | disc image> [...]
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
#include "ntdlib/sequence_export/frame.h"
#include "xsflib/binary.h"

#include "convert.h"
#include "game.h"
#include "instruments.h"
#include "performance.h"
#include "sfz.h"

namespace fs = std::filesystem;
namespace frame = ntdlib::sequence_export;
using namespace dolphinsf2;

namespace
{

// Print usage and options.
void Usage()
{
    std::puts(
        "dolphinsf2: convert the music of GameCube games that use JAudio2 to MIDI,\n"
        "SoundFont 2 and SFZ.\n"
        "\n"
        "usage: dolphinsf2 [options] <archive.baa | disc image> [...]\n"
        "\n"
        "Give it a game's start-up archive (a .baa file, with the Seqs and Waves\n"
        "folders beside it) or its disc image (.iso or .gcm, NKit included). Each\n"
        "sequence gets <name>.mid, <name>.sf2, and per-track SFZ files in sfz/<name>.\n"
        "Shared samples go in sfz/samples. Output is written to <input name>_dolphinsf2\n"
        "beside the input. You can also drop files onto dolphinsf2.exe on Windows or\n"
        "Dolphinsf2.app on macOS.\n"
        "\n"
        "options:\n"
        "  --output DIR   write to DIR (one input only)\n"
        "  --only REGEX   convert matching sequence names (for example --only BGM)\n"
        "  --loops N      repeat the main loop N times (default 1)\n"
        "                 MIDI loop markers are included for players that support them\n"
        "  --seed N       initial game random state (default 0)\n"
        "  --port N=V     set port N (0-15) of each sequence to V before it starts;\n"
        "                 repeat for more ports\n"
        "  --voices N     drop the notes that the game drops for lack of a voice,\n"
        "                 with N voices (the game has 64). The result only\n"
        "                 approximates the game.\n"
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

// Open `input`: a disc image or a start-up archive. Throw FormatError, with what to do where it can say, if it's
// neither.
Game OpenGame(const fs::path& input)
{
    std::ifstream f(input, std::ios::binary);
    if (!f)
    {
        throw FormatError("can't read it");
    }

    std::vector<uint8_t> header(0x40);
    f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(f.gcount()));

    // A disc image: a GameCube or Wii disc, or a container that holds one. The disc reader reads a plain GameCube
    // image, and says why it refuses the others.
    if (!ntdlib::disc::ContainerFormat(header).empty() || ntdlib::disc::DiscConsole(header))
    {
        return Game::FromDisc(input);
    }
    if (header.size() < 4 || std::string(header.begin(), header.begin() + 4) != "AA_<")
    {
        throw FormatError("not a JAudio2 start-up archive (.baa) or a GameCube disc image");
    }

    return Game::FromFolder(input);
}

// An input being converted: the game, the banks that its sequences play, and how many of the banks' errors the
// conversions have reported.
struct Input
{
    explicit Input(Game opened)
        : game(std::move(opened)),
          banks(game.Archive(), [this](const std::string& name) { return game.ReadWaveArchive(name); })
    {
    }

    Game game;
    BankSet banks;
    std::size_t errors_reported = 0;
};

// Open `input`, named `name`, and print its warnings. Return nothing if it doesn't exist.
std::optional<std::unique_ptr<Input>> Open(const fs::path& input, const std::string& name)
{
    std::error_code error;
    if (!fs::exists(input, error))
    {
        return std::nullopt;
    }

    auto opened = std::make_unique<Input>(OpenGame(input));
    for (const std::string& w : opened->game.Archive().Warnings())
    {
        std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), w.c_str());
    }
    for (const std::string& w : opened->game.Warnings())
    {
        std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), w.c_str());
    }

    return opened;
}

// The game's sequences, in order.
std::vector<frame::Sequence> Sequences(const std::unique_ptr<Input>& input)
{
    std::vector<frame::Sequence> sequences;
    for (std::size_t i = 0; i < input->game.Sequences().size(); i++)
    {
        sequences.push_back({i, input->game.Sequences()[i].name});
    }

    return sequences;
}

// Convert the game's sequence `index`, with the banks' errors since the last conversion as warnings.
Conversion Convert(std::unique_ptr<Input>& input, std::size_t index, const PerformOptions& perform)
{
    Conversion c = ConvertSequence(input->game.Sequences()[index], input->banks, perform);
    for (; input->errors_reported < input->banks.Errors().size(); input->errors_reported++)
    {
        c.warnings.push_back(input->banks.Errors()[input->errors_reported]);
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
    converter.folder_message = " is a folder: give its .baa file";
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
