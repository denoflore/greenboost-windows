/* SPDX-License-Identifier: GPL-2.0-only
 * Exercise the installed CUDA driver through both export and entry-point lookup.
 * No toolkit or kernel driver is needed for this bounded interception check.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef int CUresult;
typedef unsigned long long CUdeviceptr;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
#define FN(name, args) typedef CUresult (__stdcall *name##_t) args; name##_t name
#define LOAD(name) name = (name##_t)GetProcAddress(cuda, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 2; }
#define CHECK(call) do { CUresult r = (call); if (r) { fprintf(stderr, "%s failed: CUDA %d\n", #call, r); return 1; } } while (0)

int main(int argc, char **argv)
{
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    FN(cuInit, (unsigned));
    FN(cuDeviceGetName, (char *, int, int));
    FN(cuDeviceGetPCIBusId, (char *, int, int));
    FN(cuDeviceGetAttribute, (int *, int, int));
    FN(cuDevicePrimaryCtxRetain, (CUcontext *, int));
    FN(cuCtxSetCurrent, (CUcontext));
    FN(cuCtxSynchronize, (void));
    FN(cuDeviceTotalMem_v2, (size_t *, int));
    FN(cuMemGetInfo_v2, (size_t *, size_t *));
    FN(cuMemAlloc_v2, (CUdeviceptr *, size_t));
    FN(cuMemFree_v2, (CUdeviceptr));
    FN(cuGetProcAddress, (const char *, void **, int, unsigned long long));
    FN(cuGetProcAddress_v2, (const char *, void **, int, unsigned long long, int *));
    FN(cuPointerGetAttribute, (void *, int, CUdeviceptr));
    FN(cuModuleLoadData, (CUmodule *, const void *));
    FN(cuModuleGetFunction, (CUfunction *, CUmodule, const char *));
    FN(cuLaunchKernel, (CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void *, void **, void **));
    FN(cuMemcpyDtoH_v2, (void *, CUdeviceptr, size_t));
    FN(cuModuleUnload, (CUmodule));
    FN(cuDevicePrimaryCtxRelease, (int));
    const char *shim = argc > 1 ? argv[1] : "-";
    int lookup = argc > 2 ? atoi(argv[2]) : 1;
    int device = argc > 3 ? atoi(argv[3]) : 0;
    size_t bytes = (size_t)(argc > 4 ? atoi(argv[4]) : 512) << 20;
    int expect = argc > 5 ? atoi(argv[5]) : 1;
    char name[128], pci[32];
    size_t physical = 0, total = 0, free_bytes = 0;
    int concurrent = 0, managed = 0, memory_type = 0;
    CUcontext context;
    CUdeviceptr ptr = 0;
    CUmodule module;
    CUfunction kernel;
    unsigned value = 0;
    unsigned long long count = bytes / sizeof(unsigned);
    ULONGLONG until;
    void *params[] = { &ptr, &count };
    const char *ptx =
        ".version 6.0\n.target sm_52\n.address_size 64\n"
        ".visible .entry fill(.param .u64 p, .param .u64 n) {\n"
        ".reg .pred q; .reg .b32 r<5>; .reg .b64 d<6>;\n"
        "ld.param.u64 d0,[p]; ld.param.u64 d1,[n];\n"
        "mov.u32 r0,%ctaid.x; mov.u32 r1,%ntid.x; mov.u32 r2,%tid.x;\n"
        "mad.wide.u32 d2,r0,r1,0; cvt.u64.u32 d3,r2; add.u64 d2,d2,d3;\n"
        "mov.u32 r3,%nctaid.x; mul.wide.u32 d4,r3,r1;\n"
        "loop: setp.ge.u64 q,d2,d1; @q bra done;\n"
        "mul.lo.u64 d3,d2,4; add.u64 d5,d0,d3; st.global.u32 [d5],305419896;\n"
        "add.u64 d2,d2,d4; bra loop; done: ret; }\n";
    if (!cuda) { fprintf(stderr, "nvcuda.dll unavailable\n"); return 2; }
    LOAD(cuInit); LOAD(cuDeviceGetName); LOAD(cuDeviceGetPCIBusId);
    LOAD(cuDeviceGetAttribute); LOAD(cuDevicePrimaryCtxRetain); LOAD(cuCtxSetCurrent);
    LOAD(cuCtxSynchronize); LOAD(cuDeviceTotalMem_v2); LOAD(cuMemGetInfo_v2);
    LOAD(cuMemAlloc_v2); LOAD(cuMemFree_v2); LOAD(cuGetProcAddress);
    LOAD(cuGetProcAddress_v2);
    LOAD(cuPointerGetAttribute); LOAD(cuModuleLoadData); LOAD(cuModuleGetFunction);
    LOAD(cuLaunchKernel); LOAD(cuMemcpyDtoH_v2); LOAD(cuModuleUnload); LOAD(cuDevicePrimaryCtxRelease);
    CHECK(cuInit(0));
    CHECK(cuDeviceGetName(name, sizeof(name), device));
    CHECK(cuDeviceGetPCIBusId(pci, sizeof(pci), device));
    CHECK(cuDeviceGetAttribute(&concurrent, 89, device));
    CHECK(cuDeviceTotalMem_v2(&physical, device));
    CHECK(cuDevicePrimaryCtxRetain(&context, device));
    CHECK(cuCtxSetCurrent(context));
    printf("pid=%lu device=%d name=%s pci=%s concurrentManagedAccess=%d physical_mb=%zu\n",
           GetCurrentProcessId(), device, name, pci, concurrent, physical >> 20);
    fflush(stdout);
    if (shim[0] != '-' && !LoadLibraryA(shim)) {
        fprintf(stderr, "shim load failed: %lu\n", GetLastError()); return 2;
    }
    if (lookup == 2) {
        int status = -1;
        CHECK(cuGetProcAddress_v2("cuMemGetInfo", (void **)&cuMemGetInfo_v2, 3020, 0, &status));
        if (status) return 2;
        CHECK(cuGetProcAddress_v2("cuMemAlloc", (void **)&cuMemAlloc_v2, 3020, 0, &status));
        if (status) return 2;
        CHECK(cuGetProcAddress_v2("cuMemFree", (void **)&cuMemFree_v2, 3020, 0, &status));
        if (status) return 2;
    } else if (lookup) {
        CHECK(cuGetProcAddress("cuMemGetInfo", (void **)&cuMemGetInfo_v2, 3020, 0));
        CHECK(cuGetProcAddress("cuMemAlloc", (void **)&cuMemAlloc_v2, 3020, 0));
        CHECK(cuGetProcAddress("cuMemFree", (void **)&cuMemFree_v2, 3020, 0));
    }
    CHECK(cuMemGetInfo_v2(&free_bytes, &total));
    CHECK(cuMemAlloc_v2(&ptr, bytes));
    CHECK(cuPointerGetAttribute(&managed, 8, ptr));
    CHECK(cuPointerGetAttribute(&memory_type, 2, ptr));
    printf("lookup=%d total_mb=%zu allocation_mb=%zu managed=%d memory_type=%d\n",
           lookup, total >> 20, bytes >> 20, managed, memory_type);
    fflush(stdout);
    CHECK(cuModuleLoadData(&module, ptx));
    CHECK(cuModuleGetFunction(&kernel, module, "fill"));
    until = GetTickCount64() + 2500;
    do {
        CHECK(cuLaunchKernel(kernel, 256, 1, 1, 256, 1, 1, 0, NULL, params, NULL));
        CHECK(cuCtxSynchronize());
    } while (GetTickCount64() < until);
    CHECK(cuMemcpyDtoH_v2(&value, ptr + bytes - sizeof(value), sizeof(value)));
    CHECK(cuMemFree_v2(ptr));
    CHECK(cuModuleUnload(module));
    CHECK(cuDevicePrimaryCtxRelease(device));
    printf("gpu_value=%u expected_value=305419896 freed=1\n", value);
    if (value != 305419896u) return 1;
    if (expect && !(managed || memory_type == 1)) {
        fprintf(stderr, "FAIL: allocation bypassed GreenBoost\n"); return 1;
    }
    if (!expect && (managed || memory_type == 1)) {
        fprintf(stderr, "FAIL: passthrough allocation was intercepted\n"); return 1;
    }
    printf("PASS: CUDA allocation, GPU write, readback and free\n");
    return 0;
}
