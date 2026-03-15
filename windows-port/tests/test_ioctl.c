/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024-2026 Ferran Duarri. Dual-licensed: GPL v2 + Commercial.
 *
 * GreenBoost v2.3 — IOCTL Smoke Tests (Windows)
 *
 * Tests the KMDF driver's IOCTL interface from userspace.
 * Compile: cl /W4 test_ioctl.c /link advapi32.lib
 * Run: test_ioctl.exe
 *
 * Author  : Ferran Duarri
 * License : GPL v2 (open-source) / Commercial — see LICENSE
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greenboost_ioctl_win.h"

/* ------------------------------------------------------------------ */
/*  Test helpers                                                        */
/* ------------------------------------------------------------------ */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    do { tests_run++; printf("TEST %d: %s ... ", tests_run, name); } while(0)

#define TEST_PASS() \
    do { tests_passed++; printf("PASS\n"); } while(0)

#define TEST_FAIL(fmt, ...) \
    do { tests_failed++; printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)

#define TEST_ASSERT(cond, fmt, ...) \
    do { if (!(cond)) { TEST_FAIL(fmt, ##__VA_ARGS__); return; } } while(0)

static HANDLE open_device(void)
{
    HANDLE h = CreateFileW(
        L"\\\\.\\GreenBoost",
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    return h;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_device_open(void)
{
    HANDLE h;

    TEST_START("device open");

    h = open_device();
    TEST_ASSERT(h != INVALID_HANDLE_VALUE,
                "CreateFileW failed (err=%lu) — is the driver loaded?",
                GetLastError());

    CloseHandle(h);
    TEST_PASS();
}

static void test_get_info(void)
{
    HANDLE h;
    struct gb_info_win info = { 0 };
    DWORD bytesReturned;
    BOOL ok;

    TEST_START("GB_IOCTL_GET_INFO");

    h = open_device();
    TEST_ASSERT(h != INVALID_HANDLE_VALUE,
                "device not accessible");

    ok = DeviceIoControl(h, GB_IOCTL_GET_INFO,
                         NULL, 0,
                         &info, sizeof(info),
                         &bytesReturned, NULL);
    CloseHandle(h);

    TEST_ASSERT(ok, "DeviceIoControl failed (err=%lu)", GetLastError());
    TEST_ASSERT(bytesReturned == sizeof(info),
                "unexpected output size: %lu (expected %zu)",
                bytesReturned, sizeof(info));

    printf("\n");
    printf("    === Pool Info ===\n");
    printf("    T1 VRAM physical  : %llu MB\n", info.vram_physical_mb);
    printf("    T2 total RAM      : %llu MB\n", info.total_ram_mb);
    printf("    T2 free RAM       : %llu MB\n", info.free_ram_mb);
    printf("    T2 allocated      : %llu MB\n", info.allocated_mb);
    printf("    T2 max pool       : %llu MB\n", info.max_pool_mb);
    printf("    T2 safety reserve : %llu MB\n", info.safety_reserve_mb);
    printf("    T2 available      : %llu MB\n", info.available_mb);
    printf("    Active buffers    : %u\n", info.active_buffers);
    printf("    OOM active        : %u\n", info.oom_active);
    printf("    T3 NVMe total     : %llu MB\n", info.nvme_swap_total_mb);
    printf("    T3 allocated      : %llu MB\n", info.nvme_t3_allocated_mb);
    printf("    Swap pressure     : %u\n", info.swap_pressure);
    printf("    Combined          : %llu MB\n", info.total_combined_mb);
    printf("    ");

    TEST_ASSERT(info.vram_physical_mb > 0,
                "vram_physical_mb should be > 0");
    TEST_ASSERT(info.total_ram_mb > 0,
                "total_ram_mb should be > 0");
    TEST_ASSERT(info.max_pool_mb > 0,
                "max_pool_mb should be > 0");

    TEST_PASS();
}

static void test_alloc_free(void)
{
    HANDLE h;
    struct gb_alloc_req_win req = { 0 };
    struct gb_alloc_req_win resp = { 0 };
    DWORD bytesReturned;
    BOOL ok;
    PVOID mapped;

    TEST_START("GB_IOCTL_ALLOC + MapViewOfFile + free");

    h = open_device();
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "device not accessible");

    /* Allocate 2 MB */
    req.size = 2 * 1024 * 1024;
    req.flags = GB_ALLOC_WEIGHTS;

    ok = DeviceIoControl(h, GB_IOCTL_ALLOC,
                         &req, sizeof(req),
                         &resp, sizeof(resp),
                         &bytesReturned, NULL);

    TEST_ASSERT(ok, "IOCTL_ALLOC failed (err=%lu)", GetLastError());
    TEST_ASSERT(resp.buf_id > 0, "buf_id should be > 0 (got %d)", resp.buf_id);
    TEST_ASSERT(resp.handle != NULL, "section handle should not be NULL");

    printf("(id=%d handle=%p size=%llu) ", resp.buf_id, resp.handle, resp.size);

    /* Map into our address space */
    mapped = MapViewOfFile(resp.handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)resp.size);
    TEST_ASSERT(mapped != NULL, "MapViewOfFile failed (err=%lu)", GetLastError());

    /* Write a pattern to verify the memory works */
    memset(mapped, 0xAB, (SIZE_T)resp.size);

    /* Verify the pattern */
    {
        unsigned char *p = (unsigned char *)mapped;
        TEST_ASSERT(p[0] == 0xAB && p[resp.size - 1] == 0xAB,
                    "memory verification failed");
    }

    /* Unmap and close */
    UnmapViewOfFile(mapped);
    CloseHandle(resp.handle);
    CloseHandle(h);

    TEST_PASS();
}

static void test_alloc_multiple(void)
{
    HANDLE h;
    int i;
    struct gb_alloc_req_win reqs[8] = { 0 };
    struct gb_alloc_req_win resps[8] = { 0 };
    DWORD bytesReturned;
    BOOL ok;

    TEST_START("multiple allocations (8 x 4MB)");

    h = open_device();
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "device not accessible");

    /* Allocate 8 buffers */
    for (i = 0; i < 8; i++) {
        reqs[i].size = 4 * 1024 * 1024;
        reqs[i].flags = GB_ALLOC_WEIGHTS;

        ok = DeviceIoControl(h, GB_IOCTL_ALLOC,
                             &reqs[i], sizeof(reqs[i]),
                             &resps[i], sizeof(resps[i]),
                             &bytesReturned, NULL);
        TEST_ASSERT(ok, "ALLOC[%d] failed (err=%lu)", i, GetLastError());
        TEST_ASSERT(resps[i].buf_id > 0, "buf_id[%d] should be > 0", i);
    }

    printf("(ids: ");
    for (i = 0; i < 8; i++) printf("%d ", resps[i].buf_id);
    printf(") ");

    /* Free all */
    for (i = 0; i < 8; i++) {
        if (resps[i].handle)
            CloseHandle(resps[i].handle);
    }

    CloseHandle(h);
    TEST_PASS();
}

static void test_madvise(void)
{
    HANDLE h;
    struct gb_alloc_req_win alloc_req = { 0 };
    struct gb_alloc_req_win alloc_resp = { 0 };
    struct gb_madvise_req_win madv_req = { 0 };
    DWORD bytesReturned;
    BOOL ok;

    TEST_START("GB_IOCTL_MADVISE (HOT/COLD/FREEZE)");

    h = open_device();
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "device not accessible");

    /* Allocate a buffer first */
    alloc_req.size = 2 * 1024 * 1024;
    alloc_req.flags = GB_ALLOC_WEIGHTS;
    ok = DeviceIoControl(h, GB_IOCTL_ALLOC,
                         &alloc_req, sizeof(alloc_req),
                         &alloc_resp, sizeof(alloc_resp),
                         &bytesReturned, NULL);
    TEST_ASSERT(ok, "ALLOC failed");

    /* MADVISE HOT */
    madv_req.buf_id = alloc_resp.buf_id;
    madv_req.advise = GB_MADVISE_HOT;
    ok = DeviceIoControl(h, GB_IOCTL_MADVISE,
                         &madv_req, sizeof(madv_req),
                         NULL, 0, &bytesReturned, NULL);
    TEST_ASSERT(ok, "MADVISE HOT failed (err=%lu)", GetLastError());

    /* MADVISE COLD */
    madv_req.advise = GB_MADVISE_COLD;
    ok = DeviceIoControl(h, GB_IOCTL_MADVISE,
                         &madv_req, sizeof(madv_req),
                         NULL, 0, &bytesReturned, NULL);
    TEST_ASSERT(ok, "MADVISE COLD failed (err=%lu)", GetLastError());

    /* MADVISE FREEZE */
    madv_req.advise = GB_MADVISE_FREEZE;
    ok = DeviceIoControl(h, GB_IOCTL_MADVISE,
                         &madv_req, sizeof(madv_req),
                         NULL, 0, &bytesReturned, NULL);
    TEST_ASSERT(ok, "MADVISE FREEZE failed (err=%lu)", GetLastError());

    /* Cleanup */
    if (alloc_resp.handle) CloseHandle(alloc_resp.handle);
    CloseHandle(h);
    TEST_PASS();
}

static void test_evict(void)
{
    HANDLE h;
    struct gb_alloc_req_win alloc_req = { 0 };
    struct gb_alloc_req_win alloc_resp = { 0 };
    struct gb_evict_req_win evict_req = { 0 };
    DWORD bytesReturned;
    BOOL ok;

    TEST_START("GB_IOCTL_EVICT (T2 -> T3)");

    h = open_device();
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "device not accessible");

    /* Allocate */
    alloc_req.size = 2 * 1024 * 1024;
    alloc_req.flags = GB_ALLOC_WEIGHTS;
    ok = DeviceIoControl(h, GB_IOCTL_ALLOC,
                         &alloc_req, sizeof(alloc_req),
                         &alloc_resp, sizeof(alloc_resp),
                         &bytesReturned, NULL);
    TEST_ASSERT(ok, "ALLOC failed");

    /* Evict */
    evict_req.buf_id = alloc_resp.buf_id;
    ok = DeviceIoControl(h, GB_IOCTL_EVICT,
                         &evict_req, sizeof(evict_req),
                         NULL, 0, &bytesReturned, NULL);
    TEST_ASSERT(ok, "EVICT failed (err=%lu)", GetLastError());

    /* Cleanup */
    if (alloc_resp.handle) CloseHandle(alloc_resp.handle);
    CloseHandle(h);
    TEST_PASS();
}

