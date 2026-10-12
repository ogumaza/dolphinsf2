// SPDX-License-Identifier: MIT

// A game's sound files (see game.h).

#include "game.h"

#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"

namespace dolphinsf2
{
namespace
{

namespace fs = std::filesystem;

using xsflib::AsciiLower;
using xsflib::Utf8;

// The settings of ntdlib's disc reader for dolphinsf2: its messages, and only the plain images that it reads (ISO and
// NKit), with no system files but the header and the file table.
ntdlib::disc::GameOptions DiscOptions()
{
    const std::string kConvert = "Convert it to an ISO image with Dolphin: right-click the game, then Convert File";
    ntdlib::disc::GameOptions options{ntdlib::disc::Console::kGameCube, "dolphinsf2"};
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
    options.system_files = false;

    return options;
}

// The subfolder of `folder` named `name` in any capitals, or nothing.
std::optional<fs::path> Subfolder(const fs::path& folder, const std::string& name)
{
    std::error_code error;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
    {
        if (e.is_directory(error) && AsciiLower(Utf8(e.path().filename())) == AsciiLower(name))
        {
            return e.path();
        }
    }

    return std::nullopt;
}

} // namespace

Game Game::FromFolder(const fs::path& baa)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(baa, bytes))
    {
        throw FormatError("can't read it");
    }

    Game game;
    game.archive_ = std::make_unique<SoundArchive>(SoundArchive::Parse(std::move(bytes)));

    // The sequence archive: the first .arc in the Seqs folder. AppleDouble files don't count. macOS writes one, named
    // "._" and the file's name, beside a file that it copies to a FAT or exFAT drive.
    const fs::path folder = baa.parent_path().empty() ? fs::path(".") : baa.parent_path();
    std::vector<fs::path> archives;
    std::error_code error;
    for (const fs::path& where : {Subfolder(folder, "Seqs").value_or(folder), folder})
    {
        for (const fs::directory_entry& e : fs::directory_iterator(where, error))
        {
            if (e.is_regular_file(error) && AsciiLower(Utf8(e.path().extension())) == ".arc" &&
                !Utf8(e.path().filename()).starts_with("._"))
            {
                archives.push_back(e.path());
            }
        }
        if (!archives.empty())
        {
            break;
        }
    }
    std::sort(archives.begin(), archives.end());
    if (!archives.empty())
    {
        game.sequence_archive_name_ = Utf8(archives.front().filename());
        // A sequence archive that can't be read stays empty.
        xsflib::ReadFile(archives.front(), game.sequence_archive_);
        if (archives.size() > 1)
        {
            game.warnings_.push_back("several sequence archives: dolphinsf2 reads " + game.sequence_archive_name_);
        }
    }

    if (const std::optional<fs::path> waves = Subfolder(folder, "Waves"))
    {
        game.wave_folders_.push_back(*waves);
    }
    game.wave_folders_.push_back(folder);
    game.FindSequences();

    return game;
}

Game Game::FromDisc(const fs::path& image)
{
    Game game;
    game.disc_ = std::make_shared<ntdlib::disc::GameFiles>();
    if (const std::optional<std::string> error = ntdlib::disc::LoadGame(image, *game.disc_, DiscOptions()))
    {
        throw FormatError(*error);
    }

    // The disc's files in the order of its file table.
    std::vector<const ntdlib::disc::GameFile*> files;
    for (const ntdlib::disc::GameFile& f : game.disc_->files)
    {
        files.push_back(&f);
    }
    std::sort(files.begin(), files.end(), [](const auto* a, const auto* b) { return a->index < b->index; });

    // The start-up archive: a .baa file, with its Seqs and Waves folders beside it.
    const ntdlib::disc::GameFile* baa = nullptr;
    for (const ntdlib::disc::GameFile* f : files)
    {
        if (AsciiLower(f->path).ends_with(".baa"))
        {
            if (baa != nullptr)
            {
                game.warnings_.push_back("several start-up archives on the disc: dolphinsf2 reads " + baa->path);
                break;
            }
            baa = f;
        }
    }
    if (baa == nullptr)
    {
        throw FormatError("no JAudio2 start-up archive (.baa) on the disc");
    }

    game.archive_ = std::make_unique<SoundArchive>(SoundArchive::Parse(game.ReadDiscFile(baa->path)));
    const std::string folder = AsciiLower(baa->path.substr(0, baa->path.rfind('/') + 1));
    for (const ntdlib::disc::GameFile* f : files)
    {
        const std::string path = AsciiLower(f->path);
        if (!path.starts_with(folder))
        {
            continue;
        }

        const std::string below = path.substr(folder.size());
        if (below.starts_with("seqs/") && below.ends_with(".arc") && game.sequence_archive_.empty())
        {
            game.sequence_archive_ = game.ReadDiscFile(f->path);
            game.sequence_archive_name_ = f->path.substr(f->path.rfind('/') + 1);
        }
        else if (below.starts_with("waves/") && below.ends_with(".aw"))
        {
            game.disc_waves_.emplace(below.substr(6), f->path);
        }
    }

    game.FindSequences();

    return game;
}

