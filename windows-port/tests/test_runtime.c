/* SPDX-License-Identifier: GPL-2.0-only
 * Exercise the real CUDA runtime loaded after process-start injection.
 * The GPU fills the allocation; the CPU checks only the returned scalar.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define FN(name, args) typedef int (__stdcall *name##_t) args; name##_t name
#define LOAD(name) name = (name##_t)GetProcAddress(runtime, #name); if (!name) return 2
#define CHECK(call) do { int r = (call); if (r) { fprintf(stderr, "%s: CUDA %d\n", #call, r); return 1; } } while (0)
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "Usage: test_runtime.exe cudart.dll device [MB] [expect-managed]\n"); return 2; }
    HMODULE runtime = LoadLibraryA(argv[1]);
    FN(cudaSetDevice, (int)); FN(cudaMalloc, (void **, size_t));
    FN(cudaFree, (void *)); FN(cudaMemset, (void *, int, size_t));
    FN(cudaMemcpy, (void *, const void *, size_t, int));
    FN(cudaDeviceSynchronize, (void)); FN(cudaMemGetInfo, (size_t *, size_t *));
    FN(cuPointerGetAttribute, (void *, int, unsigned long long));
    FN(cuDeviceGetPCIBusId, (char *, int, int));
    size_t bytes = (size_t)(argc > 3 ? atoi(argv[3]) : 512) << 20;
    size_t free_bytes, total; void *ptr = NULL; unsigned value = 0;
    int managed = 0, device = atoi(argv[2]), expect = argc > 4 ? atoi(argv[4]) : 1;
    char pci[32]; ULONGLONG until;
    if (!runtime) { fprintf(stderr, "runtime load: Windows %lu\n", GetLastError()); return 2; }
    LOAD(cudaSetDevice); LOAD(cudaMalloc); LOAD(cudaFree); LOAD(cudaMemset);
    LOAD(cudaMemcpy); LOAD(cudaDeviceSynchronize); LOAD(cudaMemGetInfo);
    CHECK(cudaSetDevice(device));
    cuPointerGetAttribute = (cuPointerGetAttribute_t)GetProcAddress(GetModuleHandleA("nvcuda.dll"), "cuPointerGetAttribute");
    cuDeviceGetPCIBusId = (cuDeviceGetPCIBusId_t)GetProcAddress(GetModuleHandleA("nvcuda.dll"), "cuDeviceGetPCIBusId");
    if (!cuPointerGetAttribute || !cuDeviceGetPCIBusId) return 2;
    CHECK(cuDeviceGetPCIBusId(pci, sizeof(pci), device));
    CHECK(cudaMemGetInfo(&free_bytes, &total));
    CHECK(cudaMalloc(&ptr, bytes));
    CHECK(cuPointerGetAttribute(&managed, 8, (unsigned long long)(uintptr_t)ptr));
    printf("pid=%lu device=%d pci=%s runtime=%s allocation_mb=%zu total_mb=%zu managed=%d\n",
           GetCurrentProcessId(), device, pci, argv[1], bytes >> 20, total >> 20, managed);
    fflush(stdout);
    until = GetTickCount64() + 2500;
    do { CHECK(cudaMemset(ptr, 0x5a, bytes)); CHECK(cudaDeviceSynchronize()); }
    while (GetTickCount64() < until);
    CHECK(cudaMemcpy(&value, (char *)ptr + bytes - sizeof(value), sizeof(value), 2));
    CHECK(cudaFree(ptr));
    printf("gpu_value=%u expected_value=1515870810 freed=1\n", value);
    if (value != 0x5a5a5a5au || !!managed != !!expect) return 1;
    puts("PASS: real CUDA runtime allocation, GPU memset, readback and free");
    return 0;
}