static void test_stress_alloc_free(void)
{
    HANDLE h;
    int i;
    int cycles = 100;
    int failures = 0;

    TEST_START("stress test (100 alloc/free cycles x 2MB)");

    h = open_device();
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "device not accessible");

    for (i = 0; i < cycles; i++) {
        struct gb_alloc_req_win req = { 0 };
        struct gb_alloc_req_win resp = { 0 };
        DWORD bytesReturned;

        req.size = 2 * 1024 * 1024;
        req.flags = GB_ALLOC_WEIGHTS;

        if (!DeviceIoControl(h, GB_IOCTL_ALLOC,
                             &req, sizeof(req),
                             &resp, sizeof(resp),
                             &bytesReturned, NULL)) {
            failures++;
            continue;
        }

        if (resp.handle)
            CloseHandle(resp.handle);
    }

    CloseHandle(h);

    printf("(%d/%d succeeded) ", cycles - failures, cycles);
    TEST_ASSERT(failures == 0, "%d allocation failures", failures);
    TEST_PASS();
}

static void test_pressure_event(void)
{
    HANDLE hEvent;

    TEST_START("pressure event accessibility");

    hEvent = OpenEventW(SYNCHRONIZE, FALSE, L"GreenBoostPressure");
    TEST_ASSERT(hEvent != NULL,
                "OpenEvent failed (err=%lu) — is the driver loaded?",
                GetLastError());

    printf("(handle=%p) ", hEvent);
    CloseHandle(hEvent);
    TEST_PASS();
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int skip_driver_tests = 0;
    HANDLE h;

    printf("=== GreenBoost v2.3 IOCTL Test Suite ===\n\n");

    /* Quick check if driver is available */
    h = open_device();
    if (h == INVALID_HANDLE_VALUE) {
        printf("NOTE: \\\\.\\.GreenBoost not accessible (err=%lu)\n", GetLastError());
        printf("Driver may not be loaded. Running header-only validation.\n\n");
        skip_driver_tests = 1;
    } else {
        CloseHandle(h);
    }

    /* Header validation (always runs) */
    printf("--- IOCTL Header Validation ---\n");
    printf("  GB_IOCTL_ALLOC        = 0x%08X\n", GB_IOCTL_ALLOC);
    printf("  GB_IOCTL_GET_INFO     = 0x%08X\n", GB_IOCTL_GET_INFO);
    printf("  GB_IOCTL_RESET        = 0x%08X\n", GB_IOCTL_RESET);
    printf("  GB_IOCTL_MADVISE      = 0x%08X\n", GB_IOCTL_MADVISE);
    printf("  GB_IOCTL_EVICT        = 0x%08X\n", GB_IOCTL_EVICT);
    printf("  GB_IOCTL_POLL_FD      = 0x%08X\n", GB_IOCTL_POLL_FD);
    printf("  GB_IOCTL_PIN_USER_PTR = 0x%08X\n", GB_IOCTL_PIN_USER_PTR);
    printf("  sizeof(gb_alloc_req_win)   = %zu\n", sizeof(struct gb_alloc_req_win));
    printf("  sizeof(gb_info_win)        = %zu\n", sizeof(struct gb_info_win));
    printf("  sizeof(gb_madvise_req_win) = %zu\n", sizeof(struct gb_madvise_req_win));
    printf("  sizeof(gb_evict_req_win)   = %zu\n", sizeof(struct gb_evict_req_win));
    printf("  sizeof(gb_poll_req_win)    = %zu\n", sizeof(struct gb_poll_req_win));
    printf("  sizeof(gb_pin_req_win)     = %zu\n", sizeof(struct gb_pin_req_win));
    printf("\n");

    if (skip_driver_tests) {
        printf("Skipping driver tests (device not available).\n");
        printf("\n=== Results: Header validation only ===\n");
        return 0;
    }

    /* Driver tests */
    printf("--- Driver Tests ---\n");
    test_device_open();
    test_get_info();
    test_alloc_free();
    test_alloc_multiple();
    test_madvise();
    test_evict();
    test_pressure_event();

    printf("\n--- Stress Tests ---\n");
    test_stress_alloc_free();

    /* Summary */
    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
