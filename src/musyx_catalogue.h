// SPDX-License-Identifier: MIT

// The MusyX games whose code keeps its sounds in layouts of its own: Star Fox Adventures, Paper Mario: The
// Thousand-Year Door, Pokémon Channel, Star Wars Rogue Squadron II: Rogue Leader, Star Wars Rogue Squadron III: Rebel
// Strike, Resident Evil, Resident Evil Zero, Tales of Symphonia, Metroid Prime and Metroid Prime 2: Echoes. Each game's
// reader gathers a catalogue of the game's sounds: the sets of MusyX's four files that hold the groups, the songs, and
// for each sound the groups that the game pushes before it plays the sound. BuildMusic turns the catalogue into the
// groups that play the songs and the effects.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/disc/game.h"

#include "musyx_game.h"

namespace dolphinsf2::musyx
{

// MusyX's four files for one or more groups: the project, the pool, the sample directory and the samples.
struct FileSet
{
    std::vector<uint8_t> project, pool, sdir, samples;
};

// A group that the game pushes: its MusyX ID and the number of the file set that holds it.
struct PushedGroup
{
    uint16_t id = 0;
    std::size_t set = 0;
};

// A sound of a catalogue: a song or an effect, its name, what MusyX plays, and the groups that the game pushes first,
// in the game's order. A song plays at `volume`, or at MusyX's 127 without one. An effect starts at `volume` and `pan`,
// or at those of its row in its group's table of effects. The game then sets the effect's controllers. The game can
// also set a song's controllers once the song plays (GameSong::controls).
struct CatalogueSound
{
    std::string name;
    bool effect = false;
    uint16_t id = 0;         // MusyX's song ID or effect ID
    uint16_t song_group = 0; // the group that plays a song, which sndSeqPlayEx takes
    std::size_t song = 0;    // a song's SNG file: its index in Catalogue::songs
    std::vector<PushedGroup> groups;
    std::optional<uint8_t> volume;
    std::optional<uint8_t> pan;
    uint8_t volume_controller = 127; // CC7
    uint8_t reverb = 0;              // CC91
    uint16_t pitch_wheel = 0x2000;
    double fade_in_ms = 0.0;                // a song's fade-in (GameSong::fade_in_ms)
    std::vector<SongControl> controls = {}; // GameSong::controls
    double controls_ms = 0.0;               // GameSong::controls_ms
};

// A game's sounds, gathered from its files: the file sets, the songs' SNG files and the sounds.
struct Catalogue
{
    std::vector<FileSet> sets;
    std::vector<std::vector<uint8_t>> songs;
    std::vector<CatalogueSound> sounds;
};

// What the code of a game with a catalogue says about how it plays its sounds: MusyX's volume table in main.dol, the
// master volumes for music and for effects, and the volume that the game gives every volume group. Only the effects
// take the groups' volume.
struct CatalogueRules
{
    std::string_view dol_sha1;
    uint32_t volume_table = 0;
    uint8_t music_master_volume = 127;
    uint8_t effect_master_volume = 127;
    uint8_t group_volume = 127;
    Catalogue (*gather)(const ntdlib::disc::GameFiles& disc) = nullptr;
};

// The rules of the game whose main.dol has the SHA-1 `sha1`, or null for a game without a catalogue.
const CatalogueRules* FindCatalogueGame(std::string_view sha1);

// The songs and the effects of a catalogue, the groups that play them and the songs' SNG files. A song's or an
// effect's `group` is an index of `groups`. A song's SNG file has the song's index in `song_data`.
struct CatalogueMusic
{
    std::vector<std::shared_ptr<const SongGroup>> groups;
    std::vector<GameSong> songs;
    std::vector<std::vector<uint8_t>> song_data;
    std::vector<GameEffect> effects;
};

// The music of `catalogue`, played as `rules` says. A sound plays from the first of its pushed groups that holds its
// song group or its effect. That group finds an object or a sample in the groups that the sound pushes, in their
// order. A song that MusyX wouldn't start, or that plays only a stream, is left out. Throw FormatError if a file set is
// damaged, or if no pushed group plays a sound.
CatalogueMusic BuildMusic(Catalogue catalogue, const CatalogueRules& rules);

} // namespace dolphinsf2::musyx
