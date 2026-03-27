# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost IOCTL test tool

cmake_minimum_required(VERSION 3.20)

add_executable(test_ioctl test_ioctl.c)

target_include_directories(test_ioctl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver
)

target_compile_definitions(test_ioctl PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
)

# UVM test -- requires CUDA SDK (optional)
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    message(STATUS "CUDA SDK found -- building test_uvm")
    add_executable(test_uvm test_uvm.c)
    target_include_directories(test_uvm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../driver
    )
    target_compile_definitions(test_uvm PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(test_uvm PRIVATE CUDA::cudart)
else()
    message(STATUS "CUDA SDK not found -- skipping test_uvm")
endif()
