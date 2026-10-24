# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# The version is dolphinsf2 --version's, and the one in the file properties of the Windows program and the Info.plist of
# the macOS app. A release changes it here and adds its notes in docs/release-notes/.
# C is for the compression libraries below. A build that doesn't find them installed compiles them from their C
# sources.
project(dolphinsf2 VERSION 1.5.0 LANGUAGES C CXX)

# The libraries that dolphinsf2 shares with the other converters and the PSF players: seqlib for MIDI, SoundFont 2
# and SFZ, xsflib for the common code and the build's templates, and ntdlib for Nintendo's formats. All three are
# submodules. xsflib's XsflibProject.cmake sets the C++ standard, a release build by default, the static C runtime and
# the compiler options that keep the output the same on every platform. Among them, -ffp-contract=off turns off fused
# multiply-adds. JAudio2's arithmetic is single precision without them. The build then keeps the game's rounding.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
  message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(WARNINGS_ON_EVERY_TARGET)
set(NTDLIB_SEQUENCE_EXPORT ON) # the converters' layer, with their command line
# ntdlib's disc_containers reads RVZ, WIA, GCZ and CISO images. xsflib provides the libraries that disc_containers
# requires: zlib (GCZ), Zstandard (RVZ), liblzma (LZMA and LZMA2 in WIA and RVZ) and bzip2 (WIA and RVZ). xsflib takes
# the installed copies where it finds them. Otherwise xsflib downloads each library when configuring. The build then
# compiles it with dolphinsf2.
set(XSFLIB_DISC_LIBRARIES ON)
set(NTDLIB_DISC ON)
xsflib_add_libraries(seqlib xsflib ntdlib)

add_library(dolphinsf2_core STATIC
  src/disc.cpp
  src/input.cpp
  src/musyx_game.cpp
  src/musyx_msm.cpp
  src/musyx_puzzle.cpp
  src/musyx_voices.cpp
  src/musyx_player.cpp
  src/musyx_convert.cpp
  src/rel_module.cpp
  src/yay0.cpp)
target_include_directories(dolphinsf2_core PUBLIC src)
target_link_libraries(dolphinsf2_core PUBLIC seqlib::seqlib xsflib::common ntdlib::common ntdlib::disc
                      ntdlib::disc_containers ntdlib::jaudio ntdlib::musyx ntdlib::wave ntdlib::sequence_export
                      ntdlib::sequence_export_jaudio)

add_executable(dolphinsf2 src/main.cpp)
target_link_libraries(dolphinsf2 PRIVATE dolphinsf2_core)
xsflib_add_version(dolphinsf2 VERSION ${PROJECT_VERSION} DEFINE DOLPHINSF2_VERSION
                   DESCRIPTION "Nintendo GameCube music converter" PRODUCT dolphinsf2
                   COPYRIGHT "Copyright (c) 2026 the dolphinsf2 authors. MIT licence.")

# Finder cannot drop files onto a command-line executable. Package it in a macOS droplet app, with the version in its
# Info.plist.
set(app_description "Drop a GameCube game's start-up archive (.baa, .aaf), disc image (.iso, .gcm, .rvz, .wia, \
.gcz, .ciso) or the sys/main.dol of its extracted disc here to convert its music to MIDI, SoundFont and SFZ. Output \
goes in a folder beside each file.")
xsflib_add_droplet_tool(dolphinsf2 APP_NAME Dolphinsf2 VERSION ${PROJECT_VERSION} DESCRIPTION "${app_description}"
                         PROMPT "Choose GameCube start-up archives (.baa, .aaf), disc images (.iso, .gcm, .rvz, \
.wia, .gcz, .ciso) or the main.dol files of extracted discs to convert:")

include(CTest)
if(BUILD_TESTING)
  add_executable(dolphinsf2_test tests/dolphinsf2_test.cpp)
  target_link_libraries(dolphinsf2_test PRIVATE dolphinsf2_core ntdlib::testing)
  add_test(NAME dolphinsf2_units COMMAND dolphinsf2_test)

  # The program reports the version it was built as.
  string(REPLACE "." "\\." version_pattern "${PROJECT_VERSION}")
  add_test(NAME dolphinsf2_version COMMAND dolphinsf2 --version)
  set_tests_properties(dolphinsf2_version PROPERTIES PASS_REGULAR_EXPRESSION "^dolphinsf2 ${version_pattern}[\r\n]*$")

  # It rejects a port that the game doesn't have.
  add_test(NAME dolphinsf2_port_range COMMAND dolphinsf2 --port 16=1 missing.baa)
  set_tests_properties(dolphinsf2_port_range PROPERTIES PASS_REGULAR_EXPRESSION "--port 16=1 is out of range")
  # Optionally convert and validate a game's sequences:
  # -DDOLPHINSF2_TEST_ARCHIVE=path/to/archive.baa
  set(DOLPHINSF2_TEST_ARCHIVE "" CACHE FILEPATH "Start-up archive (.baa) for the conversion test")
  if(DOLPHINSF2_TEST_ARCHIVE AND EXISTS "${DOLPHINSF2_TEST_ARCHIVE}")
    add_test(NAME dolphinsf2_archive COMMAND dolphinsf2_test "${DOLPHINSF2_TEST_ARCHIVE}")
  endif()

  # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
  xsflib_include_local_tests()
endif()
