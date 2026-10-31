/* SPDX-License-Identifier: GPL-2.0-only
 * NVIDIA's free error must reach the caller, even for a tracked allocation.
 * Release through a saved native entry first to induce a real invalid free.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
typedef unsigned long long ptr_t;
typedef int (__stdcall *init_t)(unsigned);
typedef int (__stdcall *retain_t)(void **, int);
typedef int (__stdcall *current_t)(void *);
typedef int (__stdcall *alloc_t)(ptr_t *, size_t);
typedef int (__stdcall *free_t)(ptr_t);
typedef int (__stdcall *lookup_t)(const char *, void **, int, unsigned long long, int *);
int main(int argc, char **argv)
{
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    init_t init; retain_t retain; current_t current; alloc_t alloc;
    free_t release, native_release; lookup_t lookup;
    ptr_t ptr; void *context; int status = -1, result;
    if (argc != 2 || !cuda) return 2;
    init=(init_t)GetProcAddress(cuda,"cuInit");
    retain=(retain_t)GetProcAddress(cuda,"cuDevicePrimaryCtxRetain");
    current=(current_t)GetProcAddress(cuda,"cuCtxSetCurrent");
    alloc=(alloc_t)GetProcAddress(cuda,"cuMemAlloc_v2");
    release=(free_t)GetProcAddress(cuda,"cuMemFree_v2");
    lookup=(lookup_t)GetProcAddress(cuda,"cuGetProcAddress_v2");
    if (!init || !retain || !current || !alloc || !release || !lookup) return 2;
    if (init(0) || retain(&context,0) || current(context) ||
        lookup("cuMemFree",(void**)&native_release,3020,0,&status) || status) return 2;
    if (!LoadLibraryA(argv[1]) || alloc(&ptr,(size_t)512<<20)) return 2;
    if (native_release(ptr)) return 2;
    result=release(ptr);
    printf("native free succeeded; repeated tracked free returned CUDA %d (expected invalid value 1)\n",result);
    return result == 1 ? 0 : 1;
}
