cmake_minimum_required(VERSION 3.14)

# project() is a no-op when this file is included via add_subdirectory() from
# a parent (videocomposer, audioplayer, dmxplayer). Declaring a version lets
# consumers probe with find_package(mtcreceiver 2.0) once this PR lands.
project(mtcreceiver VERSION 2.0.0 LANGUAGES CXX)

add_library(mtcreceiver mtcreceiver.cpp)

# Include directories
target_include_directories(mtcreceiver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RTMIDI_INCLUDE_DIRS}
)

# Link RtMidi
if(RTMIDI_LIBRARIES)
    target_link_libraries(mtcreceiver ${RTMIDI_LIBRARIES})
endif()

# Build options (OFF by default).
#   MTCRECV_VERBOSE_DIAG — re-enables the MIDI-thread diagnostic writes that
#                         previously went via std::cout. Kept off in production
#                         because cout+endl flushes block the MIDI callback.
#   MTCRECV_TESTING     — exposes test-only helpers (invokeTickForTesting, the
#                         SkipPortOpenTag constructor, decode forwarders, and
#                         state reset helpers). Required by the test suite.
#   MTCRECV_BUILD_TESTS — builds the unit test executable. Only enabled by
#                         default when mtcreceiver is the top-level project,
#                         never when included as a submodule.
option(MTCRECV_VERBOSE_DIAG "Enable MTC receiver verbose diagnostic output" OFF)
option(MTCRECV_TESTING      "Expose test-only helpers (invokeTickForTesting, etc.)" OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(MTCRECV_BUILD_TESTS "Build mtcreceiver unit tests" ON)
else()
    option(MTCRECV_BUILD_TESTS "Build mtcreceiver unit tests" OFF)
endif()

if(MTCRECV_VERBOSE_DIAG)
    target_compile_definitions(mtcreceiver PUBLIC MTCRECV_VERBOSE_DIAG)
endif()
# MTCRECV_BUILD_TESTS implies MTCRECV_TESTING — tests need the test-only API.
if(MTCRECV_BUILD_TESTS)
    set(MTCRECV_TESTING ON CACHE BOOL "Expose test-only helpers" FORCE)
endif()
if(MTCRECV_TESTING)
    target_compile_definitions(mtcreceiver PUBLIC MTCRECV_TESTING)
endif()

if(MTCRECV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
