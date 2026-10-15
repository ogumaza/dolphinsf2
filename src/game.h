// SPDX-License-Identifier: MIT

// A game's JAudio2 sound files: its start-up archive (BAA), its sequence archive and its wave archives, from a folder
// or a GameCube disc image, and the sequences to convert.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"

#include "jaudio.h"
#include "performance.h"

namespace dolphinsf2
{

// What a game's code does with its sounds that its start-up archive doesn't say. An unknown game gets the defaults.
struct GameRules
{
    // The volumes, other than 1, that the game's code gives its effect categories. A category is a group of the sound
    // table. The sound library multiplies an effect's volume by the volume of the effect's category.
    std::map<uint8_t, float> category_volumes;

    // The direct release that the sound library gives an oscillator with neither a release table nor a direct release
    // (OscillatorState::Release). With 0, such an oscillator stops at once when its note is released.
    uint16_t default_release = 0;

    // The echo that the game's code sets up, or nothing. A sequence that sends to it ends when the echo falls silent.
    std::optional<EchoLine> echo;
};

// The rules of the game whose start-up archive has the SHA-1 `archive_sha1`, as 40 lowercase hex digits.
GameRules RulesOf(const std::string& archive_sha1);

class Game
{
public:
    // Open the start-up archive at `baa`. The game keeps its sequence archive in a Seqs folder beside it and its wave
    // archives in a Waves folder; dolphinsf2 also looks beside the archive itself. Throw FormatError if the archive
    // can't be read.
    static Game FromFolder(const std::filesystem::path& baa);

    // Open the start-up archive on a GameCube disc image. Throw FormatError if the image can't be read or has no
    // start-up archive.
    static Game FromDisc(const std::filesystem::path& image);

    const SoundArchive& Archive() const
    {
        return *archive_;
    }

    // The sequences to convert: the sounds of the sound table that play sequences, in its order, then the files of the
    // sequence archive that no sound plays, named after their files.
    const std::vector<SequenceSound>& Sequences() const
    {
        return sequences_;
    }

    // Read a wave archive by name, or return nothing if it can't be found.
    std::optional<std::vector<uint8_t>> ReadWaveArchive(const std::string& name) const;

    // What dolphinsf2 found missing or damaged, such as a missing sequence archive.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // Read the start-up archive in `bytes`, and look up its game's rules. Throw FormatError if the archive can't be
    // read.
    void OpenArchive(std::vector<uint8_t> bytes);

    // List the sequences once the archive and the sequence archive are read.
    void FindSequences();

    // Read the disc's file at `path`. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadDiscFile(const std::string& path) const;

    std::unique_ptr<SoundArchive> archive_;
    GameRules rules_;
    std::vector<uint8_t> sequence_archive_;
    std::string sequence_archive_name_;
    std::vector<ArchiveFile> sequence_files_;
    std::vector<SequenceSound> sequences_;
    std::vector<std::string> warnings_;

    // Where the wave archives are: the folders to look in, or the disc's files, their paths by lowercase name.
    std::vector<std::filesystem::path> wave_folders_;
    std::shared_ptr<ntdlib::disc::GameFiles> disc_;
    std::map<std::string, std::string> disc_waves_;
};

} // namespace dolphinsf2
