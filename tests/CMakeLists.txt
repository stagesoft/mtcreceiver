# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>

include(FetchContent)

# Prefer a system gtest if one is already available, otherwise fetch v1.14.0.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# rtmidi needs to be resolvable in standalone mode; in submodule mode the
# parent usually sets RTMIDI_* vars. Use pkg-config as a fallback.
if(NOT RTMIDI_LIBRARIES)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RTMIDI rtmidi)
    endif()
endif()

add_executable(test_callback test_callback.cpp)
target_link_libraries(test_callback
    PRIVATE
    mtcreceiver
    GTest::gtest_main
    ${RTMIDI_LIBRARIES}
    pthread
)
target_include_directories(test_callback PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${RTMIDI_INCLUDE_DIRS}
)

add_executable(test_no_plus_two_compensation test_no_plus_two_compensation.cpp)
target_link_libraries(test_no_plus_two_compensation
    PRIVATE
    mtcreceiver
    GTest::gtest_main
    ${RTMIDI_LIBRARIES}
    pthread
)
target_include_directories(test_no_plus_two_compensation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${RTMIDI_INCLUDE_DIRS}
)

include(GoogleTest)
gtest_discover_tests(test_callback)
gtest_discover_tests(test_no_plus_two_compensation)
