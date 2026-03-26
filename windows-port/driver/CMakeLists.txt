# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost Windows KMDF Driver — CMake build config

cmake_minimum_required(VERSION 3.20)

set(DRIVER_SOURCES
    greenboost_win.c
)

set(DRIVER_HEADERS
    greenboost_win.h
    greenboost_ioctl_win.h
)

function(gb_find_wdk)
    if(DEFINED WDK_ROOT AND EXISTS "${WDK_ROOT}")
        set(WDK_FOUND TRUE PARENT_SCOPE)
        set(WDK_ROOT_PATH "${WDK_ROOT}" PARENT_SCOPE)
        return()
    endif()

    set(WDK_SEARCH_PATHS
        "$ENV{WDKContentRoot}"
        "$ENV{WindowsSdkDir}"
        "C:/Program Files (x86)/Windows Kits/10"
        "C:/Program Files/Windows Kits/10"
    )

    foreach(search_path IN LISTS WDK_SEARCH_PATHS)
        if(EXISTS "${search_path}")
            if(EXISTS "${search_path}/Include")
                set(WDK_FOUND TRUE PARENT_SCOPE)
                set(WDK_ROOT_PATH "${search_path}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()

    set(WDK_FOUND FALSE PARENT_SCOPE)
    set(WDK_ROOT_PATH "" PARENT_SCOPE)
endfunction()

function(gb_get_wdk_version WDK_ROOT_PATH OUTPUT_VAR)
    file(GLOB wdk_versions "${WDK_ROOT_PATH}/Include/10.0.*")
    if(wdk_versions)
        list(SORT wdk_versions ORDER DESCENDING)
        list(GET wdk_versions 0 latest_version)
        get_filename_component(version_name "${latest_version}" NAME)
        set(${OUTPUT_VAR} "${version_name}" PARENT_SCOPE)
    else()
        set(${OUTPUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

gb_find_wdk()

if(WDK_FOUND)
    gb_get_wdk_version("${WDK_ROOT_PATH}" WDK_VERSION)

    if(WDK_VERSION)
        message(STATUS "WDK found at ${WDK_ROOT_PATH}")
        message(STATUS "WDK version: ${WDK_VERSION}")

        set(WDK_INCLUDE_DIR "${WDK_ROOT_PATH}/Include/${WDK_VERSION}")
        set(WDK_KM_DIR "${WDK_INCLUDE_DIR}/km")
        set(WDK_SHARED_DIR "${WDK_INCLUDE_DIR}/shared")
        set(WDK_UM_DIR "${WDK_INCLUDE_DIR}/um")
        set(WDK_UCRT_DIR "${WDK_INCLUDE_DIR}/ucrt")

        set(WDK_LIB_DIR "${WDK_ROOT_PATH}/Lib/${WDK_VERSION}/km/x64")

        file(GLOB kmdf_versions "${WDK_ROOT_PATH}/Include/wdf/kmdf/*")
        if(kmdf_versions)
            list(SORT kmdf_versions ORDER DESCENDING)
            list(GET kmdf_versions 0 latest_kmdf)
            get_filename_component(KMDF_VERSION "${latest_kmdf}" NAME)
            set(WDK_KMDF_DIR "${WDK_ROOT_PATH}/Include/wdf/kmdf/${KMDF_VERSION}")
            set(WDK_KMDF_LIB_DIR "${WDK_ROOT_PATH}/Lib/wdf/kmdf/x64/${KMDF_VERSION}")
            message(STATUS "KMDF version: ${KMDF_VERSION}")
        else()
            set(WDK_KMDF_DIR "")
            set(WDK_KMDF_LIB_DIR "")
        endif()

        if(EXISTS "${WDK_KM_DIR}" AND EXISTS "${WDK_LIB_DIR}")
            message(STATUS "WDK KM include: ${WDK_KM_DIR}")
            message(STATUS "WDK lib dir: ${WDK_LIB_DIR}")
        else()
            message(WARNING "WDK directories incomplete - KM: ${WDK_KM_DIR}, Lib: ${WDK_LIB_DIR}")
            set(WDK_FOUND FALSE)
        endif()
    else()
        message(WARNING "WDK found but no version detected in ${WDK_ROOT_PATH}/Include")
        set(WDK_FOUND FALSE)
    endif()
endif()

add_library(greenboost_driver SHARED ${DRIVER_SOURCES} ${DRIVER_HEADERS})

target_compile_definitions(greenboost_driver PRIVATE
    _KERNEL_MODE
    _AMD64_
    NTDDI_VERSION=0x0A000007
    _WIN32_WINNT=0x0A00
    _WIN64
    _NDEBUG
)

if(WDK_FOUND)
    message(STATUS "Configuring driver with WDK paths for .sys output")

    target_include_directories(greenboost_driver PRIVATE
        "${WDK_KM_DIR}"
        "${WDK_SHARED_DIR}"
        "${WDK_UM_DIR}"
        "${WDK_UCRT_DIR}"
        "${WDK_KMDF_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_compile_options(greenboost_driver PRIVATE
        /kernel
        /Zc:wchar_t-
        /Gd
        /Zl
        /O2
        /Oy-
        /GL-
        /MP
    )

    set(WDK_KERNEL_LIBS
        "${WDK_LIB_DIR}/BufferOverflowK.lib"
        "${WDK_LIB_DIR}/ntoskrnl.lib"
        "${WDK_LIB_DIR}/hal.lib"
        "${WDK_LIB_DIR}/wmilib.lib"
        "${WDK_LIB_DIR}/wdm.lib"
        "${WDK_KMDF_LIB_DIR}/wdfdriverentry.lib"
        "${WDK_KMDF_LIB_DIR}/wdfldr.lib"
    )

    target_link_libraries(greenboost_driver PRIVATE ${WDK_KERNEL_LIBS})

    set_target_properties(greenboost_driver PROPERTIES
        SUFFIX ".sys"
        OUTPUT_NAME "greenboost_win"
        LINK_FLAGS "/DRIVER /SUBSYSTEM:NATIVE /MANIFEST:NO"
        EXCLUDE_FROM_ALL FALSE
    )

else()
    message(STATUS "WDK not found - driver target is syntax-check only")
    message(STATUS "To enable WDK, set WDK_ROOT or install WDK to default location")

    target_include_directories(greenboost_driver PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    set_target_properties(greenboost_driver PROPERTIES
        EXCLUDE_FROM_ALL TRUE
    )
endif()
