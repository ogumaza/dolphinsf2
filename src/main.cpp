// SPDX-License-Identifier: MIT

// dolphinsf2: convert the music of GameCube games that use JAudio or MusyX to MIDI, SoundFont and SFZ. It converts the
// files of Wii and Wii U games that use JAudio too.
//
//     dolphinsf2 [options] <start-up archive | disc image | main.dol> [...]
//
// Output goes beside each input. Windows accepts files dropped on the executable; macOS uses Dolphinsf2.app.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/sequence_export/frame.h"

#include "input.h"
#include "jaudio.h"

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
        "Give it a GameCube game's disc image (.iso or .gcm, NKit included, or .rvz,\n"
        ".wia, .gcz or .ciso). For a game extracted with Dolphin, give its sys/main.dol.\n"
        "For a JAudio game, its start-up archive works too: a .baa file, with the Seqs\n"
        "and Waves folders beside it, or a .aaf file, with the Seqs and Banks folders.\n"
        "For a Wii or Wii U game, give its start-up archive. Each sequence gets\n"
        "<name>.mid, <name>.sf2, and per-track SFZ files in sfz/<name>. Shared samples\n"
        "go in sfz/samples. Output is written to <input name>_dolphinsf2 beside the\n"
        "input. You can also drop files onto dolphinsf2.exe on Windows or Dolphinsf2.app\n"
        "on macOS.\n"
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

// Open `input`, named `name`, and print its warnings. Return nothing if it doesn't exist.
std::optional<Input> Open(const fs::path& input, const std::string& name)
{
    std::error_code error;
    if (!fs::exists(input, error))
    {
        return std::nullopt;
    }

    Input opened = Input::Open(input);
    opened.PrintWarnings(name);

    return opened;
}

} // namespace

int main(int argc, char** argv)
{
    frame::Converter<PerformOptions, Input, Conversion> converter;
    converter.program = kProgram;
    converter.version_line = "dolphinsf2 " DOLPHINSF2_VERSION;
    converter.input_word = "input";
    converter.folder_message = " is a folder: give its .baa or .aaf file, or an extracted disc's sys/main.dol";
    converter.samples_folder = kSfzSamplesFolder;
    converter.usage = Usage;
    converter.option = ExtraOption;
    converter.open = Open;
    converter.sequences = std::mem_fn(&Input::Sequences);
    converter.convert = std::mem_fn(&Input::Convert);

    auto run = [&](const std::vector<std::string>& args)
    {
        return frame::Run(args, converter);
    };
    return frame::RunMain(argc, argv, run);
}
