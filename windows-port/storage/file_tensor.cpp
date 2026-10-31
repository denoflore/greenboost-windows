/* SPDX-License-Identifier: GPL-2.0-only
 * Public CUDA VMM plus bounded Windows file I/O. No CPU tensor computation.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "file_tensor.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>

namespace greenboost {
namespace {
void check(CUresult r, const char *op) {
    if (r == CUDA_SUCCESS) return;
    const char *name = nullptr;
    cuGetErrorName(r, &name);
    throw std::runtime_error(std::string(op) + ": " + (name ? name : "CUDA error") +
                             " (" + std::to_string((int)r) + ")");
}
void wincheck(bool ok, const char *op) {
    if (!ok) throw std::runtime_error(std::string(op) + ": Windows " + std::to_string(GetLastError()));
}
void cleanup(CUresult r, const char *op) noexcept {
    if (r != CUDA_SUCCESS) std::fprintf(stderr,"GreenBoost storage cleanup: %s CUDA %d\n",op,(int)r);
}
}
struct FileTensor::Impl {
    struct Slot { CUmemGenericAllocationHandle handle=0; uint64_t page=UINT64_MAX; bool mapped=false; };
    struct Lane { void *host=nullptr; CUevent done=nullptr; };
    HANDLE file=INVALID_HANDLE_VALUE;
    CUcontext context=nullptr;
    CUdevice device=0;
    DWORD owner=GetCurrentThreadId();
    CUdeviceptr address=0;
    CUstream queue=nullptr;
    size_t reserved=0, chunk=0, victim=0, lane_index=0;
    uint64_t length=0;
    bool poisoned=false;
    Stats counters;
    std::vector<Slot> slots;
    Lane lanes[2];

    ~Impl() {
        CUcontext popped=nullptr;
        const bool pushed=context && cuCtxPushCurrent(context)==CUDA_SUCCESS;
        if (pushed) {
            if (queue) cleanup(cuStreamSynchronize(queue),"synchronize");
            for(auto &s:slots) {
                if(s.mapped) cleanup(cuMemUnmap(address+s.page*chunk,chunk),"unmap");
                if(s.handle) cleanup(cuMemRelease(s.handle),"release");
            }
            if(address) cleanup(cuMemAddressFree(address,reserved),"address free");
            for(auto &l:lanes) {
                if(l.done) cleanup(cuEventDestroy(l.done),"event destroy");
                if(l.host) cleanup(cuMemFreeHost(l.host),"host free");
            }
            if(queue) cleanup(cuStreamDestroy(queue),"stream destroy");
            cleanup(cuCtxPopCurrent(&popped),"context pop");
        } else if(context) {
            std::fputs("GreenBoost storage: owning CUDA context unavailable during cleanup\n",stderr);
        }
        if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    void ensure_owner() {
        CUcontext current=nullptr;
        check(cuCtxGetCurrent(&current),"current context");
        if(owner!=GetCurrentThreadId() || current!=context)
            throw std::runtime_error("FileTensor requires its owner thread and original CUDA context");
        if(poisoned) throw std::runtime_error("FileTensor is unusable after an I/O or CUDA failure");
    }
    Slot *find(uint64_t page) {
        for(auto &s:slots) if(s.mapped && s.page==page) return &s;
        return nullptr;
    }
    void access(uint64_t page, CUmemAccess_flags flags) {
        CUmemAccessDesc desc={};
        desc.location.type=CU_MEM_LOCATION_TYPE_DEVICE;
        desc.location.id=device;
        desc.flags=flags;
        check(cuMemSetAccess(address+page*chunk,chunk,&desc,1),"set access");
    }
};

FileTensor::FileTensor(const wchar_t *path,size_t budget,size_t chunk,bool compression)
    : impl_(std::make_unique<Impl>()) {
    auto &p=*impl_;
    check(cuCtxGetCurrent(&p.context),"current context");
    if(!p.context) throw std::runtime_error("Create a CUDA context before FileTensor");
    check(cuCtxGetDevice(&p.device),"context device");
    int supported=0;
    check(cuDeviceGetAttribute(&supported,CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED,p.device),"VMM capability");
    if(!supported) throw std::runtime_error("CUDA virtual memory management is unavailable on this device");
    if(compression) {
        check(cuDeviceGetAttribute(&supported,CU_DEVICE_ATTRIBUTE_GENERIC_COMPRESSION_SUPPORTED,p.device),"compression capability");
        if(!supported) throw std::runtime_error("Hardware memory compression is unavailable on this device");
    }
    p.file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    wincheck(p.file!=INVALID_HANDLE_VALUE,"open tensor file");
    LARGE_INTEGER length;
    wincheck(GetFileSizeEx(p.file,&length)!=0,"file size");
    if(length.QuadPart<=0) throw std::runtime_error("Tensor file is empty");
    p.length=(uint64_t)length.QuadPart;
    CUmemAllocationProp prop={};
    prop.type=CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type=CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id=p.device;
    if(compression) prop.allocFlags.compressionType=CU_MEM_ALLOCATION_COMP_GENERIC;
    size_t granularity=0;
    check(cuMemGetAllocationGranularity(&granularity,&prop,CU_MEM_ALLOC_GRANULARITY_MINIMUM),"allocation granularity");
    if(!chunk || chunk%granularity || chunk>MAXDWORD || budget<chunk)
        throw std::runtime_error("Chunk must fit DWORD, align to CUDA granularity, and fit the VRAM budget");
    if(p.length>SIZE_MAX-(chunk-1)) throw std::runtime_error("Tensor address range is too large");
    p.chunk=chunk;
    p.reserved=((size_t)p.length+chunk-1)/chunk*chunk;
    const size_t count=std::min(budget/chunk,p.reserved/chunk);
    p.slots.resize(count);
    check(cuMemAddressReserve(&p.address,p.reserved,granularity,0,0),"reserve virtual address");
    check(cuStreamCreate(&p.queue,CU_STREAM_NON_BLOCKING),"create transfer/compute stream");
    for(auto &s:p.slots) {
        check(cuMemCreate(&s.handle,chunk,&prop,0),"create physical chunk");
        if(compression) {
            CUmemAllocationProp actual={};
            check(cuMemGetAllocationPropertiesFromHandle(&actual,s.handle),"actual allocation properties");
            if(actual.allocFlags.compressionType!=CU_MEM_ALLOCATION_COMP_GENERIC)
                throw std::runtime_error("CUDA did not grant requested hardware compression");
        }
    }
    for(auto &l:p.lanes) {
        check(cuMemHostAlloc(&l.host,chunk,0),"allocate pinned staging");
        check(cuEventCreate(&l.done,CU_EVENT_DISABLE_TIMING),"create staging fence");
        check(cuEventRecord(l.done,p.queue),"initialize staging fence");
    }
    p.counters.resident_limit=count*chunk;
    p.counters.staging_bytes=2*chunk;
    p.counters.chunk_bytes=chunk;
    p.counters.compressed=compression;
}
FileTensor::~FileTensor()=default;
CUdeviceptr FileTensor::base() const {return impl_->address;}
uint64_t FileTensor::size() const {return impl_->length;}
CUstream FileTensor::stream() const {return impl_->queue;}
FileTensor::Stats FileTensor::stats() const {return impl_->counters;}

CUdeviceptr FileTensor::acquire(uint64_t offset,size_t bytes) {
    auto &p=*impl_;
    p.ensure_owner();
    if(!bytes || offset>=p.length || bytes>p.length-offset)
        throw std::out_of_range("Requested span is outside the tensor file");
    const uint64_t first=offset/p.chunk,last=(offset+bytes-1)/p.chunk;
    if(last-first+1>p.slots.size())
        throw std::out_of_range("Requested span exceeds the resident VRAM budget");
    bool missing=false;
    for(uint64_t page=first;page<=last;page++) if(!p.find(page)) missing=true;
    // A cache hit requires no host/device barrier. Misses may reuse a mapping,
    // so all earlier consumers on the owned stream must finish first.
    if(!missing) {
        p.counters.hits+=last-first+1;
        return p.address+offset;
    }
    try {
        check(cuStreamSynchronize(p.queue),"finish previous consumers");
        std::vector<uint64_t> loaded;
        for(uint64_t page=first;page<=last;page++) {
            if(p.find(page)) {p.counters.hits++;continue;}
            Impl::Slot *slot=nullptr;
            for(size_t attempt=0;attempt<p.slots.size();attempt++) {
                auto &s=p.slots[p.victim++%p.slots.size()];
                if(!s.mapped || s.page<first || s.page>last) {slot=&s;break;}
            }
            if(!slot) throw std::runtime_error("No evictable residency slot");
            if(slot->mapped) {
                check(cuMemUnmap(p.address+slot->page*p.chunk,p.chunk),"evict mapping");
                slot->mapped=false;
                p.counters.evictions++;
            }
            check(cuMemMap(p.address+page*p.chunk,p.chunk,0,slot->handle,0),"map physical chunk");
            slot->page=page;slot->mapped=true;
            p.access(page,CU_MEM_ACCESS_FLAGS_PROT_READWRITE);
            auto &lane=p.lanes[p.lane_index++%2];
            check(cuEventSynchronize(lane.done),"wait staging reuse");
            LARGE_INTEGER position;position.QuadPart=(LONGLONG)(page*p.chunk);
            wincheck(SetFilePointerEx(p.file,position,nullptr,FILE_BEGIN)!=0,"seek tensor file");
            const DWORD wanted=(DWORD)std::min<uint64_t>(p.chunk,p.length-page*p.chunk);
            DWORD got=0;
            wincheck(ReadFile(p.file,lane.host,wanted,&got,nullptr)!=0,"read tensor file");
            if(got!=wanted) throw std::runtime_error("Short tensor file read");
            check(cuMemcpyHtoDAsync(p.address+page*p.chunk,lane.host,got,p.queue),"copy file chunk to CUDA");
            if(got<p.chunk)
                check(cuMemsetD8Async(p.address+page*p.chunk+got,0,p.chunk-got,p.queue),"clear final chunk padding");
            check(cuEventRecord(lane.done,p.queue),"record staging fence");
            p.counters.file_bytes+=got;p.counters.misses++;
            loaded.push_back(page);
        }
        check(cuStreamSynchronize(p.queue),"finish file transfers");
        for(auto page:loaded) p.access(page,CU_MEM_ACCESS_FLAGS_PROT_READ);
    } catch(...) {p.poisoned=true;throw;}
    return p.address+offset;
}
}
