// SPDX-License-Identifier: MIT

// A game's sound files (see game.h).

#include "game.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "binary.h"

namespace dolphinsf2
{

namespace
{

namespace fs = std::filesystem;

std::string Lower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return s;
}

std::string Utf8(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::optional<std::vector<uint8_t>> ReadFile(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return std::nullopt;
    }

    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The subfolder of `folder` named `name` in any capitals, or nothing.
std::optional<fs::path> Subfolder(const fs::path& folder, const std::string& name)
{
    std::error_code error;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
    {
        if (e.is_directory(error) && Lower(Utf8(e.path().filename())) == Lower(name))
        {
            return e.path();
        }
    }

    return std::nullopt;
}

} // namespace

Game Game::FromFolder(const fs::path& baa)
{
    std::optional<std::vector<uint8_t>> bytes = ReadFile(baa);
    if (!bytes)
    {
        throw FormatError("can't read it");
    }

    Game game;
    game.archive_ = std::make_unique<SoundArchive>(SoundArchive::Parse(std::move(*bytes)));

    // The sequence archive: the first .arc in the Seqs folder.
    const fs::path folder = baa.parent_path().empty() ? fs::path(".") : baa.parent_path();
    std::vector<fs::path> archives;
    std::error_code error;
    for (const fs::path& where : {Subfolder(folder, "Seqs").value_or(folder), folder})
    {
        for (const fs::directory_entry& e : fs::directory_iterator(where, error))
        {
            if (e.is_regular_file(error) && Lower(Utf8(e.path().extension())) == ".arc")
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
        game.sequence_archive_ = ReadFile(archives.front()).value_or(std::vector<uint8_t>{});
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
    game.disc_ = DiscImage::Open(image);

    // The start-up archive: a .baa file, with its Seqs and Waves folders beside it.
    const DiscImage::File* baa = nullptr;
    for (const DiscImage::File& f : game.disc_->Files())
    {
        if (Lower(f.path).ends_with(".baa"))
        {
            if (baa != nullptr)
            {
                game.warnings_.push_back("several start-up archives on the disc: dolphinsf2 reads " + baa->path);
                break;
            }
            baa = &f;
        }
    }
    if (baa == nullptr)
    {
        throw FormatError("no JAudio2 start-up archive (.baa) on the disc");
    }

    game.archive_ = std::make_unique<SoundArchive>(SoundArchive::Parse(game.disc_->Read(*baa)));
    const std::string folder = Lower(baa->path.substr(0, baa->path.rfind('/') + 1));
    for (const DiscImage::File& f : game.disc_->Files())
    {
        const std::string path = Lower(f.path);
        if (!path.starts_with(folder))
        {
            continue;
        }

        const std::string below = path.substr(folder.size());
        if (below.starts_with("seqs/") && below.ends_with(".arc") && game.sequence_archive_.empty())
        {
            game.sequence_archive_ = game.disc_->Read(f);
            game.sequence_archive_name_ = f.path.substr(f.path.rfind('/') + 1);
        }
        else if (below.starts_with("waves/") && below.ends_with(".aw"))
        {
            game.disc_waves_.emplace(below.substr(6), f);
        }
    }
    game.FindSequences();

    return game;
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
        const auto f = disc_waves_.find(Lower(name));
        if (f == disc_waves_.end())
        {
            return std::nullopt;
        }

        return disc_->Read(f->second);
    }

    std::error_code error;
    for (const fs::path& folder : wave_folders_)
    {
        for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
        {
            if (e.is_regular_file(error) && Lower(Utf8(e.path().filename())) == Lower(name))
            {
                return ReadFile(e.path());
            }
        }
    }

    return std::nullopt;
}

} // namespace dolphinsf2
