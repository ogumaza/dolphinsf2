// SPDX-License-Identifier: MIT

// The games with the Nintendo 64's sound library that dolphinsf2 converts (see n64_game.h).

#include "n64_game.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "xsflib/sha1.h"

#include "jaudio.h"
#include "n64_convert.h"
#include "n64_player.h"
#include "n64_sound.h"

namespace dolphinsf2::n64
{
namespace
{

// What a game's code says about its music: where the library's tables lie, and how the game starts its pieces. The
// addresses come from the disc's link map of main.dol.
struct GameRules
{
    std::string_view dol_sha1;
    LibraryTables tables;

    // SEQ_TABLE: the sequence of each of the game's numbers for its pieces, a byte each, or 16 bits with
    // `wide_numbers`.
    uint32_t music_table = 0;
    uint32_t pieces = 256;
    bool wide_numbers = false;

    // The masks of the channels that the game mutes: BGM_MUTE_TABLE_FINE, _SNOW and _SAKURA, a 16-bit mask for each
    // piece of the hour, and BGM_MUTE_TABLE_MUSEUM, a mask for each room. Room 0 is outside the museum.
    std::array<uint32_t, 3> weather_masks{};
    uint32_t museum_masks = 0;

    // The pieces of the music discs. The game plays them from a stereo (kDiscVolume).
    uint32_t first_disc = 0;
    uint32_t last_disc = 0;

    // The numbers that the game's start of a piece plays as effects of the start-up screen, or 0 and 0.
    uint32_t first_effect = 0;
    uint32_t last_effect = 0;

    // A piece whose sequence ends at once, or after a tick without a sound.
    uint32_t empty_piece = 0;

