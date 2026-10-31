/* SPDX-License-Identifier: GPL-2.0-only
 * Compare real NVIDIA lookup results before/after loading the shim.
 * No replacement driver or mocked success is used.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef int (__stdcall *lookup_v1)(const char *, void **, int, unsigned long long);
typedef int (__stdcall *lookup_v2)(const char *, void **, int, unsigned long long, int *);
typedef struct { const char *name; int version; unsigned long long flags; int redirect; } query;
typedef struct { int result, status; void *entry; } answer;
static const query queries[] = {
    {"cuMemAlloc", 3020, 0, 1}, {"cuMemAlloc", 3020, 1, 1},
    {"cuMemAlloc", 3020, 2, 1}, {"cuMemFree", 3020, 0, 1},
    {"cuMemGetInfo", 3020, 0, 1}, {"cuDeviceTotalMem", 3020, 0, 1},
    {"cuGetProcAddress", 11030, 0, 1}, {"cuGetProcAddress", 12000, 0, 1},
    {"cuMemAlloc", 2000, 0, 0}, {"cuMemAlloc", 1, 0, 0},
    {"cuMemAlloc", 999990, 0, 0}, {"cuMemAlloc", 3020, 8, 0},
    {"greenboost_missing_symbol", 12000, 0, 0},
    {"cuMemAllocAsync", 11020, 0, 0}, {"cuMemAllocAsync", 11020, 2, 0},
    {"cuMemAllocManaged", 6000, 0, 0}, {"cuMemCreate", 10020, 0, 0}
};
#define COUNT (sizeof(queries) / sizeof(queries[0]))

static answer ask(lookup_v1 first, lookup_v2 second, int abi, const query *q)
{
    answer a = {0, -1, NULL};
    a.result = abi == 1 ? first(q->name, &a.entry, q->version, q->flags)
                       : second(q->name, &a.entry, q->version, q->flags, &a.status);
    return a;
}

int main(int argc, char **argv)
{
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    lookup_v1 first; lookup_v2 second;
    answer before[2][COUNT]; unsigned i; int abi, checked = 0;
    if (argc != 2 || !cuda) { fprintf(stderr, "Usage: test_lookup.exe shim.dll (NVIDIA driver required)\n"); return 2; }
    first = (lookup_v1)GetProcAddress(cuda, "cuGetProcAddress");
    second = (lookup_v2)GetProcAddress(cuda, "cuGetProcAddress_v2");
    if (!first) { fprintf(stderr, "Driver has no CUDA 11.3 entry-point resolver\n"); return 2; }
    for (abi = 1; abi <= (second ? 2 : 1); ++abi)
        for (i = 0; i < COUNT; ++i) before[abi - 1][i] = ask(first, second, abi, &queries[i]);
    if (!LoadLibraryA(argv[1])) { fprintf(stderr, "Shim load failed: %lu\n", GetLastError()); return 2; }
    for (abi = 1; abi <= (second ? 2 : 1); ++abi) {
        for (i = 0; i < COUNT; ++i) {
            answer old = before[abi - 1][i], now = ask(first, second, abi, &queries[i]);
            int resolved = old.result == 0 && old.entry && (abi == 1 || old.status == 0);
            int redirected = queries[i].redirect && resolved;
            if (old.result != now.result || old.status != now.status ||
                (!!old.entry != !!now.entry) ||
                (redirected ? old.entry == now.entry : old.entry != now.entry)) {
                fprintf(stderr, "FAIL: ABI %d %s version=%d flags=%llu result=%d/%d status=%d/%d\n",
                    abi, queries[i].name, queries[i].version, queries[i].flags,
                    old.result, now.result, old.status, now.status);
                return 1;
            }
            ++checked;
        }
    }
    printf("PASS: %d NVIDIA resolver results preserved or redirected as specified\n", checked);
    return 0;
}
