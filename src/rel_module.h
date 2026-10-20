// SPDX-License-Identifier: MIT

// A module of a GameCube game's code: a REL file that the game links at run time. The file holds the module's sections
// and the relocations that link it. dolphinsf2 reads the relocations that a module makes against itself. They show
// where the module's pointers point, and where the objects of its data start.
//
// The header's big-endian words give the module's ID at 0, the count of sections at 0xC, the offset of the table of
// sections at 0x10, and the offset and the size of the table of imports at 0x28 and 0x2C. A section's entry holds the
// section's offset in the file, with bit 0 set for code, then the section's size. An offset of 0 marks a section
// without data in the file. An import holds a module's ID and the offset of the relocations against that module. A
// relocation takes 8 bytes: the distance from the place of the relocation before it in a big-endian half, the type, the
// section that the relocation points into, and the offset there in a big-endian word. Type 202 starts the relocations
// of the section that it gives. Type 203 ends the list. Type 201 only moves the place on. Type 1 writes the address of
// its target into a word.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dolphinsf2
{

class RelModule
{
public:
    // Read the module in `file`. `name` names the module in the errors. `file` must outlive the object. Throw
    // FormatError if the module is damaged.
    RelModule(std::span<const uint8_t> file, std::string name);

    // The offset in the file of `offset` in section `section`. Throw FormatError if the file holds no such place.
    std::size_t At(uint32_t section, uint32_t offset) const;

    // The offset in the file of the place that the word at file offset `at` points at, or nothing if no relocation sets
    // the word to a place in the file.
    std::optional<std::size_t> Pointer(std::size_t at) const;

    // The object at file offset `at`: its bytes up to the next place in its section that a relocation points at, or up
    // to the section's end. Throw FormatError if no section's data holds `at`.
    std::span<const uint8_t> Object(std::size_t at) const;

    // The pointers of the table at file offset `at`, each with its index in the table. The table runs up to the next
    // object (Object). A word that no relocation sets gives no pointer.
    std::vector<std::pair<uint16_t, std::size_t>> TablePointers(std::size_t at) const;

private:
    // A section: its offset in the file, or 0 for none, its size, and the offsets in it that relocations point at.
    struct Section
    {
        std::size_t offset = 0;
        std::size_t size = 0;
        std::set<uint32_t> targets;
    };

    // Read the list of relocations at file offset `at`. Their targets lie in the module.
    void ReadRelocations(std::size_t at);

    std::span<const uint8_t> file_;
    std::string name_;
    std::vector<Section> sections_;
    std::map<std::size_t, std::size_t> pointers_; // the place that each word points at, by the word's place
};

} // namespace dolphinsf2