    // Doubutsu no Mori e+'s piece of the island, and the channels that the game mutes unless the islander is there
    // (BGM_MUTE_PRIVATE_ISLAND). The game takes the mask from its small data.
    uint32_t island_piece = 0;
    uint16_t island_mask = 0;
};

constexpr std::array<GameRules, 5> kGames{{
    {
        // Animal Crossing (USA)
        .dol_sha1 = "2ae8f56e7791d37e165bd5900921f2269f9515bf",
        .tables =
            {
                .sequences = 0x800cca40,    // AudioseqHeaderStart
                .banks = 0x800cd9e0,        // AudiobankHeaderStart
                .sample_banks = 0x800ce3e0, // AudiowaveHeaderStart
                .segments = 0x800ce450,
                .bank_map = 0x800ce490,
                .cents = 0x800d24c4,            // PCENTTABLE
                .small_cents = 0x800d28c4,      // PCENTTABLE2
                .pitches = 0x800d2cc4,          // PITCHTABLE
                .short_velocities = 0x800d2ec4, // DEFAULT_VTABLE
                .short_gate_times = 0x800d2ed4, // DEFAULT_GTABLE
                .default_envelope = 0x800d2ee4, // DEFAULT_ENV
            },
        .music_table = 0x800a9838,
        .weather_masks = {0x800ced70, 0x800ceda0, 0x800cedd0},
        .museum_masks = 0x800cee00,
        .first_disc = 128,
        .last_disc = 182,
        .first_effect = 83,
        .last_effect = 87,
        .empty_piece = 126,
    },
    {
        // Doubutsu no Mori+ (Japan), disc version 1. It plays numbers 83 to 87 as pieces.
        .dol_sha1 = "25a5ada7a025c092307e61b4420953f36633500d",
        .tables =
            {
                .sequences = 0x800c5200,
                .banks = 0x800c6140,
                .sample_banks = 0x800c6ae0,
                .segments = 0x800c6b50,
                .bank_map = 0x800c6b90,
                .cents = 0x800cae44,
                .small_cents = 0x800cb244,
                .pitches = 0x800cb644,
                .short_velocities = 0x800cb844,
                .short_gate_times = 0x800cb854,
                .default_envelope = 0x800cb864,
            },
        .music_table = 0x800a63e8,
        .weather_masks = {0x800c7570, 0x800c75a0, 0x800c75d0},
        .museum_masks = 0x800c7600,
        .first_disc = 128,
        .last_disc = 182,
        .empty_piece = 126,
    },
    {
        // Doubutsu no Mori e+ (Japan), disc version 1. A later build of the library, with 16-bit numbers of pieces and
        // sequences. The island's piece plays a copy of piece 91's sequence.
        .dol_sha1 = "7574bec2692efc921c0975c52af41038f3fb7093",
        .tables =
            {
                .sequences = 0x800f22e0,
                .banks = 0x800f3900,
                .sample_banks = 0x800f4410,
                .segments = 0x800f4460,
                .bank_map = 0x800f44a0,
                .cents = 0x800f81e4,
                .small_cents = 0x800f85e4,
                .pitches = 0x800f89e4,
                .short_velocities = 0x800f8be4,
                .short_gate_times = 0x800f8bf4,
                .default_envelope = 0x800f8c04,
            },
        .music_table = 0x800d1940,
        .pieces = 384,
        .wide_numbers = true,
        .weather_masks = {0x800f4f30, 0x800f4f60, 0x800f4f90},
        .museum_masks = 0x800f4fc0,
        .first_disc = 208,
        .last_disc = 367,
        .first_effect = 83,
        .last_effect = 87,
        .empty_piece = 90,
        .island_piece = 376,
        .island_mask = 0x04dc, // at 0x80291760, read by the start of a piece at 0x80010890
    },
    {
        // Doubutsu no Mori+ (Japan), disc version 0. The disc's link map of main.dol (static.map) gives each address.
        // The tables hold disc version 1's values, except the offsets of what follows sequence 240 in audiorom.img.
        // That sequence is 32 bytes shorter here.
        .dol_sha1 = "afb54e84f33c77165b3b7672ab098b88bbdefd09",
        .tables =
            {
                .sequences = 0x800c4ee0,
                .banks = 0x800c5e20,
                .sample_banks = 0x800c67c0,
                .segments = 0x800c6830,
                .bank_map = 0x800c6870,
                .cents = 0x800ca924,
                .small_cents = 0x800cad24,
                .pitches = 0x800cb124,
                .short_velocities = 0x800cb324,
                .short_gate_times = 0x800cb334,
                .default_envelope = 0x800cb344,
            },
        .music_table = 0x800a60c8,
        .weather_masks = {0x800c7250, 0x800c7280, 0x800c72b0},
        .museum_masks = 0x800c72e0,
        .first_disc = 128,
        .last_disc = 182,
        .empty_piece = 126,
    },
    {
        // Doubutsu no Mori e+ (Japan), disc version 0. The disc's link map of main.dol (static.map) gives each address.
        // Every table holds disc version 1's values, and the disc holds disc version 1's audiorom.img.
        .dol_sha1 = "25d65de224306e90b12e89613c5658e4214c63b2",
        .tables =
            {
                .sequences = 0x800f0700,
                .banks = 0x800f1d20,
                .sample_banks = 0x800f2830,
                .segments = 0x800f2880,
                .bank_map = 0x800f28c0,
                .cents = 0x800f6604,
                .small_cents = 0x800f6a04,
                .pitches = 0x800f6e04,
                .short_velocities = 0x800f7004,
                .short_gate_times = 0x800f7014,
                .default_envelope = 0x800f7024,
            },
        .music_table = 0x800d04c0,
        .pieces = 384,
        .wide_numbers = true,
        .weather_masks = {0x800f3350, 0x800f3380, 0x800f33b0},
        .museum_masks = 0x800f33e0,
        .first_disc = 208,
        .last_disc = 367,
        .first_effect = 83,
        .last_effect = 87,
        .empty_piece = 90,
        .island_piece = 376,
        .island_mask = 0x04dc, // BGM_MUTE_PRIVATE_ISLAND, at 0x80290b20
    },
}};

// The pieces that play the hour, and the weathers for which they hold arrangements, in the order of
// GameRules::weather_masks. The game plays another piece in the rain.
constexpr uint32_t kFirstHour = 1;
constexpr uint32_t kLastHour = 24;
constexpr std::array<std::string_view, 3> kWeathers{"fine", "snow", "sakura"};

// The museum's piece, and its rooms from 1 on.
constexpr uint32_t kMuseum = 88;
constexpr std::array<std::string_view, 5> kMuseumRooms{"lobby", "paintings", "fish", "insects", "fossils"};

// The first parts of Kapp'n's songs. The game starts a song's second part as a piece of its own. Its start writes the
// first part's ports 0 and 1: the sequence player that plays the second part, and the second part's sequence. The
// first part's script then starts that sequence there. Port 0's 0xff stands for the first part's player. The first
// part plays alone here: its ports name its player and the empty piece's sequence. The first part's player then stops
// where the second part would start.
constexpr uint32_t kFirstSong = 92;
constexpr uint32_t kLastSong = 111;
constexpr int8_t kThisPlayer = -1;

// The volume of a music disc's piece from the stereo, with the listener at the stereo: the game's highest. The game
// sets the pan of every channel to the centre once a frame.
constexpr float kDiscVolume = 0.8f;

const GameRules* RulesOf(std::string_view sha1)
{
    const auto it =
        std::find_if(kGames.begin(), kGames.end(), [sha1](const GameRules& rules) { return rules.dol_sha1 == sha1; });
    return it != kGames.end() ? &*it : nullptr;
}

// The bytes of `size` at `address` in `dol`. Throw FormatError if `dol` holds fewer there.
std::span<const uint8_t> BytesAt(const ntdlib::Dol& dol, uint32_t address, std::size_t size, const char* what)
{
    const std::span<const uint8_t> bytes = dol.At(address);
    if (bytes.size() < size)
    {
        throw FormatError(std::string("main.dol's ") + what + " ends early");
    }

    return bytes.first(size);
}

uint16_t MaskAt(std::span<const uint8_t> table, std::size_t index)
{
    return static_cast<uint16_t>(table[2 * index] << 8 | table[2 * index + 1]);
}

std::string PieceName(uint32_t number)
{
    std::string digits = std::to_string(number);
    return "bgm" + std::string(3 - std::min<std::size_t>(digits.size(), 3), '0') + digits;
}

// The game's pieces (GameInput::Pieces).
std::vector<Piece> PiecesOf(const ntdlib::Dol& dol, const GameRules& rules, const SoundData& data)
{
    const std::size_t entry_size = rules.wide_numbers ? 2 : 1;
    const std::span<const uint8_t> music = BytesAt(dol, rules.music_table, rules.pieces * entry_size, "table of music");
    const auto sequence_of = [&music, entry_size](uint32_t number)
    {
        return entry_size == 2 ? static_cast<uint32_t>(music[2 * number] << 8 | music[2 * number + 1]) : music[number];
    };
    std::array<std::span<const uint8_t>, 3> weather_masks;
    for (std::size_t w = 0; w < kWeathers.size(); w++)
    {
        weather_masks[w] = BytesAt(dol, rules.weather_masks[w], 2 * (kLastHour - kFirstHour + 1), "table of masks");
    }

    const std::span<const uint8_t> museum_masks =
        BytesAt(dol, rules.museum_masks, 2 * (kMuseumRooms.size() + 1), "table of the museum's masks");

    // A port holds a byte. The empty piece's sequence has a number below 256.
    const uint32_t empty_sequence = sequence_of(rules.empty_piece);

    std::vector<Piece> pieces;
    std::set<uint32_t> sequences;
    for (uint32_t number = 0; number < rules.pieces; number++)
    {
        Piece piece{.name = PieceName(number), .sequence = sequence_of(number)};
        const uint32_t played = data.Resolve(piece.sequence);
        if (number >= kFirstHour && number <= kLastHour)
        {
            for (std::size_t w = 0; w < kWeathers.size(); w++)
            {
                Piece arrangement = piece;
                arrangement.name += "-" + std::string(kWeathers[w]);
                arrangement.muted = MaskAt(weather_masks[w], number - kFirstHour);
                pieces.push_back(std::move(arrangement));
            }
            continue;
        }
        if (number == kMuseum)
        {
            for (std::size_t room = 1; room <= kMuseumRooms.size(); room++)
            {
                Piece arrangement = piece;
                arrangement.name += "-" + std::string(kMuseumRooms[room - 1]);
                arrangement.muted = MaskAt(museum_masks, room);
                pieces.push_back(std::move(arrangement));
            }
            continue;
        }

        if (number != 0 && number == rules.island_piece)
        {
            Piece islander = piece;
            islander.name += "-islander";
            pieces.push_back(std::move(islander));
            piece.name += "-no-islander";
            piece.muted = rules.island_mask;
            pieces.push_back(std::move(piece));
            continue;
        }

        // Number 0 stops the music.
        const bool effect = rules.first_effect != 0 && number >= rules.first_effect && number <= rules.last_effect;
        if (number == 0 || effect || !sequences.insert(played).second)
        {
            continue;
        }
        if (number >= rules.first_disc && number <= rules.last_disc)
        {
            piece.volume = kDiscVolume;
            piece.centre_pans = true;
        }
        if (number >= kFirstSong && number <= kLastSong)
        {
            piece.ports = {{0, kThisPlayer}, {1, static_cast<int8_t>(empty_sequence & 0xff)}};
            piece.end_sequence = static_cast<int>(empty_sequence);
        }
        pieces.push_back(std::move(piece));
    }

    return pieces;
}

} // namespace

GameInput::GameInput(SoundData data, std::vector<Piece> pieces) : data_(std::move(data)), pieces_(std::move(pieces))
{
}

std::unique_ptr<GameInput> GameInput::Open(const std::shared_ptr<ntdlib::disc::GameFiles>& disc)
{
    const GameRules* rules = RulesOf(xsflib::Sha1::Hex(xsflib::Sha1::Of(disc->dol)));
    if (rules == nullptr)
    {
        return nullptr;
    }

    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc->dol);
    std::vector<uint8_t> rom;
    try
    {
        if (!disc->read_file("audiorom.img", rom))
        {
            throw FormatError("can't read audiorom.img");
        }
    }
    catch (const ntdlib::disc::Error& e)
    {
        throw FormatError(e.what());
    }

    SoundData data(dol, rules->tables, std::move(rom));
    std::vector<Piece> pieces = PiecesOf(dol, *rules, data);
    return std::unique_ptr<GameInput>(new GameInput(std::move(data), std::move(pieces)));
}

Conversion GameInput::Convert(std::size_t index, const PerformOptions& perform)
{
    if (index >= pieces_.size())
    {
        throw FormatError("the game has no piece " + std::to_string(index));
    }

    Conversion c = ConvertPiece(pieces_[index], data_, perform);
    const std::vector<std::string>& errors = data_.Errors();
    for (; errors_reported_ < errors.size(); errors_reported_++)
    {
        c.warnings.push_back(errors[errors_reported_]);
    }

    return c;
}

} // namespace dolphinsf2::n64
