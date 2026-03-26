# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost CUDA Shim DLL — CMake build config

cmake_minimum_required(VERSION 3.20)

set(SHIM_SOURCES
    greenboost_cuda_shim_win.c
)

set(SHIM_HEADERS
    greenboost_cuda_shim_win.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver/greenboost_ioctl_win.h
)

add_library(greenboost_cuda SHARED ${SHIM_SOURCES} ${SHIM_HEADERS})

target_include_directories(greenboost_cuda PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver
)

target_compile_definitions(greenboost_cuda PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

# Link against Windows APIs
target_link_libraries(greenboost_cuda PRIVATE
    advapi32   # Registry access
    psapi      # Process info
)

# Microsoft Detours — use it when both header and library are available.
# vcpkg typically installs header as <prefix>/include/detours.h.
# Clear stale cache from previous find_path patterns.
unset(DETOURS_INCLUDE_DIR CACHE)
find_path(DETOURS_INCLUDE_DIR_REAL
    NAMES detours.h
    PATH_SUFFIXES detours
)
find_library(DETOURS_LIBRARY_RELEASE
    NAMES detours
    PATH_SUFFIXES lib
)
find_library(DETOURS_LIBRARY_DEBUG
    NAMES detours
    PATH_SUFFIXES debug/lib
)

if(DETOURS_INCLUDE_DIR_REAL AND (DETOURS_LIBRARY_RELEASE OR DETOURS_LIBRARY_DEBUG))
    message(STATUS "Detours found: ${DETOURS_INCLUDE_DIR_REAL}")
    target_include_directories(greenboost_cuda PRIVATE ${DETOURS_INCLUDE_DIR_REAL})
    if(DETOURS_LIBRARY_RELEASE)
        target_link_libraries(greenboost_cuda PRIVATE optimized ${DETOURS_LIBRARY_RELEASE})
    endif()
    if(DETOURS_LIBRARY_DEBUG)
        target_link_libraries(greenboost_cuda PRIVATE debug ${DETOURS_LIBRARY_DEBUG})
    endif()
    target_compile_definitions(greenboost_cuda PRIVATE GB_USE_DETOURS=1)
else()
    message(STATUS "Detours not found — shim will use manual IAT hooking")
    target_compile_definitions(greenboost_cuda PRIVATE GB_USE_DETOURS=0)
endif()

# DLL exports
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/greenboost_cuda.def)
    set_target_properties(greenboost_cuda PROPERTIES
        LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/greenboost_cuda.def\""
    )
endif()

set_target_properties(greenboost_cuda PROPERTIES
    OUTPUT_NAME "greenboost_cuda"
    SUFFIX ".dll"
)
