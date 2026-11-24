// SPDX-License-Identifier: MIT

// The groups of unit tests that dolphinsf2_test.cpp runs. Each group runs its tests and adds its failures to
// xsflib::testing::failures.

#pragma once

namespace dolphinsf2::testing
{

void RunMusyxGameTests();
void RunMusyxMacroTests();
void RunMusyxSongTests();
void RunMusyxZoneTests();
void RunMusyxStepTests();
void RunFadeFitTests();
void RunN64Tests();
void RunRedTests();

} // namespace dolphinsf2::testing