std::vector<uint8_t> Game::ReadDiscFile(const std::string& path) const
{
    std::vector<uint8_t> data;
    try
    {
        disc_->read_file(path, data);
    }
    catch (const ntdlib::disc::Error& e)
    {
        throw FormatError(e.what());
    }

    return data;
}

void Game::FindSequences()
{
    if (sequence_archive_.empty())
    {
        warnings_.push_back("no sequence archive found: its music is skipped");
    }
    else
    {
        try
        {
            // Twilight Princess compresses its sequence archive with Yaz0.
            if (IsYaz0(sequence_archive_))
            {
                sequence_archive_ = DecodeYaz0(sequence_archive_);
            }

            sequence_files_ = ReadRarc(sequence_archive_);
        }
        catch (const FormatError& e)
        {
            warnings_.push_back(sequence_archive_name_ + ": " + e.what() + ": its music is skipped");
        }
    }

    // The sound table's sequences. A sound of the collection starts at its offset in the collection; a sound of the
    // sequence archive at the start of its file.
    std::vector<bool> played(sequence_files_.size());
    for (const SoundEntry& s : archive_->Sounds())
    {
        // A stream plays an audio file from the disc, not a sequence.
        if (LayoutOf(s.type).stream)
        {
            continue;
        }

        SequenceSound seq;
        seq.name = s.name.empty() ? "SOUND_" + std::to_string(s.section) + "_" + std::to_string(s.group) + "_" +
                                        std::to_string(s.index)
                                  : s.name;
        seq.volume = static_cast<float>(s.volume) / 127.0f;
        if (s.source == SoundEntry::Source::kCollection)
        {
            const std::optional<uint32_t> start = archive_->CollectionSequence(s.group, s.resource);
            if (!start)
            {
                continue;
            }
            seq.data = archive_->Collection();
            seq.start = *start;
        }
        else
        {
            const auto file = std::find_if(sequence_files_.begin(), sequence_files_.end(),
                                           [&](const ArchiveFile& f) { return f.id == s.resource; });
            if (file == sequence_files_.end())
            {
                continue;
            }
            played[static_cast<std::size_t>(file - sequence_files_.begin())] = true;
            seq.data = file->data;
            seq.start = 0;
        }
        sequences_.push_back(std::move(seq));
    }

    // The files of the sequence archive that no sound plays.
    for (std::size_t i = 0; i < sequence_files_.size(); i++)
    {
        if (!played[i] && !sequence_files_[i].data.empty())
        {
            const std::string& name = sequence_files_[i].name;
            sequences_.push_back({name.substr(0, name.rfind('.')), sequence_files_[i].data, 0, 1.0f});
        }
    }
}

std::optional<std::vector<uint8_t>> Game::ReadWaveArchive(const std::string& name) const
{
    if (disc_)
    {
        const auto f = disc_waves_.find(AsciiLower(name));
        if (f == disc_waves_.end())
        {
            return std::nullopt;
        }

        return ReadDiscFile(f->second);
    }

    std::error_code error;
    for (const fs::path& folder : wave_folders_)
    {
        for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
        {
            if (e.is_regular_file(error) && AsciiLower(Utf8(e.path().filename())) == AsciiLower(name))
            {
                std::vector<uint8_t> data;
                return xsflib::ReadFile(e.path(), data) ? std::optional(std::move(data)) : std::nullopt;
            }
        }
    }

    return std::nullopt;
}

} // namespace dolphinsf2
