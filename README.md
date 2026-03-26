![GreenBoost Windows](assets/header.png)

# GreenBoost Windows Port

**Original work by [Ferran Duarri](https://gitlab.com/IsolatedOctopi/nvidia_greenboost) (GPL v2)**

Windows port by [Chris Zuger](https://github.com/denoflore)

---

## The story

I was scrolling Reddit and saw Ferran Duarri's GreenBoost drop on r/LocalLLaMA. A Linux kernel module that transparently extends your GPU's VRAM with system RAM and NVMe so you can run LLMs that don't fit in your card. No code changes to your inference engine, no manual layer offloading, just load the module and your 12GB card suddenly sees 60+ GB of addressable memory. Clever as hell.

My first thought was "that's sick." My second thought was "but I'm on Windows."

I run a multi-GPU homelab with LM Studio on Windows. I've got the VRAM to handle most things, but there's always a bigger model. And plenty of people in the local LLM community are on Windows with a single 12GB or 16GB card who could genuinely use this.

So I figured: why not port it? The original is open source, GPL v2, well-documented. Ferran even published a full architecture doc explaining exactly how the kernel module and CUDA shim work together. The core insight that makes the port possible is that the CUDA memory registration path (`cuMemHostRegister` + `cuMemHostGetDevicePointer`) is identical on both platforms. The only difference is how you get the memory mapped into userspace in the first place.

On Linux: `alloc_pages` -> DMA-BUF fd -> `mmap` -> `cuMemHostRegister`

On Windows: `MmAllocateContiguousMemorySpecifyCache` -> MDL -> `MmMapLockedPagesSpecifyCache` -> `cuMemHostRegister`

Same CUDA calls at the end. Everything else is plumbing.

This is my first actually useful open source contribution. I've got research repos but this is the first piece of practical tooling I've put out there that other people might genuinely benefit from. No deep reason for doing it beyond "someone built something cool for Linux and Windows users deserve it too."

---

## What's in this fork

Everything from Ferran's original repo (untouched, his README preserved as `README_original_ferran.md`) plus a complete `windows-port/` directory:

| Component | File | Lines | What it does |
|-----------|------|-------|-------------|
| KMDF Driver | `driver/greenboost_win.c` | 1,424 | Windows kernel driver replacing `greenboost.ko`. Allocates pinned 2MB contiguous memory blocks, maps them directly into userspace via MDL, monitors memory pressure, manages buffer lifecycle. |
| Driver Header | `driver/greenboost_win.h` | 193 | Per-buffer and global device state structs. |
| IOCTL Header | `driver/greenboost_ioctl_win.h` | 165 | Windows IOCTL definitions using CTL_CODE. Shared between driver and shim. |
| CUDA Shim | `shim/greenboost_cuda_shim_win.c` | 1,030 | DLL that hooks cudaMalloc/cudaFree via Microsoft Detours. Routes large allocations to the driver, spoofs VRAM reporting so LM Studio/Ollama see the extended memory. |
| Shim Header | `shim/greenboost_cuda_shim_win.h` | 155 | Hash table, config struct, CUDA type definitions. |
| Driver INF | `driver/greenboost_win.inf` | 84 | Standard KMDF driver installation manifest. |
| Installer | `tools/install.ps1` | 300 | PowerShell script that detects your hardware, computes optimal config, writes registry keys, installs the driver. |
| Diagnostics | `tools/diagnose.ps1` | 241 | Health check script. |
| IOCTL Tests | `tests/test_ioctl.c` | 442 | Smoke tests and stress tests for the driver interface. |
| Build System | `CMakeLists.txt` (x3) | 106 | CMake configs for driver, shim, and tests. |
| Docs | `BUILDING.md`, `TROUBLESHOOTING.md` | 240 | Build from source instructions and common fixes. |

Total: ~4,500 lines of new code across 17 files.

---

## What changed from the Linux version

The full architecture mapping is documented in `windows-port/CC_INSTRUCTIONS.md`, but here's the summary:

**Memory allocation:** Linux uses `alloc_pages` with compound pages (order 9 = 2MB). Windows uses `MmAllocateContiguousMemorySpecifyCache` for the same 2MB contiguous blocks, with an MDL fallback for when contiguous memory isn't available.

**Sharing memory with userspace:** This was the trickiest part and where the first implementation had a critical bug. The original attempt used `ZwCreateSection` to create an NT section object, but `ZwCreateSection` with a NULL file handle creates anonymous pagefile-backed memory -- completely separate from the pinned physical pages we allocated. The shim would have gotten the wrong memory entirely. The fix uses `MmMapLockedPagesSpecifyCache(UserMode)` which maps the actual physical pages described by the MDL directly into the calling process. This is the true Windows equivalent of Linux `mmap(dma_buf_fd)`.

**Buffer lifecycle:** Linux relies on `close(fd)` triggering the DMA-BUF release callback for automatic cleanup. Windows has no equivalent for MDL user mappings, so we added an explicit `GB_IOCTL_FREE` that the shim calls on `cudaFree`. The driver tracks the owning process so it can do cross-process cleanup via `KeStackAttachProcess` if needed during driver unload.

**CUDA hook injection:** Linux uses `LD_PRELOAD` + a `dlsym` intercept (because Ollama resolves symbols via `dlopen` internally). Windows uses Microsoft Detours (MIT licensed) for API hooking, with an IAT patching fallback.

**Hash table bug fix:** The original Linux `ht_remove` zeroes deleted slots with `memset(e, 0, sizeof(*e))`, which breaks open-addressing probe chains. A lookup for a key that hashed past the deleted slot would stop early at the zeroed slot and miss the target. The Windows port uses tombstone markers instead, which preserves probe chain integrity. This is a bug in the upstream Linux code too.

**Watchdog:** Linux `kthread` becomes `PsCreateSystemThread`. Linux `eventfd` becomes a named kernel event. Memory pressure queries use `ZwQuerySystemInformation` instead of `/proc/meminfo`.

---

## Status

**Work in progress.** The code is structurally complete and has been reviewed, but hasn't been compiled against WDK or tested on real hardware yet. It needs:

- A build with Visual Studio 2022 + WDK to shake out any compile errors
- Test signing enabled and a test load on a real Windows machine
- Actual end-to-end testing with LM Studio loading a model larger than physical VRAM
- Driver Verifier pass to catch any kernel memory handling issues

If you know Windows kernel driver development and want to help get this across the finish line, PRs are very welcome.

---

## Building

Prerequisites: Visual Studio 2022, Windows Driver Kit (WDK), CMake 3.20+, Microsoft Detours (NuGet).

See `windows-port/BUILDING.md` for full instructions.

---

## The original

All credit to **Ferran Duarri** for the original GreenBoost architecture and implementation. The Linux source in this repo is unmodified from the [upstream GitLab repository](https://gitlab.com/IsolatedOctopi/nvidia_greenboost). He did the hard work of figuring out the DMA-BUF + CUDA external memory integration, the 3-tier memory hierarchy, the Ollama-specific dlsym hooks, and all the system tuning. This port just translates his design to Windows APIs.

Thanks Ferran. Hope this is useful to the Windows side of the community.

---

## License

GPL v2, matching upstream. Attribution to Ferran Duarri required per license terms.

```
Original work: Copyright (C) 2024-2026 Ferran Duarri
Windows port: Copyright (C) 2026 Chris Zuger
SPDX-License-Identifier: GPL-2.0-only
```

