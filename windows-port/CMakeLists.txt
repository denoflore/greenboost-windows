# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost Windows Port — Top-level CMake
#
# Builds both the KMDF driver and the CUDA shim DLL.
# Target: Windows 10/11 x64 only.
#
# Prerequisites:
#   - Visual Studio 2022 with C++ workload
#   - Windows Driver Kit (WDK) 10/11
#   - CMake 3.20+
#   - Microsoft Detours (NuGet: Microsoft.Detours)
#
# Usage:
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.20)
project(GreenBoost-Windows VERSION 2.3.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# x64 only
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "GreenBoost Windows port requires 64-bit build (x64)")
endif()

# Shared IOCTL header available to all targets
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/driver)

# KMDF driver — requires WDK; conditionally included
option(GB_BUILD_DRIVER "Build the KMDF driver (requires WDK)" OFF)
if(GB_BUILD_DRIVER)
    add_subdirectory(driver)
endif()

# CUDA shim DLL — requires Detours
add_subdirectory(shim)

# IOCTL test tool
add_subdirectory(tests)
