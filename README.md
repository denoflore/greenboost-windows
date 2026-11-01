![GreenBoost Windows](assets/header.png)

# GreenBoost for Windows

[![Windows portable build](https://github.com/denoflore/greenboost-windows/actions/workflows/windows-build.yml/badge.svg?branch=main)](https://github.com/denoflore/greenboost-windows/actions/workflows/windows-build.yml)

**Use NVIDIA GPU memory more flexibly on Windows.**

GreenBoost launches a CUDA application with a small library that changes how
it requests memory. The current source uses the card's own memory (VRAM)
first. If an eligible large allocation runs out of VRAM, it asks NVIDIA's driver
for managed memory, whose location the driver controls. CUDA calculations
continue to run on the GPU.

For local AI, the included scripts install a known version of llama.cpp and
run a model you supply. You can choose a GPU, set a memory allowance and reduce
the memory used to remember previous tokens.

**The larger goal is one logical memory space spanning VRAM, RAM and files.
The experimental file library now has an opt-in llama.cpp adapter that loads
selected MoE experts into a bounded VRAM pool.** It requires a separate source
build. [Offloading setup, memory limits and launch instructions](windows-port/integrations/llama/README.md).

## Choose a version

| Version | How to get it | What you get |
| --- | --- | --- |
| **2.5.0 experimental** on `main` | [Build from source](windows-port/BUILDING.md) | VRAM first allocation, cache controls, GPU inspection and an optional file library with a separate MoE inference backend. |
| **2.4.0 beta 3**, latest published binary | [Download the Windows ZIP](https://github.com/denoflore/greenboost-windows/releases/download/v2.4.0-beta.3/GreenBoost-Windows-2.4.0-beta.3-x64.zip) | Portable launcher and llama.cpp scripts. It uses the earlier allocation policy and lacks the 2.5 features above. |

There is **no published 2.5 binary yet**. Downloading beta 3 does not install the
new code on `main`.

To build 2.5, install Visual Studio C++ Build Tools, CMake and build Microsoft
Detours 4.0.1 as described in the [build guide](windows-port/BUILDING.md).
From the GreenBoost repository:

```powershell
.\windows-port\build.ps1 -DetoursRoot C:\src\Detours -StaticRuntime -Package
```

Extract `windows-port/tools/outputs/GreenBoost-Windows-2.5.0-experimental-x64.zip`,
then follow the same steps below.

## Run your first model

You need Windows 10 or 11 x64, an NVIDIA CUDA GPU with a compatible driver,
and a model in GGUF format. Running the portable package requires no
administrator rights, GreenBoost kernel driver or CUDA Toolkit.

1. Extract your GreenBoost ZIP and open PowerShell in that folder. Keep
   `greenboost.exe` and `greenboost_cuda.dll` together.
2. Install the llama.cpp backend for your GPU. This downloads the inference
   program and its CUDA libraries into a local `backends` folder.
3. List your GPUs, then run your own GGUF file using its GPU number.

For GeForce RTX 3000, 4000 and 5000 cards, with a compatible driver:

```powershell
.\install_llama.ps1 -Cuda 13.4
.\run_llama.ps1 -ListDevices
.\run_llama.ps1 -Model 'D:\models\model.gguf' -Gpu 0 -Prompt 'Explain why the sky is blue.'
```

Replace the model path and GPU number. Use the number printed by
`-ListDevices`; `nvidia-smi` may number the cards differently. The helper runs
one prompt and exits. **It does not download a model.**

The helper keeps model tensors and supported token sampling on the selected
GPU. It uses one GPU per run and defaults to a 4,096 token context and a
4,096 MiB additional capacity allowance. An allowance is a value reported to
the application; it does not reserve RAM or guarantee that a larger model fits.

For GeForce RTX 2000, or the older CUDA runtime option on RTX 3000/4000:

```powershell
.\install_llama.ps1 -Cuda 12.4
.\run_llama.ps1 -LlamaDirectory .\backends\llama-b12019-cuda12.4 -ListDevices
.\run_llama.ps1 -LlamaDirectory .\backends\llama-b12019-cuda12.4 -Model 'D:\models\model.gguf' -Gpu 0
```

These commands work with both versions above. RTX 2000 code is included in the
backend, but execution on an RTX 2000 card remains untested. The CUDA 12.4 option
does not establish support for every older NVIDIA driver. Use CUDA 13.4 for RTX
5000. See [GPU architecture and driver details](windows-port/RTX_KERNELS.md).

## Use the new memory controls in 2.5

By default, **adaptive allocation** keeps an allocation in ordinary VRAM when
it fits and tries managed memory only after an out of memory result. The older
`managed` policy tries managed memory first for eligible allocations.

The **KV cache** holds information about previous tokens. Reducing its precision
can free memory for longer conversations:

```powershell
.\run_llama.ps1 -Model 'D:\models\model.gguf' -Gpu 0 -CacheType q8_0 -Context 32768
```

`f16` is the default. `q8_0` and `q4_0` use less cache memory and can change
model output. The model must support the requested context, and the complete
workload must still fit the memory available to CUDA.

Use `-ExtraMiB 0` to disable the extra capacity allowance, or
`-AllocationPolicy managed` to select the older allocation behaviour.
`greenboost-info.exe` shows your GPUs and their memory capabilities.
The [portable guide](windows-port/README.md) lists all allocation settings and
verification commands.

## Offload model experts when needed

Use normal loading when your model and context fit on the card. The separate
experimental MoE backend can keep expert weights in a GGUF file and bring
the sections it needs through RAM into a limited GPU memory pool. Other
weights, context memory and calculations remain on the GPU.

**You select this mode before launching. The ordinary helper does not switch
to it automatically when VRAM fills up.** It applies to supported quantised
MoE models and requires the separate source build. Dense models and existing
Ollama or LM Studio installations do not gain expert paging from the shim.

After building that backend, run from the repository root:

```powershell
.\windows-port\tools\run_moe.ps1 `
    -LlamaDirectory '.\windows-port\tools\backends\greenboost-moe-b12019\build\bin' `
    -Model 'D:\models\your-moe-model.gguf' -Gpu 0 `
    -ExpertVramMiB 1024 -ExpertRamMiB 4096 `
    -Context 2048 -MicroBatch 8 -Tokens 128
```

These example settings give experts a 1 GiB VRAM pool and 4 GiB RAM cache.
They do not cap total process memory or guarantee that a particular model
fits. Moving weights repeatedly can slow generation considerably.

The [full offloading guide](windows-port/integrations/llama/README.md) covers
prerequisites, building, choosing the GPU, sizing both budgets, recognising
active paging, resolving memory errors and returning to normal loading.

## What "one memory" means here

A program can keep one address for a piece of data while software controls
where that data is held. **VRAM, system RAM and disk still have different
capacities and access speeds.**

GreenBoost has two mechanisms:

| Mechanism | How it uses memory | Status |
| --- | --- | --- |
| **Allocation library** used by the launcher | Intercepts selected CUDA allocation and capacity calls. NVIDIA controls the placement of managed memory. | Available now. Success and speed depend on the application, GPU, driver and Windows memory limits. |
| **File library** called `FileTensor` | Gives a file one CUDA address range. It reads requested sections through RAM into a limited VRAM pool and reuses that VRAM for later sections. Each section keeps its address when reloaded. | Experimental developer API in 2.5. The application must request a section before the GPU uses it. |

The optional llama.cpp adapter requests the actual routed expert weights
before their CUDA kernels run. Its VRAM pool and pinned RAM cache have explicit
budgets. The initial small-model check produced the resident run's text while
using less VRAM, with slower inference. Launching an ordinary backend, Ollama
or LM Studio through the allocation shim does not enable this adapter.

The current file path uses the Windows file cache and RAM staging buffers.
It does not implement direct SSD to GPU transfers. Optional NVIDIA hardware
memory compression is lossless; it does not enlarge the allocated VRAM pool
or compress the model file. See the [API and worked example](windows-port/EXPERIMENTAL.md).

## What has been tested

Recorded on **October 31, 2026**, on Windows 11 with NVIDIA driver 610.88:

| Check | Recorded result | What that establishes |
| --- | --- | --- |
| CUDA allocation compatibility | 45 cases across CUDA 11.8 through 13.4, primarily on RTX 3060, plus packaged CUDA 13.4 on RTX 5090. | The tested allocation, GPU write, readback and free paths worked with those runtimes. |
| Allocation policies and file library | Passed on RTX 3060, 3090, 4090, 5060 Ti and 5090. | Those bounded tests worked on five cards. |
| File larger than its VRAM pool | A 256 MiB file plus a partial final chunk used a 32 MiB VRAM pool, with zero GPU verification mismatches. | Explicit loading, eviction and reloading worked in that storage fixture. |
| Smaller KV cache | For one Qwen3.5 4B model at a 32,768 token context: 1,024 MiB for f16, 544 MiB for q8_0, 288 MiB for q4_0. | Memory savings and a short completion were observed. Long context accuracy and broader model quality remain unmeasured. |

The allocation change also recovered a slow configuration: with five GPUs
visible, a Qwen3.5 4B Q6_K comparison on RTX 3060 went from about 1.4 to 69
generated tokens/s. Each run used 128 prompt tokens, 8 generated tokens and
two repetitions. The model already fit in VRAM. With only one GPU visible,
both policies performed similarly. This result applies to that configuration;
inference beyond VRAM capacity was not measured by this comparison.

Separate [November 1 model receipts](windows-port/integrations/llama/README.md#evidence)
cover the new MoE backend. Two prompts matched resident output using a small
expert pool on an RTX 3060. Paging was slower for that model, which already
fits on the card; throughput for models larger than VRAM remains unqualified.

The [full report](windows-port/EXPERIMENTAL.md) records exact workloads,
processes, device placement, utilisation and limitations. GitHub Actions checks
the Windows build and package without a GPU. RTX 2000 hardware, older drivers,
long production runs, Ollama and LM Studio integration remain unverified.
AMD and Intel GPUs are outside this CUDA port.

## Use another CUDA application

```powershell
.\greenboost.exe 'C:\application\app.exe' arguments
```

GreenBoost starts a fresh x64 process, loads its allocation library, follows
supported x64 child launches and returns the application's exit code.
An already running server is unaffected. Each application keeps its own CUDA
runtime and inference kernels.

The supplied AI backend is pinned to **official llama.cpp b12019**. Its recent
CUDA kernels come from llama.cpp; GreenBoost supplies memory handling and launch
controls. [Backend changes and attribution](windows-port/RTX_KERNELS.md).

## Documentation and credits

- [Portable usage and settings](windows-port/README.md)
- [Configure MoE offloading and memory budgets](windows-port/integrations/llama/README.md)
- [Build instructions](windows-port/BUILDING.md)
- [Troubleshooting](windows-port/TROUBLESHOOTING.md)
- [Experimental design, public research and measurements](windows-port/EXPERIMENTAL.md)
- [Earlier release validation](windows-port/VALIDATION.md) and [compatibility](windows-port/COMPATIBILITY.md)

Original GreenBoost by [Ferran Duarri](https://gitlab.com/IsolatedOctopi/greenboost).
Windows port and maintenance by [Chris Zuger / denoflore](https://github.com/denoflore).
Linux sources are retained from upstream. [GPL v2](LICENSE);
Microsoft Detours is MIT licensed.
