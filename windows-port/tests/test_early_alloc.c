/* SPDX-License-Identifier: GPL-2.0-only
 * A failed allocation before cuInit must not disable managed memory later.
 * Reuse the real driver allocation/write/read/free check after that failure.
 */
#define main allocation_check
#include "test_cuda.c"
#undef main

int main(int argc, char **argv)
{
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    CUdeviceptr ptr = 0;
    CUresult (__stdcall *allocate)(CUdeviceptr *, size_t);
    CUresult result;
    if (!cuda || argc < 2 || !LoadLibraryA(argv[1])) return 2;
    allocate = (CUresult (__stdcall *)(CUdeviceptr *, size_t))GetProcAddress(cuda, "cuMemAlloc_v2");
    if (!allocate) return 2;
    result = allocate(&ptr, 512ULL << 20);
    printf("early_allocation_result=%d expected=3\n", result);
    if (result != 3) return 1;
    return allocation_check(argc, argv);
}
