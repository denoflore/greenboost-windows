# SPDX-License-Identifier: GPL-2.0-only
# Driver-only library: no cudart dependency. This also supports SDK layouts
# where FindCUDAToolkit cannot locate the optional runtime import library.
find_path(GB_CUDA_INCLUDE cuda.h HINTS "${CUDAToolkit_ROOT}/include" "$ENV{CUDA_PATH}/include" REQUIRED)
find_library(GB_CUDA_DRIVER cuda HINTS "${CUDAToolkit_ROOT}/lib/x64" "$ENV{CUDA_PATH}/lib/x64" REQUIRED)
add_library(greenboost_storage STATIC file_tensor.cpp)
target_compile_features(greenboost_storage PUBLIC cxx_std_17)
target_include_directories(greenboost_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${GB_CUDA_INCLUDE})
target_link_libraries(greenboost_storage PUBLIC ${GB_CUDA_DRIVER})
add_executable(test_storage test_storage.cpp)
target_link_libraries(test_storage PRIVATE greenboost_storage)
