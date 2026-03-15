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
