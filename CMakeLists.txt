# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# The version is dolphinsf2 --version's, and the one in the file properties of the Windows program and the Info.plist of
# the macOS app. A release changes it here and adds its notes in docs/release-notes/.
project(dolphinsf2 VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/W4 /utf-8)
  # Link the runtime statically so the drag-and-drop executable runs without runtime DLLs.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  # Disable FMA contraction. Clang enables it by default on CPUs such as Apple Silicon. JAudio2's arithmetic is single
  # precision without fused multiply-adds. Without contraction, the build keeps the game's rounding and gives the same
  # results on every platform.
  add_compile_options(-Wall -Wextra -ffp-contract=off)
  # Use SSE2 on 32-bit x86 to avoid x87's extra precision. Results then match 64-bit x86.
  if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86|x86_64|AMD64)$")
    add_compile_options(-msse2 -mfpmath=sse)
  endif()
endif()

add_library(dolphinsf2_core STATIC
  src/jaudio.cpp
  src/wave.cpp
  src/instruments.cpp
  src/envelope.cpp
  src/performance.cpp
  src/midi.cpp
  src/sf2.cpp
  src/sfz.cpp
  src/convert.cpp
  src/disc.cpp
  src/game.cpp)
target_include_directories(dolphinsf2_core PUBLIC src)

add_executable(dolphinsf2 src/main.cpp)
target_link_libraries(dolphinsf2 PRIVATE dolphinsf2_core)
target_compile_definitions(dolphinsf2 PRIVATE DOLPHINSF2_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_link_libraries(dolphinsf2 PRIVATE shell32) # CommandLineToArgvW
  # A version resource. Explorer shows its version and description in the file's properties.
  configure_file(platform/windows/version.rc.in version.rc @ONLY)
  target_sources(dolphinsf2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
endif()

# Finder cannot drop files onto a command-line executable. Package it in a macOS droplet app, with the version in its
# Info.plist.
if(APPLE)
  find_program(OSACOMPILE osacompile)
  find_program(CODESIGN codesign)
  find_program(PLUTIL plutil)
  if(OSACOMPILE AND CODESIGN AND PLUTIL)
    set(app "${CMAKE_CURRENT_BINARY_DIR}/Dolphinsf2.app")
    set(app_script "${PROJECT_SOURCE_DIR}/platform/macos/Dolphinsf2.applescript")
    add_custom_command(
      OUTPUT "${app}/Contents/Resources/dolphinsf2"
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${app}"
      COMMAND "${OSACOMPILE}" -o "${app}" "${app_script}"
      COMMAND "${PLUTIL}" -replace CFBundleShortVersionString -string "${PROJECT_VERSION}" "${app}/Contents/Info.plist"
      COMMAND "${PLUTIL}" -replace CFBundleVersion -string "${PROJECT_VERSION}" "${app}/Contents/Info.plist"
      COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:dolphinsf2>" "${app}/Contents/Resources/dolphinsf2"
      COMMAND "${CODESIGN}" --force --deep --sign - "${app}"
      DEPENDS dolphinsf2 "${app_script}"
      COMMENT "Building Dolphinsf2.app"
      VERBATIM)
    add_custom_target(dolphinsf2_app ALL DEPENDS "${app}/Contents/Resources/dolphinsf2")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(dolphinsf2_test tests/dolphinsf2_test.cpp)
  target_link_libraries(dolphinsf2_test PRIVATE dolphinsf2_core)
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
  file(GLOB local_tests CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/*/local_tests.cmake")
  foreach(file IN LISTS local_tests)
    include("${file}")
  endforeach()
endif()
