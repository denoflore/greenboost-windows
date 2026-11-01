// SPDX-License-Identifier: GPL-2.0-only
// Included inside the pinned llama.cpp CUDA backend. File I/O metadata only
// crosses to the CPU; routing membership is produced from CUDA router IDs.
#include "file_tensor.h"
#include <io.h>
#include <windows.h>
#include <memory>
#include <map>

static size_t gb_env_mib(const char *name, size_t fallback) {
    const char *value=std::getenv(name);
    if(!value) return fallback<<20;
    char *end=nullptr;
    unsigned long long n=std::strtoull(value,&end,10);
    if(!*value || *end || *value=='-' || n>SIZE_MAX/(1ULL<<20))
        GGML_ABORT("GreenBoost: invalid MiB value for %s",name);
    return (size_t)n<<20;
}
static bool gb_paging_enabled() {return gb_env_mib("GREENBOOST_EXPERT_VRAM_MB",0)!=0;}
static void gb_driver(CUresult r,const char *op) {
    if(r!=CUDA_SUCCESS) GGML_ABORT("GreenBoost: %s failed, CUDA %d",op,(int)r);
}
struct gb_paged_file {
    greenboost::FileTensor file;
    unsigned *requests=nullptr,*host=nullptr;
    uint64_t operations=0;
    gb_paged_file(const wchar_t *path):file(path,gb_env_mib("GREENBOOST_EXPERT_VRAM_MB",0),
            2ULL<<20,false,gb_env_mib("GREENBOOST_EXPERT_RAM_MB",0)) {
        CUDA_CHECK(cudaMalloc((void**)&requests,4097*sizeof(unsigned)));
        CUDA_CHECK(cudaMallocHost((void**)&host,4097*sizeof(unsigned)));
        CUdevice device;char pci[32];
        gb_driver(cuCtxGetDevice(&device),"residency device");
        gb_driver(cuDeviceGetPCIBusId(pci,sizeof(pci),device),"residency PCI identity");
        std::fprintf(stderr,"GreenBoost pid=%lu CUDA=%d PCI=%s expert_pool=%zu host_cache=%zu chunk=%zu\n",
            GetCurrentProcessId(),device,pci,file.stats().resident_limit,file.stats().host_cache_bytes,file.stats().chunk_bytes);
    }
    ~gb_paged_file() {
        const auto s=file.stats();
        std::fprintf(stderr,"GreenBoost residency: operations=%llu pool=%zu host_cache=%zu file_read=%llu uploaded=%llu gpu_hits=%llu host_hits=%llu misses=%llu evictions=%llu\n",
            (unsigned long long)operations,s.resident_limit,s.host_cache_bytes,
            (unsigned long long)s.file_bytes,(unsigned long long)s.uploaded_bytes,
            (unsigned long long)s.hits,(unsigned long long)s.host_hits,
            (unsigned long long)s.misses,(unsigned long long)s.evictions);
        CUDA_CHECK(cudaFree(requests));CUDA_CHECK(cudaFreeHost(host));
    }
};
