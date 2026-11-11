// SPDX-License-Identifier: MIT

// The MusyX games whose code keeps its sounds in layouts of its own: Star Fox Adventures, Paper Mario: The
// Thousand-Year Door, Pokémon Channel, Pokémon Colosseum, Star Wars Rogue Squadron II: Rogue Leader, Star Wars Rogue
// Squadron III: Rebel Strike, Resident Evil, Resident Evil Zero, Tales of Symphonia, Metroid Prime, Metroid Prime 2:
// Echoes, Nintendo Puzzle Collection and the NDDEMO disc. ntdlib::musyx::ReadCatalogue reads a catalogue of each
// game's sounds: the sets of MusyX's four files that hold the groups, the songs, and for each sound the groups that the
// game pushes before it plays the sound. BuildMusic turns the catalogue into the groups that play the songs and the
// effects.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/musyx/catalogue.h"

#include "musyx_game.h"

namespace dolphinsf2::musyx
{

// How the groups that play a catalogue's sounds find the objects and the samples of the other groups that a sound
// pushes.
enum class PushedSets : uint8_t
{
    // A group takes the objects of every set that the sound pushes. It finds missing samples in the sets' holders, one
    // group of the music for each set (SongGroup::pushed).
    kHolders,

    // A group takes the objects and the samples of the other sets that the sound pushes. The game keeps those sets
    // loaded, and their samples play in every group (AddMissingSamples).
    kLoaded,
};

// What the code of a game with a catalogue says about how it plays its sounds: the catalogue's layout and tables,
// MusyX's volume table in main.dol, the master volumes for music and for effects, and the volume that the game gives
// every volume group. Only the effects take the groups' volume. A sound of the catalogue can give another master volume
// or groups' volume. The MusyX build of the game moves the sequencer clock by `tick_step`. Its ScaleVolume divides by
// `volume_scale_divisor` (SongGroup). The songs in `streamed_songs`, each as its group << 16 | its song ID, play a
// stream along with their notes. They're left out. The controllers that a song of the catalogue sets come `controls_ms`
// milliseconds after its start (GameSong::controls_ms).
struct CatalogueRules
{
    std::string_view dol_sha1;
    ntdlib::musyx::CatalogueLayout layout = ntdlib::musyx::CatalogueLayout::kRetro;
    ntdlib::musyx::CatalogueTables tables;
    uint32_t volume_table = 0;
    uint8_t music_master_volume = 127;
    uint8_t effect_master_volume = 127;
    uint8_t group_volume = 127;
    PushedSets pushed_sets = PushedSets::kHolders;
    TickStep tick_step = TickStep::kFloat;
    int volume_scale_divisor = 127;
    std::span<const uint32_t> streamed_songs = {};
    double controls_ms = 0.0;
};

// The games with a catalogue that dolphinsf2 converts.
std::span<const CatalogueRules> CatalogueGames();

// The songs and the effects of a catalogue, the groups that play them and the songs' SNG files. A song's or an
// effect's `group` is an index of `groups`. A song's SNG file has the song's index in `song_data`.
struct CatalogueMusic
{
    std::vector<std::shared_ptr<const SongGroup>> groups;
    std::vector<GameSong> songs;
    std::vector<std::vector<uint8_t>> song_data;
    std::vector<GameEffect> effects;
};

// The music of `catalogue`, played as `rules` says. A song plays from the first of its pushed groups that holds its
// song group. An effect plays from the first of its pushed groups that holds the effect, or with PushedSets::kLoaded
// from the first set that it pushes. That group finds an object or a sample in the groups that the sound pushes, in
// their order. A song that plays only a stream is left out. So is an effect that the game starts at a volume of 0.
// With PushedSets::kHolders, so is a song that MusyX wouldn't start. The songs come first. Throws FormatError if a
// file set is damaged, or if no pushed group plays a sound.
CatalogueMusic BuildMusic(ntdlib::musyx::Catalogue catalogue, const CatalogueRules& rules);

} // namespace dolphinsf2::musyx
