/* SPDX-License-Identifier: GPL-2.0-only
 * Compare real driver capacity before injection with the configured allowance.
 * These queries inspect scalar memory accounting; they perform no workload.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
typedef int (__stdcall *init_t)(unsigned);
typedef int (__stdcall *total_t)(size_t *, int);
int main(int argc, char **argv)
{
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    init_t init = cuda ? (init_t)GetProcAddress(cuda, "cuInit") : NULL;
    total_t total = cuda ? (total_t)GetProcAddress(cuda, "cuDeviceTotalMem_v2") : NULL;
    int a, b; size_t before_a, before_b, after_a, after_b, extra;
    if (argc != 5 || !init || !total) return 2;
    a = atoi(argv[2]); b = atoi(argv[3]); extra = (size_t)strtoull(argv[4], NULL, 10) << 20;
    if (init(0) || total(&before_a, a) || total(&before_b, b)) return 2;
    if (!LoadLibraryA(argv[1])) return 2;
    if (total(&after_a, a) || total(&after_b, b)) return 2;
    printf("device=%d physical_mb=%zu reported_mb=%zu extra_mb=%zu\n", a, before_a >> 20, after_a >> 20, (after_a-before_a) >> 20);
    printf("device=%d physical_mb=%zu reported_mb=%zu extra_mb=%zu\n", b, before_b >> 20, after_b >> 20, (after_b-before_b) >> 20);
    if (after_a != before_a + extra || after_b != before_b + extra) return 1;
    puts("PASS: per-device physical capacity plus configured allowance");
    return 0;
}
