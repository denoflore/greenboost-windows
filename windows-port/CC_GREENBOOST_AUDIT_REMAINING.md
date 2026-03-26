# CC BUILD: GreenBoost Windows Audit Cleanup (4 Remaining Issues)

## EXECUTION PATTERN

**Follow this iterative pattern:**
1. Read a section of this file
2. Build what that section specifies
3. Test/validate what you built
4. Read the file AGAIN to find where you are
5. Continue to next section
6. Repeat until complete

**DO NOT try to build everything at once.**

---

## PROJECT OVERVIEW

GreenBoost-windows is a KMDF driver + CUDA shim DLL for extending GPU VRAM with system RAM. A full audit found 20 issues; 16 have been fixed. This spec covers the remaining 4. All are low-risk, bounded changes.

**Repo**: `denoflore/GreenBoost-windows`
**Branch**: `main`

---

## REQUIRED READING

Before writing ANY code, read these files completely:

1. **`windows-port/AUDIT_2026_03_26.md`** -- The full audit report. Issues #11, #13, #16, #19 are yours.
2. **`windows-port/driver/greenboost_win.inf`** -- The INF file (issue #11)
3. **`windows-port/shim/greenboost_cuda_shim_win.h`** -- Hash table constants (issue #13)
4. **`windows-port/shim/greenboost_cuda_shim_win.c`** -- Hash table implementation (issue #13)
5. **`windows-port/sign.ps1`** -- Chinese signing script (issue #16)
6. **`windows-port/tests/test_uvm.c`** -- UVM test (issue #19)
7. **`windows-port/tests/CMakeLists.txt`** -- Test build config (issue #19)

---

## CRITICAL REQUIREMENTS

**DO NOT** modify any file not listed in the phases below. The driver .c, driver .h, shim .c, install.ps1, config.ps1, diagnose.ps1, and build.ps1 were JUST fixed in the previous commit batch. Do not touch them.

**DO NOT** change any IOCTL codes, struct layouts, or function signatures.

**DO NOT** use Ollama. Anywhere. Ever.

---

## Repo Consciousness Brief

### What This Repo IS
A Windows port of a Linux kernel module (greenboost.ko) that provides 3-tier GPU memory pooling (VRAM + DDR4 + NVMe). Two main components: a KMDF kernel driver and a userspace CUDA shim DLL that intercepts CUDA allocations.

### What Success Looks Like
All 4 remaining audit items resolved. No regressions. The INF installs cleanly, the hash table handles high-churn workloads, the signing script is readable, and the UVM test compiles.

### Protected Zones (DO NOT MODIFY)
- `windows-port/driver/greenboost_win.c` -- JUST fixed
- `windows-port/driver/greenboost_win.h` -- JUST fixed
- `windows-port/shim/greenboost_cuda_shim_win.c` -- JUST fixed
- `windows-port/tools/install.ps1` -- JUST fixed
- `windows-port/tools/config.ps1` -- already correct
- `windows-port/tools/diagnose.ps1` -- JUST fixed
- `windows-port/build.ps1` -- JUST fixed

### Active Build Zone
- `windows-port/driver/greenboost_win.inf` (Phase 1)
- `windows-port/shim/greenboost_cuda_shim_win.h` (Phase 2 -- header only, NOT .c)
- `windows-port/sign.ps1` (Phase 3)
- `windows-port/tests/test_uvm.c` (Phase 4 -- minor edit)
- `windows-port/tests/CMakeLists.txt` (Phase 4)

---

## Phase 1: Fix INF KmdfLibraryVersion Placeholder (Audit #11)

**Goal:** Replace the `$KMDFVERSION$` placeholder with a concrete KMDF version so the INF works without stampinf.exe preprocessing.

**File to modify:** `windows-port/driver/greenboost_win.inf`

**Change:**
Find this line:
```ini
KmdfLibraryVersion = $KMDFVERSION$
```

Replace with:
```ini
KmdfLibraryVersion = 1.33
```

Why 1.33: This is the KMDF version for Windows 10 21H2+ / Windows 11 (WDK 10.0.22621+). It matches the minimum OS version declared in the INF (`NTamd64.10.0...16299`). If a specific WDK version needs a different number, the builder can change this one line.

Add a comment above it:
```ini
; KMDF 1.33 = WDK 10.0.22621+ (Win10 21H2+ / Win11)
; Adjust if targeting a different WDK version.
KmdfLibraryVersion = 1.33
```

**Validation:**
- [ ] The string `$KMDFVERSION$` no longer appears in the file
- [ ] The replacement is a concrete version number
- [ ] A comment explains what version to use

**After completing Phase 1, read this document again to find Phase 2.**

---

## Phase 2: Add Hash Table Tombstone Reclamation (Audit #13)

**Goal:** Add periodic tombstone cleanup to the open-addressed hash table so it doesn't degrade under high-churn workloads.

**File to modify:** `windows-port/shim/greenboost_cuda_shim_win.h` (add counter + threshold constant)

**Changes to the header (greenboost_cuda_shim_win.h):**

After the `HT_LOCKS` define, add:
```c
#define HT_TOMBSTONE_THRESHOLD  (HT_SIZE / 4)  /* Reclaim when 25% tombstones */
```

In the globals section (near `extern gb_ht_entry_t gb_htable[HT_SIZE]`), add:
```c
extern volatile LONG gb_tombstone_count;
```

**File to modify:** `windows-port/shim/greenboost_cuda_shim_win.c`

**DO NOT touch the hook functions, init/cleanup, or any function added in the previous fix batch.** Only modify the hash table functions.

1. After the `CRITICAL_SECTION ht_locks[HT_LOCKS];` global declaration, add:
```c
volatile LONG gb_tombstone_count = 0;
```

2. In `ht_insert()`, after the line `e->ptr = ptr;` (the successful insert into a tombstone slot), add tombstone count decrement:
```c
        EnterCriticalSection(lk);
        if (e->ptr == HT_EMPTY || e->ptr == HT_TOMBSTONE) {
            if (e->ptr == HT_TOMBSTONE)
                InterlockedDecrement(&gb_tombstone_count);
            e->ptr            = ptr;
```

3. In `ht_remove()`, after `e->ptr = HT_TOMBSTONE;`, add:
```c
            e->ptr            = HT_TOMBSTONE;
            InterlockedIncrement(&gb_tombstone_count);
```

4. Add a new function `ht_reclaim_tombstones()` right after `ht_lookup()`:
```c
/*
 * Reclaim tombstone slots by compacting probe chains.
 * Called periodically when tombstone count exceeds threshold.
 * Must NOT be called while any other ht operation is in progress
 * on the same slot range -- we acquire ALL locks sequentially.
 */
static void ht_reclaim_tombstones(void)
{
    uint32_t i;
    LONG reclaimed = 0;

    /* Acquire all locks to ensure exclusive access */
    for (i = 0; i < HT_LOCKS; i++)
        EnterCriticalSection(&ht_locks[i]);

    for (i = 0; i < HT_SIZE; i++) {
        if (gb_htable[i].ptr == HT_TOMBSTONE) {
            gb_htable[i].ptr = HT_EMPTY;
            reclaimed++;
        }
    }

    /*
     * After clearing tombstones to EMPTY, some entries that were
     * inserted past a tombstone may now be unreachable. Re-insert
     * all live entries to fix probe chains.
     */
    for (i = 0; i < HT_SIZE; i++) {
        if (gb_htable[i].ptr != HT_EMPTY && gb_htable[i].ptr != HT_TOMBSTONE) {
            gb_ht_entry_t saved = gb_htable[i];
            gb_htable[i].ptr = HT_EMPTY;

            /* Re-insert at correct position */
            uint32_t h = ht_hash(saved.ptr);
            uint32_t j;
            for (j = 0; j < HT_SIZE; j++) {
                uint32_t idx = (h + j) & HT_MASK;
                if (gb_htable[idx].ptr == HT_EMPTY) {
                    gb_htable[idx] = saved;
                    break;
                }
            }
        }
    }

    InterlockedExchange(&gb_tombstone_count, 0);

    for (i = HT_LOCKS; i > 0; i--)
        LeaveCriticalSection(&ht_locks[i - 1]);

    gb_log("hash table reclaimed %ld tombstones", reclaimed);
}
```

5. In `ht_remove()`, after the successful removal return, add a check:
```c
            LeaveCriticalSection(lk);
            /* Trigger reclamation if tombstones are accumulating */
            if (gb_tombstone_count > HT_TOMBSTONE_THRESHOLD)
                ht_reclaim_tombstones();
            return 1;
```
Make sure to remove the existing `return 1;` that was on the line after `LeaveCriticalSection(lk);` so there's only one return.

**Validation:**
- [ ] `gb_tombstone_count` is declared in .h and defined in .c
- [ ] `HT_TOMBSTONE_THRESHOLD` is defined
- [ ] `ht_insert` decrements tombstone count when reusing a tombstone slot
- [ ] `ht_remove` increments tombstone count when creating a tombstone
- [ ] `ht_reclaim_tombstones` exists and is called when threshold exceeded
- [ ] No hook functions or init/cleanup functions were modified

**After completing Phase 2, read this document again to find Phase 3.**

---

## Phase 3: Translate sign.ps1 to English (Audit #16)

**Goal:** Replace all Chinese strings in sign.ps1 with English equivalents. Do NOT change functionality, logic, parameters, or certificate handling.

**File to modify:** `windows-port/sign.ps1`

**Translation table:**

| Chinese | English |
|---------|---------|
| 未找到 Windows SDK 路径 | Windows SDK path not found |
| 找到 signtool | Found signtool |
| 未在任何 SDK 版本中找到 signtool.exe | signtool.exe not found in any SDK version |
| 证书不存在，开始初始化... | Certificate not found, initializing... |
| 证书已生成，指纹 | Certificate generated, thumbprint |
| 证书已导出 | Certificate exported |
| PFX 已导出 | PFX exported |
| 证书已添加到受信任根证书颁发机构 | Certificate added to Trusted Root CA |
| 证书已添加到受信任发布者 | Certificate added to Trusted Publishers |
| 开始签名驱动... | Signing driver... |
| 签名失败，退出码 | Signing failed, exit code |
| 签名完成 | Signing complete |
| 步骤 | Step |
| 查找 signtool | Finding signtool |
| 检测证书 | Checking certificate |
| 签名驱动 | Signing driver |
| 证书不存在，需要初始化 | Certificate not found, initialization required |
| 证书已存在 | Certificate already exists |
| 驱动文件不存在 | Driver file not found |
| 驱动路径 | Driver path |
| 所有步骤完成！ | All steps complete! |
| 错误 | Error |
| GreenBoost 驱动签名工具 | GreenBoost Driver Signing Tool |
| GreenBoost 驱动签名脚本（优化版） | GreenBoost driver signing script |
| 功能：证书存在检测 + 自动查找最新 signtool + 签名驱动 | Functions: Certificate detection + auto-find latest signtool + sign driver |
| 要求：以管理员身份运行 PowerShell | Requires: Run PowerShell as Administrator |

Replace every Chinese string with its English equivalent. Keep all PowerShell logic, variable names, and control flow identical.

Also change the default password from the hardcoded value to a parameter with a note:
```powershell
[string]$Password = "GreenBoost123"  # Development only. Use a real cert for production.
```
(This is already the default -- just add the comment if not present.)

**Validation:**
- [ ] No Chinese characters remain in sign.ps1
- [ ] Script structure and logic unchanged
- [ ] All Write-Host messages are in English
- [ ] All comments are in English
- [ ] The BOM (byte order mark) at start of file is preserved if present

**After completing Phase 3, read this document again to find Phase 4.**

---

## Phase 4: Wire test_uvm.c Into CMake (Audit #19)

**Goal:** Add a conditional build target for test_uvm.c that compiles when the CUDA SDK is available.

**File to modify:** `windows-port/tests/CMakeLists.txt`

**Append the following after the existing `test_ioctl` target:**

```cmake
# UVM test -- requires CUDA SDK (optional)
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    message(STATUS "CUDA SDK found -- building test_uvm")
    add_executable(test_uvm test_uvm.c)
    target_include_directories(test_uvm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../driver
    )
    target_compile_definitions(test_uvm PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(test_uvm PRIVATE CUDA::cudart)
else()
    message(STATUS "CUDA SDK not found -- skipping test_uvm")
endif()
```

**Also in test_uvm.c** -- the include uses `<cuda_runtime.h>` which is correct. No changes needed to the .c file.

**Validation:**
- [ ] `CMakeLists.txt` has a `find_package(CUDAToolkit QUIET)` call
- [ ] `test_uvm` target is only created when CUDA is found
- [ ] `test_uvm` links against `CUDA::cudart`
- [ ] Builds without CUDA SDK installed (graceful skip)
- [ ] The existing `test_ioctl` target is untouched

---

## Phase 5: Reconciliation Review

Before declaring the build complete, verify what you built:

### Code Consistency Check
1. Read `.nsca/repo_profile.md` if available
2. For every file you created or modified:
   - Are variable names consistent with the naming conventions in related files?
   - Do imports resolve to actual modules in the repo?
   - If you used a function from an existing file, does your usage match its actual signature?

### Protected Zone Verification
3. Confirm you did NOT modify any of these files:
   - `windows-port/driver/greenboost_win.c`
   - `windows-port/driver/greenboost_win.h`
   - `windows-port/shim/greenboost_cuda_shim_win.c`
   - `windows-port/tools/install.ps1`
   - `windows-port/tools/config.ps1`
   - `windows-port/tools/diagnose.ps1`
   - `windows-port/build.ps1`

### Blast Radius Check
4. The hash table changes (Phase 2) affect alloc/free paths in the shim. Verify:
   - `ht_insert` still returns 1 on success, 0 on failure
   - `ht_remove` still returns 1 if found, 0 if not
   - `ht_lookup` is untouched
   - Lock acquisition order is always low-index to high-index (no deadlocks)

### Reconciliation Verdict
If ANY issue is found:
  - Fix it before proceeding
  - Document what was wrong and how you fixed it in the commit message

If all checks pass:
  - Commit with message: `fix: remaining audit items #11,#13,#16,#19 -- INF version, HT reclaim, sign.ps1 English, test_uvm CMake`

---

## SUCCESS CRITERIA

- [ ] `$KMDFVERSION$` placeholder replaced with concrete version in INF
- [ ] Hash table has tombstone reclamation with threshold trigger
- [ ] sign.ps1 is fully English with no Chinese characters
- [ ] test_uvm.c builds when CUDA SDK is present, skips gracefully when absent
- [ ] No protected files were modified
- [ ] All changes committed to main
