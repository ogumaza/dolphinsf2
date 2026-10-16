# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# The version is dolphinsf2 --version's, and the one in the file properties of the Windows program and the Info.plist of
# the macOS app. A release changes it here and adds its notes in docs/release-notes/.
project(dolphinsf2 VERSION 1.2.0 LANGUAGES CXX)

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
xsflib_add_libraries(seqlib xsflib ntdlib)

add_library(dolphinsf2_core STATIC
  src/disc.cpp
  src/musyx_game.cpp
  src/musyx_voices.cpp
  src/musyx_player.cpp
  src/musyx_convert.cpp)
target_include_directories(dolphinsf2_core PUBLIC src)
target_link_libraries(dolphinsf2_core PUBLIC seqlib::seqlib xsflib::common ntdlib::common ntdlib::disc ntdlib::jaudio
                      ntdlib::musyx ntdlib::wave ntdlib::sequence_export)

add_executable(dolphinsf2 src/main.cpp)
target_link_libraries(dolphinsf2 PRIVATE dolphinsf2_core)
# The version that --version prints, and on Windows a version resource. Explorer shows its version and description in
# the file's properties.
xsflib_add_version(dolphinsf2 VERSION ${PROJECT_VERSION} DEFINE DOLPHINSF2_VERSION
                   DESCRIPTION "Nintendo GameCube music converter" PRODUCT dolphinsf2
                   COPYRIGHT "Copyright (c) 2026 the dolphinsf2 authors. MIT licence.")

# Finder cannot drop files onto a command-line executable. Package it in a macOS droplet app, with the version in its
# Info.plist.
set(app_description "Drop a GameCube game's start-up archive (.baa, .aaf) or disc image (.iso, .gcm) here to convert \
its music to MIDI, SoundFont and SFZ. Output goes in a folder beside each file.")
xsflib_add_droplet_tool(dolphinsf2 APP_NAME Dolphinsf2 VERSION ${PROJECT_VERSION} DESCRIPTION "${app_description}"
                         PROMPT "Choose GameCube start-up archives (.baa, .aaf) or disc images (.iso, .gcm) to convert:")

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
