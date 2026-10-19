// SPDX-License-Identifier: MIT

// GameCube discs (see disc.h).

#include "disc.h"

#include <optional>
#include <string>

namespace dolphinsf2
{
namespace
{

// The messages of ntdlib's disc reader for dolphinsf2, and only the plain images that it reads (ISO and NKit).
ntdlib::disc::GameOptions DiscOptions()
{
    const std::string kConvert = "Convert it to an ISO image with Dolphin: right-click the game, then Convert File";
    ntdlib::disc::GameOptions options{ntdlib::disc::Console::kGameCube, kProgram};
    options.image.refuse_container = [kConvert](const std::string& format)
    {
        return "it's " + std::string(format == "RVZ" ? "an " : "a ") + format +
               " disc image. dolphinsf2 can't read those. " + kConvert;
    };
    options.not_this_console = "not a GameCube disc image";
    options.other_console =
        "it's a Wii disc. dolphinsf2 can't read those. Extract the game's files with Dolphin, and convert its .baa "
        "file";
    options.damaged_file_system = "the disc image's file table is damaged";
    options.short_header = "not a GameCube disc image";
    options.file_past_end = [](const std::string& path)
    {
        return path + " runs past the end of the disc image";
    };

    return options;
}

} // namespace

std::shared_ptr<ntdlib::disc::GameFiles> LoadDisc(const std::filesystem::path& path, bool system_files)
{
    auto disc = std::make_shared<ntdlib::disc::GameFiles>();
    ntdlib::disc::GameOptions options = DiscOptions();
    options.system_files = system_files;
    if (const std::optional<std::string> error = ntdlib::disc::LoadGame(path, *disc, options))
    {
        throw FormatError(*error);
    }

    return disc;
}

} // namespace dolphinsf2
