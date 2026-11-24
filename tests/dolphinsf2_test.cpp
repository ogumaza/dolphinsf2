// SPDX-License-Identifier: MIT

// The tests of dolphinsf2. The unit tests run without game data: they build small MusyX song groups, and pieces of the
// Nintendo 64's sound library and of Square's Red driver, in memory. The tests of ntdlib cover the conversion of the
// JAudio sound data, the file of Hudson's sound manager, Yay0 data and modules of code. Given the path of a game, the
// test also converts every sequence of the game and checks each file.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <vector>

#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "xsflib/testing/check.h"

#include "input.h"
#include "jaudio.h"
#include "unit_tests.h"

namespace
{

using namespace dolphinsf2;
using xsflib::testing::failures;

// Open the game at `path` as dolphinsf2 opens its inputs. Convert every sequence, and validate each file.
void TestGameConvertsEverySequence(const char* path)
{
    std::optional<Input> input;
    try
    {
        input = Input::Open(std::filesystem::path(path));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: %s\n", path, e.what());
        failures++;
        return;
    }

    const std::vector<ntdlib::sequence_export::Sequence> sequences = input->Sequences();
    int looping = 0;
    for (const ntdlib::sequence_export::Sequence& sequence : sequences)
    {
        Conversion c;
        try
        {
            c = input->Convert(sequence.index, {});
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", sequence.name.c_str(), e.what());
            failures++;
            continue;
        }

        failures += ntdlib::testing::CheckConversion(c, sequence.name);
        looping += c.performance.loop ? 1 : 0;
    }

    std::printf("%s: %zu sequences converted and checked, %d loop\n", path, sequences.size(), looping);
}

} // namespace

int main(int argc, char** argv)
{
    dolphinsf2::testing::RunMusyxGameTests();
    dolphinsf2::testing::RunMusyxMacroTests();
    dolphinsf2::testing::RunMusyxSongTests();
    dolphinsf2::testing::RunMusyxZoneTests();
    dolphinsf2::testing::RunMusyxStepTests();
    dolphinsf2::testing::RunFadeFitTests();
    dolphinsf2::testing::RunN64Tests();
    dolphinsf2::testing::RunRedTests();

    if (argc > 1)
    {
        TestGameConvertsEverySequence(argv[1]);
    }

    return xsflib::testing::Summary("dolphinsf2");
}
