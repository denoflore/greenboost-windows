# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost Windows Port — Top-level CMake
#
# Builds the portable launcher, CUDA shim DLL and verification tools.
# Target: Windows 10/11 x64 only.
#
# Prerequisites:
#   - Visual Studio C++ workload supported by the installed CMake
#   - CMake 3.20+
#   - Microsoft Detours headers and x64 library
#
# Usage:
#   cmake -S windows-port -B windows-port/build -A x64
#   cmake --build windows-port/build --config Release

cmake_minimum_required(VERSION 3.20)
project(GreenBoost-Windows VERSION 2.4.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Windows port requires Windows and MSVC.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(GB_STATIC_RUNTIME "Use the static MSVC runtime (match a /MT Detours build)" OFF)
if(GB_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# x64 only
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "GreenBoost Windows port requires 64-bit build (x64)")
endif()

# Shared IOCTL header available to all targets
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/driver)

# KMDF driver — requires WDK; conditionally included
option(GB_BUILD_DRIVER "Build the KMDF driver (requires WDK)" OFF)
if(GB_BUILD_DRIVER)
    message(FATAL_ERROR "The legacy KMDF driver requires a WDK project. CMake builds the portable shim and launcher only; see BUILDING.md.")
endif()

# CUDA shim DLL — requires Detours
add_subdirectory(shim)

add_executable(greenboost tools/launch.cpp)
target_compile_features(greenboost PRIVATE cxx_std_11)
target_link_libraries(greenboost PRIVATE gb_detours)

# IOCTL test tool
add_subdirectory(tests)
