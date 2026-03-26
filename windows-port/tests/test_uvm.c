/* SPDX-License-Identifier: GPL-2.0-only
 * GreenBoost v2.3 -- UVM allocation test
 *
 * Build: cl /O2 test_uvm.c /link cuda.lib
 * Run:   withdll.exe /d:greenboost_cuda.dll test_uvm.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <cuda_runtime.h>

#define TEST_SMALL   (64  * 1024 * 1024)
#define TEST_LARGE   (512 * 1024 * 1024)
#define TEST_MEDIUM  (256 * 1024 * 1024)

static int n_run = 0, n_pass = 0;

#define RUN(name)  do { n_run++; printf("Test %d: %s ... ", n_run, name); fflush(stdout); } while(0)
#define PASS()     do { n_pass++; printf("PASS\n"); } while(0)
#define FAIL(f,...)do { printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

int main(void)
{
    void *ptr = NULL;
    cudaError_t err;

    printf("=== GreenBoost UVM Tests ===\n\n");

    /* 1: Small alloc -- below threshold, passthrough */
    RUN("small alloc passthrough");
    err = cudaMalloc(&ptr, TEST_SMALL);
    if (err == cudaSuccess && ptr) { cudaFree(ptr); ptr = NULL; PASS(); }
    else FAIL("err=%d", err);

    /* 2: Large alloc -- above threshold, intercepted */
    RUN("large alloc interception");
    err = cudaMalloc(&ptr, TEST_LARGE);
    if (err == cudaSuccess && ptr) PASS();
    else FAIL("err=%d", err);

    /* 3: GPU memset on intercepted buffer */
    if (ptr) {
        RUN("GPU memset on intercepted buffer");
        err = cudaMemset(ptr, 0xAB, TEST_LARGE);
        if (err == cudaSuccess) {
            err = cudaDeviceSynchronize();
            if (err == cudaSuccess) PASS();
            else FAIL("sync err=%d", err);
        } else FAIL("memset err=%d", err);
    }

    /* 4: Free intercepted buffer */
    if (ptr) {
        RUN("free intercepted buffer");
        err = cudaFree(ptr);
        if (err == cudaSuccess) PASS();
        else FAIL("err=%d", err);
        ptr = NULL;
    }

    /* 5: Multiple large allocs */
    RUN("4x 256MB allocs");
    {
        void *p[4] = {0};
        int i, ok = 1;
        for (i = 0; i < 4; i++) {
            if (cudaMalloc(&p[i], TEST_MEDIUM) != cudaSuccess) { ok = 0; break; }
        }
        if (ok) {
            for (i = 0; i < 4; i++) cudaMemset(p[i], (unsigned char)i, TEST_MEDIUM);
            cudaDeviceSynchronize();
            PASS();
        } else FAIL("alloc %d failed", i);
        for (i = 0; i < 4; i++) if (p[i]) cudaFree(p[i]);
    }

    /* 6: VRAM spoofing active */
    RUN("VRAM spoofing reports extended memory");
    {
        size_t fr = 0, tot = 0;
        err = cudaMemGetInfo(&fr, &tot);
        if (err == cudaSuccess) {
            printf("(total=%zuMB free=%zuMB) ", tot >> 20, fr >> 20);
            if (tot > (size_t)24ULL * 1024 * 1024 * 1024) PASS();
            else FAIL("total not spoofed -- shim may not be active");
        } else FAIL("err=%d", err);
    }

    printf("\n=== %d/%d passed ===\n", n_pass, n_run);
    return (n_pass == n_run) ? 0 : 1;
}
